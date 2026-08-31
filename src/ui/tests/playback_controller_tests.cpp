#include <bloom/commands/command_stack.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate)) {
            return true;
        }
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

[[nodiscard]] bloom::core::RationalTime time(const std::int64_t numerator,
                                             const std::int64_t denominator = 1) {
    const auto value = bloom::core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

// 25 fps rather than the default 24 fps (docs/architecture/animation-and-time.md's default via
// bloom::document::FrameRate::framesPerSecond24()): 25 divides 1e9 nanoseconds evenly (one frame
// == exactly 40,000,000 ns), so every elapsed value below lands exactly on or off a frame boundary
// with no fractional-nanosecond ambiguity -- the frame-math tests below need that exactness, not
// an inexact rate like frame_time_mapping_tests.cpp's own fractional-rate coverage already handles
// at the bloom::core level.
bloom::document::CompositionFormat testFormat() {
    const auto rate = bloom::document::FrameRate::create(25, 1);
    if (!rate.has_value()) {
        std::abort();
    }
    const auto format = bloom::document::CompositionFormat::create(
        4, 3, bloom::core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

bloom::document::NewProject makeTestProject(std::string projectName,
                                            const bloom::core::RationalTime duration) {
    return bloom::document::makeNewProject(std::move(projectName), "Main", duration, testFormat());
}

// Mirrors composition_preview_controller_tests.cpp's makeSecondComposition(): a second, minimal,
// layer-less composition added directly to the project (bypassing makeNewProject, which only
// builds one) so testStopsOnCompositionSwitch()/testOverflowingDurationCompositionPlaybackIsNoOp()
// have somewhere else to switch to.
bloom::document::Composition secondComposition(const bloom::core::RationalTime duration,
                                               const bloom::document::CompositionFormat format) {
    using namespace bloom::document;
    const auto compositionId = CompositionId::fromRaw(2);
    const auto stackId = NodeId::fromRaw(100);
    const auto outputId = NodeId::fromRaw(101);
    const auto edgeId = EdgeId::fromRaw(100);
    CanonicalGraph graph(stackId);
    const bool built =
        graph.addNode(
            {stackId, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}) &&
        graph.addNode({outputId,
                       std::string(kCompositionOutputNodeType),
                       {},
                       kCompositionOutputNodeSchemaVersion}) &&
        graph.addEdge({edgeId,
                       {stackId, std::string(kLayerStackOutputPort)},
                       NodeInputRef{outputId, std::string(kCompositionOutputInputPort)}});
    graph.setCompositionOutput({outputId, std::string(kCompositionOutputOutputPort)});
    if (!built) {
        std::abort();
    }
    return Composition(compositionId, "Second", duration, std::move(graph), format);
}

// A manually-advanced fake for PlaybackController::ClockFunction (design decision 1's "tests:
// manual" injectable clock) -- no dependency on real elapsed wall time. Every controller unit test
// below drives playback purely by calling advance() then tick(), never by waiting on a real timer.
struct ManualClock final {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

    void advance(const std::chrono::nanoseconds delta) { now += delta; }
};

struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             qualifiedProcessorProvider);
    }
};

// Same bundling idiom as timeline_ruler_tests.cpp's own SessionFixture: a real document, command
// stack, session, scheduler, bridge, and preview controller (offscreen). PlaybackController is
// composed with CompositionSession/CompositionPreviewController, never a mock of either (this
// task's frozen design), so every controller-unit test below still needs this full real stack.
struct SessionFixture final {
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::ui::CompositionSession session;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    bloom::ui::CompositionPreviewController controller;

    explicit SessionFixture(bloom::document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline) {}
};

// Drains the fixture's scheduler the same way every SessionFixture-based test in
// timeline_ruler_tests.cpp/composition_preview_controller_tests.cpp does, so no background worker
// thread outlives the test process.
void finishFixture(SessionFixture& fixture, Expectations& expectations) {
    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "the playback fixture reaches asynchronous scheduler quiescence");
}

// Controller test 1: play from t=0 advances session time through the exact expected frame times
// for a synthetic elapsed sequence, including a multi-frame jump that produces exactly ONE session
// advance (drop-not-slow, never catch-up-frame-by-frame).
void testPlayAdvancesExactFrameTimesAndDropsFrames(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Playback Frame Math", time(4)));
    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);

    expectations.expect(playback.state() == ui::PlaybackState::Stopped, "playback starts Stopped");

    playback.play();
    expectations.expect(playback.state() == ui::PlaybackState::Playing,
                        "play() enters the Playing state");
    expectations.expect(fixture.session.currentTime() == time(0),
                        "play() itself does not move session time before any tick");

    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "a tick at zero elapsed makes no session-time change");

    clock.advance(40'000'000ns); // exactly one 25 fps frame
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "one frame duration elapsed advances session time to exactly frame one's "
                        "exact time");

    int currentTimeChangedCount = 0;
    QObject::connect(&fixture.session, &ui::CompositionSession::currentTimeChanged,
                     [&currentTimeChangedCount] { ++currentTimeChangedCount; });
    clock.advance(200'000'000ns); // five more frames: total elapsed == six frame durations
    playback.tick();
    expectations.expect(currentTimeChangedCount == 1,
                        "a tick spanning several frame durations advances session time exactly "
                        "ONCE, to the latest frame");
    expectations.expect(fixture.session.currentTime() == time(6, 25),
                        "the single advance lands on the frame TOTAL elapsed time now demands, "
                        "not lastFrame + 1");

    playback.pause();
    finishFixture(fixture, expectations);
}

// Controller test 2: loop wrap at duration is exact -- no drift after several wraps, verified by
// exact RationalTime equality (never an approximate/epsilon comparison).
void testLoopWrapExactAfterManyWraps(Expectations& expectations) {
    using namespace bloom;
    // 4 seconds @ 25 fps == 100 frames per loop (frame indices 0..99).
    SessionFixture fixture(makeTestProject("Playback Loop Wrap", time(4)));
    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);

    playback.play();

    clock.advance(250 * 40'000'000ns); // 2.5 loops: 250 frames elapsed
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(50, 25),
                        "elapsed time past two full loops wraps to the exact mid-loop frame time");

    clock.advance(49 * 40'000'000ns); // total elapsed now 299 frames (one before the third wrap)
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(99, 25),
                        "the frame immediately before a wrap is exact");

    clock.advance(1 * 40'000'000ns); // total elapsed now exactly 300 frames: three full loops
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "landing on an exact loop boundary after three full wraps returns to "
                        "exact zero with no accumulated drift");

    playback.pause();
    finishFixture(fixture, expectations);
}

// Controller test 3: pause freezes time (a later stopped tick is a no-op), and a later play()
// resumes from the CURRENT session time -- including a time set by something other than playback
// itself while stopped -- never the original start.
void testPauseFreezesAndResumeUsesCurrentSessionTime(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Playback Pause Resume", time(4)));
    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);

    playback.play();
    clock.advance(3 * 40'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(3, 25), "advances to frame three");

    playback.pause();
    expectations.expect(playback.state() == ui::PlaybackState::Stopped, "pause() stops playback");
    expectations.expect(fixture.session.currentTime() == time(3, 25),
                        "pause freezes session time at the last applied exact time -- no "
                        "snap-back");

    clock.advance(10s);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(3, 25),
                        "a tick while stopped is a no-op regardless of elapsed clock time");

    // Something other than playback moves the session time while stopped (the honest stand-in for
    // a scrub gesture or direct time entry landing here).
    expectations.expect(fixture.session.setCurrentTime(time(50, 25)),
                        "the session time can change while playback is stopped");

    playback.play();
    clock.advance(1 * 40'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(51, 25),
                        "resuming after a pause plays forward from the CURRENT session time "
                        "(frame fifty), never the original start (frame zero) or the old pause "
                        "time (frame three)");

    playback.pause();
    finishFixture(fixture, expectations);
}

// Design decision 3's reported rule: a scrub gesture PAUSES playback. Simulated here the honest
// way -- calling the exact CompositionSession::setCurrentTime() mutator TimelineRuler's own scrub
// gesture calls -- rather than constructing a full TimelineRuler widget and synthesizing pointer
// events, since only the session-time-mutator boundary matters to this rule.
void testScrubDuringPlaybackPauses(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Playback Scrub Pause", time(4)));
    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);

    playback.play();
    clock.advance(2 * 40'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(2, 25), "advances to frame two");
    expectations.expect(playback.state() == ui::PlaybackState::Playing, "still playing");

    expectations.expect(fixture.session.setCurrentTime(time(10, 25)),
                        "a scrub-shaped setCurrentTime() call succeeds mid-playback");
    expectations.expect(playback.state() == ui::PlaybackState::Stopped,
                        "a session-time change PlaybackController did not itself make pauses "
                        "playback");
    expectations.expect(fixture.session.currentTime() == time(10, 25),
                        "the scrub's own target time is left exactly as the scrub set it -- "
                        "playback does not fight or revert it");

    finishFixture(fixture, expectations);
}

// If the composition/session changes (composition switch here; CompositionSession::rebind() --
// the document-close/open path -- emits the SAME compositionChanged() signal, verified by reading
// composition_session.cpp, so one connection covers both), playback stops.
void testStopsOnCompositionSwitch(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Playback Composition Switch", time(4));
    const auto firstCompositionId = newProject.initialCompositionId;
    expectations.expect(newProject.project.addComposition(secondComposition(time(4), testFormat())),
                        "a second composition is added to the project");
    SessionFixture fixture(std::move(newProject));

    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);

    playback.play();
    clock.advance(40'000'000ns);
    playback.tick();
    expectations.expect(playback.state() == ui::PlaybackState::Playing, "playing normally");

    expectations.expect(fixture.session.setComposition(document::CompositionId::fromRaw(2)),
                        "switching composition succeeds");
    expectations.expect(playback.state() == ui::PlaybackState::Stopped,
                        "a composition switch stops playback");

    // Switching back does not resurrect the old Playing state on its own.
    expectations.expect(fixture.session.setComposition(firstCompositionId),
                        "switching back succeeds");
    expectations.expect(playback.state() == ui::PlaybackState::Stopped,
                        "playback stays stopped after switching composition again");

    finishFixture(fixture, expectations);
}

// Design decision 5: playback of an empty/zero-duration composition is a no-op (typed/guarded).
// Deviation worth reporting: a genuinely zero-duration composition cannot be reached through any
// real bloom::document::Document -- document::Project::validate() rejects duration <= 0
// (project.cpp), and Document's constructor throws on an invalid project (document.cpp), so the
// zero-duration branch is unreachable via live session state today (defensive-only, matching
// FrameTimeMapping's own "checked, not merely assumed" philosophy). This instead exercises the
// SAME guard -- PlaybackController::play() refusing when bloom::core::FrameTimeMapping::create()
// itself refuses -- through a duration/frame-rate pair that DOES pass document validation
// (duration is a large but positive RationalTime) yet still overflows FrameTimeMapping's checked
// frame-index range, the identical extreme pairing frame_time_mapping_tests.cpp's own
// testRateNormalizationAndExtremeArithmetic() already proves FrameTimeMapping::create() refuses.
void testOverflowingDurationCompositionPlaybackIsNoOp(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Playback Overflow Guard", time(4));
    const auto overflowRateOptional =
        document::FrameRate::create(std::numeric_limits<std::uint32_t>::max(), 1);
    if (!overflowRateOptional.has_value()) {
        std::abort();
    }
    const auto overflowRate = *overflowRateOptional;
    const auto overflowFormatOptional =
        document::CompositionFormat::create(4, 3, core::PixelAspectRatio::square(), overflowRate);
    if (!overflowFormatOptional.has_value()) {
        std::abort();
    }
    const auto overflowFormat = *overflowFormatOptional;
    const auto overflowDuration = time(std::numeric_limits<std::int64_t>::max());
    const auto overflowCompositionId = document::CompositionId::fromRaw(2);
    expectations.expect(
        newProject.project.addComposition(secondComposition(overflowDuration, overflowFormat)),
        "a large-but-positive-duration, extreme-rate composition is added to the project -- "
        "document validation only requires duration > 0, so this passes it");

    SessionFixture fixture(std::move(newProject));
    expectations.expect(fixture.session.setComposition(overflowCompositionId),
                        "switching to the overflow-guard composition succeeds");
    expectations.expect(
        core::FrameTimeMapping::create(overflowDuration, overflowRate.numerator(),
                                       overflowRate.denominator())
                .hasValue() == false,
        "this duration/rate pair really does overflow FrameTimeMapping's checked frame-index "
        "range");

    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);
    playback.play();
    expectations.expect(playback.state() == ui::PlaybackState::Stopped,
                        "play() guards against an unrepresentable FrameTimeMapping instead of "
                        "constructing one anyway (design decision 5's guarded no-op, exercised "
                        "at its checked-arithmetic boundary rather than via an unreachable "
                        "zero-duration document state)");

    finishFixture(fixture, expectations);
}

// Integration test: with the real CompositionPreviewController, playing produces Interactive-kind
// requests honoring the one-active/newest-pending gate -- asserted via the controller's existing
// observable seams (runtime::TaskScheduler::snapshots(), each TaskSnapshot's sourceVersion/
// priority), with NO gate modification, mirroring composition_preview_controller_tests.cpp's own
// testNewestPendingRequestGate()/testInteractiveCadenceCoalescesBurstAndVisibleBypasses() idiom.
struct WorkerGate final {
    void enterAndWait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

std::optional<bloom::runtime::TaskSnapshot>
snapshotForGeneration(const bloom::runtime::TaskScheduler& scheduler,
                      const std::uint64_t generation) {
    for (const auto& snapshot : scheduler.snapshots()) {
        if (snapshot.sourceVersion.requestGeneration == generation) {
            return snapshot;
        }
    }
    return std::nullopt;
}

void testIntegrationPlaybackDrivesInteractivePriorityUnderGate(Expectations& expectations) {
    using namespace bloom;
    auto newProject = makeTestProject("Playback Interactive Gate", time(4));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    PipelineFixture fixture;
    WorkerGate firstRequest;
    std::atomic<int> invocationCount = 0;
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&firstRequest, &invocationCount, pipeline = fixture.pipeline](
            const document::Snapshot& snapshot,
            const runtime::PreviewRequestIdentity& desiredIdentity,
            const std::size_t pixelStorageByteLimit,
            const std::optional<runtime::SnapshotParameterOverride>& interactionOverride,
            runtime::TaskContext& context) mutable {
            if (invocationCount.fetch_add(1) == 0) {
                firstRequest.enterAndWait();
            }
            return pipeline(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride,
                            context);
        });

    expectations.expect(waitUntil([&] { return firstRequest.entered(); }),
                        "the controller's own initial preview request enters preparation and "
                        "gates as the active request");
    expectations.expect(scheduler.snapshots().size() == 1,
                        "exactly the initial (Visible) request has been submitted so far");

    ManualClock clock;
    ui::PlaybackController playback(session, controller, [&clock] { return clock.now; }, 16ms);
    playback.play();
    clock.advance(40'000'000ns); // one 25 fps frame
    playback.tick();
    const auto tickGeneration = controller.state().desiredIdentity.has_value()
                                    ? controller.state().desiredIdentity->requestGeneration
                                    : 0;
    expectations.expect(tickGeneration != 0, "the tick's own request identity is observable");
    expectations.expect(scheduler.snapshots().size() == 1,
                        "the tick's request is held as the newest pending request behind the "
                        "still-active initial request -- the gate is not bypassed");

    firstRequest.release();
    expectations.expect(
        waitUntil([&] { return scheduler.snapshots().size() == 2; }),
        "the pending request submits once the active initial request reaches terminal");
    const auto tickSnapshot = snapshotForGeneration(scheduler, tickGeneration);
    expectations.expect(tickSnapshot.has_value() &&
                            tickSnapshot->priority == runtime::TaskPriority::Interactive,
                        "playback's own session-time change submits at Interactive priority, "
                        "through the SAME arming CompositionPreviewController::"
                        "beginInteractiveScrub() already grants scrub -- no new request kind");

    playback.pause();
    controller.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the interactive-gate fixture reaches asynchronous scheduler quiescence");
}

// Widget test: the play/pause toggle button and Space application shortcut flip transport state
// offscreen; Space while a QLineEdit has focus does NOT toggle. TimelineEditor itself owns no
// QLineEdit (verified by reading composition_editors.cpp), so this builds the closest honest
// fixture: a plain QLineEdit as a sibling of the real TimelineEditor inside one shown top-level
// window, so Qt::WindowShortcut's real per-window dispatch (not a synthetic focus check) decides
// whether Space reaches the action.
void testPlaybackToggleButtonAndSpaceShortcut(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Playback Widget", time(4)));

    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller, &host);
    auto* probeLineEdit = new QLineEdit(&host);
    probeLineEdit->setObjectName("playbackTestProbeLineEdit");
    layout->addWidget(editor);
    layout->addWidget(probeLineEdit);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    auto* button = editor->findChild<QToolButton*>("playPauseButton");
    expectations.expect(button != nullptr, "the play/pause toggle button is reachable by name");
    if (button == nullptr) {
        finishFixture(fixture, expectations);
        return;
    }
    expectations.expect(!button->isChecked() && button->text() == QStringLiteral("Play"),
                        "the button starts in the Stopped/Play visual state");

    button->click();
    expectations.expect(button->isChecked() && button->text() == QStringLiteral("Pause"),
                        "clicking the button toggles to the Playing/Pause visual state");
    button->click();
    expectations.expect(!button->isChecked() && button->text() == QStringLiteral("Play"),
                        "clicking it again toggles back to Stopped/Play");

    editor->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(editor, Qt::Key_Space);
    QCoreApplication::processEvents();
    expectations.expect(button->isChecked() && button->text() == QStringLiteral("Pause"),
                        "the Space application shortcut toggles playback when no text-entry "
                        "widget has focus");

    probeLineEdit->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(QApplication::focusWidget() == probeLineEdit,
                        "the probe line edit genuinely holds keyboard focus");
    QTest::keyClick(probeLineEdit, Qt::Key_Space);
    QCoreApplication::processEvents();
    expectations.expect(button->isChecked() && button->text() == QStringLiteral("Pause"),
                        "Space is NOT stolen from a focused text-entry widget: playback state is "
                        "unchanged by this keystroke");
    expectations.expect(probeLineEdit->text() == QStringLiteral(" "),
                        "the focused line edit consumed Space as ordinary text input, confirming "
                        "it -- not a dropped/ignored event -- is what won the key");

    finishFixture(fixture, expectations);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testPlayAdvancesExactFrameTimesAndDropsFrames(expectations);
    testLoopWrapExactAfterManyWraps(expectations);
    testPauseFreezesAndResumeUsesCurrentSessionTime(expectations);
    testScrubDuringPlaybackPauses(expectations);
    testStopsOnCompositionSwitch(expectations);
    testOverflowingDurationCompositionPlaybackIsNoOp(expectations);
    testIntegrationPlaybackDrivesInteractivePriorityUnderGate(expectations);
    testPlaybackToggleButtonAndSpaceShortcut(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
