#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
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
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTest>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>

namespace bloom::ui {
struct CompositionPreviewDeadlineTestAccess final {
    static std::optional<std::chrono::steady_clock::time_point>
    active(const CompositionPreviewController& controller) {
        return controller.active_ ? controller.active_->playbackDeadline : std::nullopt;
    }
    static std::optional<std::chrono::steady_clock::time_point>
    pending(const CompositionPreviewController& controller) {
        return controller.pending_ ? controller.pending_->playbackDeadline : std::nullopt;
    }
};
} // namespace bloom::ui

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
bloom::document::CompositionFormat testFormat(const std::uint32_t p = 25,
                                              const std::uint32_t q = 1) {
    const auto rate = bloom::document::FrameRate::create(p, q);
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

    explicit SessionFixture(bloom::document::NewProject newProject,
                            const std::chrono::milliseconds monitorInterval = 1ms)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, monitorInterval),
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
void testWorkAreaLoop(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Work area loop", time(4)));
    commands::Transaction range("Work area", fixture.session.snapshot().revision());
    range.emplace<commands::SetWorkArea>(fixture.session.compositionId(), time(1), time(2));
    expectations.expect(fixture.session.executeTransaction(std::move(range)).changed(),
                        "work area accepted");
    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);
    playback.play();
    expectations.expect(fixture.session.currentTime() == time(1),
                        "out-of-range playback starts at work-area start");
    clock.advance(960'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(49, 25),
                        "work-area final frame included");
    clock.advance(40'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1),
                        "exclusive end wraps to nonzero work-area start");
    playback.pause();
    finishFixture(fixture, expectations);
}

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
// Real cached products affect preparation only; late/repeated ticks retain the same real-time grid.
void testCachedFramesUseElapsedClock(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Cached Playback Clock", time(4)));
    for (const auto index : {5, 6, 12, 0}) {
        (void)fixture.session.setCurrentTime(time(index, 25));
        expectations.expect(waitUntil([&] {
                                return fixture.controller.state().freshness ==
                                       ui::FrameFreshness::Current;
                            }),
                            "seed genuine cached exact-time product");
    }
    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);
    playback.play();
    clock.advance(200'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(5, 25) &&
                            fixture.controller.state().freshness == ui::FrameFreshness::Current &&
                            !fixture.controller.state().taskId &&
                            fixture.controller.droppedFrameCount() == 4,
                        "late warm tick jumps directly to frame five without evaluation");
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(5, 25) &&
                            fixture.controller.droppedFrameCount() == 4,
                        "repeated tick at unchanged elapsed time never pays cached frame debt");
    clock.advance(40'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(6, 25),
                        "next cached boundary is exact");
    clock.advance(200'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(11, 25) &&
                            fixture.controller.droppedFrameCount() == 8,
                        "late cold tick uses the same elapsed target and skipped-frame accounting");
    clock.advance(40'000'000ns);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(12, 25) &&
                            fixture.controller.state().freshness == ui::FrameFreshness::Current,
                        "warm transition never selects a different clock");
    playback.pause();
    finishFixture(fixture, expectations);
}

void testTransportRationalRateBoundaries(Expectations& expectations) {
    using namespace bloom;
    constexpr std::array rates{std::pair{24U, 1U},       std::pair{25U, 1U},
                               std::pair{30U, 1U},       std::pair{60U, 1U},
                               std::pair{24000U, 1001U}, std::pair{30000U, 1001U},
                               std::pair{60000U, 1001U}};
    constexpr auto ns = std::uint64_t{1'000'000'000};
    for (const auto& [p, q] : rates) {
        SessionFixture fixture(
            document::makeNewProject("Rational transport", "Main", time(4), testFormat(p, q)));
        ManualClock clock;
        const auto origin = clock.now;
        ui::PlaybackController playback(fixture.session, fixture.controller,
                                        [&clock] { return clock.now; });
        playback.play();
        clock.advance(200ms);
        playback.tick();
        const auto frame = 200'000'000ULL * p / (ns * q);
        const auto expected = time(static_cast<std::int64_t>(frame * q), p);
        expectations.expect(fixture.session.currentTime() == expected,
                            "late cold tick lands on exact elapsed target at every required rate");
        playback.tick();
        expectations.expect(fixture.session.currentTime() == expected,
                            "identical clock reading never advances frame at any rate");
        const auto boundary = ((frame + 1) * ns * q + p - 1) / p;
        clock.now = origin + std::chrono::nanoseconds{boundary - 1};
        playback.tick();
        expectations.expect(fixture.session.currentTime() == expected,
                            "fractional due boundary does not fire one nanosecond early");
        clock.now = origin + std::chrono::nanoseconds{boundary};
        playback.tick();
        expectations.expect(fixture.session.currentTime() ==
                                time(static_cast<std::int64_t>((frame + 1) * q), p),
                            "first whole nanosecond after due boundary lands exactly");
        playback.pause();
        finishFixture(fixture, expectations);
    }
}

void testAudioMasterFloorLoopAndDisableReanchor(Expectations& expectations) {
    using namespace bloom;
    namespace audio = media::audio::playback;
    SessionFixture fixture(makeTestProject("Audio master", time(2)));
    ManualClock clock;
    ui::PlaybackController playback(fixture.session, fixture.controller,
                                    [&clock] { return clock.now; });
    auto owned = std::make_unique<audio::AudioEngine>(std::make_unique<audio::NullBackend>(),
                                                      audio::AudioEngine::Config{1000, 2, 4096});
    auto* engine = owned.get();
    playback.setAudioEngine(std::move(owned));
    runtime::AudioMixDescription mix;
    mix.clips.push_back(
        {document::NodeId::fromRaw(1), document::AssetId::fromRaw(1), time(0), time(10)});
    playback.setAudioMix(std::move(mix), {audio::AudioClip{nullptr, time(0)}});
    playback.play();
    clock.advance(20s);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "written audio frames, not advanced wall time, are the master");
    expectations.expect(!engine->renderForTesting(999), "silent NullBackend advances audio clock");
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(24, 25),
                        "audio just before frame boundary floors instead of nearest rounding");
    expectations.expect(!engine->renderForTesting(1), "audio reaches exact boundary");
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1),
                        "audio exact boundary is frame25");
    expectations.expect(!engine->renderForTesting(2200), "audio continues beyond composition end");
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(30, 25),
                        "unclamped audio frame offset keeps exact loop progress beyond duration");
    playback.setLooping(false);
    expectations.expect(!engine->renderForTesting(1000), "audio nonloop end advances");
    playback.tick();
    expectations.expect(playback.state() == ui::PlaybackState::Stopped &&
                            fixture.session.currentTime() == time(49, 25),
                        "nonloop audio stops exactly on work-area last frame");
    playback.setLooping(true);
    playback.play();
    expectations.expect(!engine->renderForTesting(80), "audio resumes from last-frame origin");
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "resumed audio loops exactly");
    clock.advance(5s);
    playback.tick();
    playback.setAudioEnabled(false);
    clock.advance(39ms);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "audio disable anchors wall clock to current frame without old-clock jump");
    clock.advance(1ms);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(2, 25),
                        "first full period after disabling audio advances exactly one frame");
    playback.pause();
    finishFixture(fixture, expectations);
}

void testSharedSubframeOriginAndNonOneAudioRates(Expectations& expectations) {
    using namespace bloom;
    namespace audio = media::audio::playback;
    for (const auto origin : {time(1, 100), time(1, 50), time(3, 100)}) {
        for (const bool useAudio : {false, true}) {
            SessionFixture fixture(makeTestProject("Subframe origin", time(4)));
            (void)fixture.session.setCurrentTime(origin);
            ManualClock clock;
            ui::PlaybackController playback(fixture.session, fixture.controller,
                                            [&clock] { return clock.now; });
            audio::AudioEngine* engine = nullptr;
            if (useAudio) {
                auto owned =
                    std::make_unique<audio::AudioEngine>(std::make_unique<audio::NullBackend>(),
                                                         audio::AudioEngine::Config{1000, 2, 4096});
                engine = owned.get();
                playback.setAudioEngine(std::move(owned));
                runtime::AudioMixDescription mix;
                mix.clips.push_back({document::NodeId::fromRaw(1), document::AssetId::fromRaw(1),
                                     time(0), time(10)});
                playback.setAudioMix(std::move(mix), {audio::AudioClip{nullptr, time(0)}});
            }
            playback.play();
            expectations.expect(fixture.session.currentTime() == origin,
                                "play preserves immediate exact subframe session time");
            clock.advance(39ms);
            if (engine)
                expectations.expect(!engine->renderForTesting(39), "silent audio39ms");
            playback.tick();
            expectations.expect(fixture.session.currentTime() == origin,
                                "shared nearest-grid origin advances only after full period");
            clock.advance(1ms);
            if (engine)
                expectations.expect(!engine->renderForTesting(1), "silent audio boundary");
            playback.tick();
            const auto nearest = origin < time(1, 50) ? 0 : 1;
            expectations.expect(
                fixture.session.currentTime() == time(nearest + 1, 25),
                "wall/audio subframe and greater halfway tie agree at first boundary");
            playback.pause();
            (void)fixture.session.setCurrentTime(origin);
            playback.play();
            clock.advance(40ms);
            if (engine)
                expectations.expect(!engine->renderForTesting(40), "silent audio resume");
            playback.tick();
            expectations.expect(fixture.session.currentTime() == time(nearest + 1, 25),
                                "pause/seek/resume restores the same shared origin convention");
            playback.pause();
            finishFixture(fixture, expectations);
        }
    }
    for (const auto speed : {2.0, 0.5}) {
        SessionFixture fixture(makeTestProject("Non-one audio", time(4)));
        expectations.expect(waitUntil([&] {
                                return fixture.controller.state().freshness ==
                                       ui::FrameFreshness::Current;
                            }),
                            "non-one fixture opening is ready");
        ManualClock clock;
        ui::PlaybackController playback(fixture.session, fixture.controller,
                                        [&clock] { return clock.now; });
        auto owned = std::make_unique<audio::AudioEngine>(
            std::make_unique<audio::NullBackend>(), audio::AudioEngine::Config{1000, 2, 4096});
        auto* engine = owned.get();
        expectations.expect(engine->setRate(speed), "existing non-one audio speed is supported");
        playback.setAudioEngine(std::move(owned));
        runtime::AudioMixDescription mix;
        mix.clips.push_back(
            {document::NodeId::fromRaw(1), document::AssetId::fromRaw(1), time(0), time(10)});
        playback.setAudioMix(std::move(mix), {audio::AudioClip{nullptr, time(0)}});
        playback.play();
        const auto wallFrames = speed == 2.0 ? std::size_t{20} : std::size_t{80};
        const auto budget = speed == 2.0 ? 20ms : 80ms;
        expectations.expect(!engine->renderForTesting(wallFrames - 1),
                            "non-one audio before boundary");
        playback.tick();
        expectations.expect(fixture.session.currentTime() == time(0),
                            "non-one audio does not run early");
        expectations.expect(!engine->renderForTesting(1), "non-one audio exact boundary");
        const auto before = std::chrono::steady_clock::now();
        playback.tick();
        const auto after = std::chrono::steady_clock::now();
        const auto deadline = ui::CompositionPreviewDeadlineTestAccess::active(fixture.controller);
        expectations.expect(
            fixture.session.currentTime() == time(1, 25) && deadline &&
                *deadline >= before + budget && *deadline <= after + budget,
            "supported audio rate scales target and accounting deadline to wall time");
        commands::Transaction edit("Audio live rate", fixture.session.snapshot().revision());
        edit.emplace<commands::SetCompositionFormat>(fixture.session.compositionId(),
                                                     testFormat(60));
        const auto editBefore = std::chrono::steady_clock::now();
        expectations.expect(fixture.session.executeTransaction(std::move(edit)).changed(),
                            "audio live frame-rate edit accepted");
        const auto editAfter = std::chrono::steady_clock::now();
        auto reanchoredDeadline =
            ui::CompositionPreviewDeadlineTestAccess::pending(fixture.controller);
        if (!reanchoredDeadline)
            reanchoredDeadline =
                ui::CompositionPreviewDeadlineTestAccess::active(fixture.controller);
        const auto reanchoredBudget = speed == 2.0 ? std::chrono::nanoseconds{8'333'334}
                                                   : std::chrono::nanoseconds{33'333'334};
        expectations.expect(fixture.session.currentTime() == time(2, 60) && reanchoredDeadline &&
                                *reanchoredDeadline >= editBefore + reanchoredBudget &&
                                *reanchoredDeadline <= editAfter + reanchoredBudget,
                            "live rate reanchor preserves non-one audio wall deadline");
        playback.pause();
        finishFixture(fixture, expectations);
    }
}

void testRateChangesEmptyGridAndInvalidClock(Expectations& expectations) {
    using namespace bloom;
    for (const auto newRate : {24U, 60U}) {
        SessionFixture fixture(makeTestProject("Live rate", time(4)));
        ManualClock clock;
        ui::PlaybackController playback(fixture.session, fixture.controller,
                                        [&clock] { return clock.now; });
        playback.play();
        clock.advance(400ms);
        playback.tick();
        commands::Transaction edit("Frame rate", fixture.session.snapshot().revision());
        edit.emplace<commands::SetCompositionFormat>(fixture.session.compositionId(),
                                                     testFormat(newRate));
        expectations.expect(fixture.session.executeTransaction(std::move(edit)).changed(),
                            "live frame-rate command accepted");
        const auto anchor = newRate == 24 ? 10 : 24;
        expectations.expect(playback.state() == ui::PlaybackState::Playing &&
                                fixture.session.currentTime() == time(anchor, newRate),
                            "live rate reanchors the current nearest grid rather than old offset");
        clock.advance(std::chrono::nanoseconds{(1'000'000'000ULL + newRate - 1) / newRate});
        playback.tick();
        expectations.expect(fixture.session.currentTime() == time(anchor + 1, newRate),
                            "new-rate rational period advances without freeze or clock debt");
        playback.pause();
        finishFixture(fixture, expectations);
    }
    for (const auto bounds : {std::pair{35, 36}, std::pair{10, 20}}) {
        SessionFixture fixture(
            document::makeNewProject("Grid disappears", "Main", time(4), testFormat(1000)));
        commands::Transaction area("Narrow work area", fixture.session.snapshot().revision());
        area.emplace<commands::SetWorkArea>(fixture.session.compositionId(),
                                            time(bounds.first, 1000), time(bounds.second, 1000));
        expectations.expect(fixture.session.executeTransaction(std::move(area)).changed(),
                            "valid1000fps area");
        ManualClock clock;
        ui::PlaybackController playback(fixture.session, fixture.controller,
                                        [&clock] { return clock.now; });
        playback.play();
        commands::Transaction rate("Remove grid", fixture.session.snapshot().revision());
        rate.emplace<commands::SetCompositionFormat>(fixture.session.compositionId(),
                                                     testFormat(25));
        expectations.expect(fixture.session.executeTransaction(std::move(rate)).changed(),
                            "valid format leaves empty grid");
        expectations.expect(
            playback.state() == ui::PlaybackState::Stopped,
            "range with no valid new-rate frame pauses instead of reanchoring forever");
        playback.play();
        expectations.expect(playback.state() == ui::PlaybackState::Stopped,
                            "a new play also refuses the empty valid-grid range");
        finishFixture(fixture, expectations);
    }
    {
        SessionFixture fixture(
            document::makeNewProject("Off-grid valid range", "Main", time(4), testFormat(1000)));
        commands::Transaction area("Work area", fixture.session.snapshot().revision());
        area.emplace<commands::SetWorkArea>(fixture.session.compositionId(), time(10, 1000),
                                            time(90, 1000));
        expectations.expect(fixture.session.executeTransaction(std::move(area)).changed(),
                            "valid authored range is accepted");
        ManualClock clock;
        ui::PlaybackController playback(fixture.session, fixture.controller,
                                        [&clock] { return clock.now; });
        playback.play();
        commands::Transaction rate("Off-grid endpoints", fixture.session.snapshot().revision());
        rate.emplace<commands::SetCompositionFormat>(fixture.session.compositionId(),
                                                     testFormat(25));
        expectations.expect(fixture.session.executeTransaction(std::move(rate)).changed(),
                            "supported rate edit makes endpoints off-grid");
        expectations.expect(fixture.session.currentTime() == time(1, 25),
                            "reanchor chooses first valid frame instead of preceding frame zero");
        clock.advance(40ms);
        playback.tick();
        expectations.expect(fixture.session.currentTime() == time(2, 25),
                            "nonempty off-grid range advances to its last valid frame");
        clock.advance(40ms);
        playback.tick();
        expectations.expect(fixture.session.currentTime() == time(1, 25),
                            "off-grid loop excludes both preceding frame and exclusive end");
        playback.pause();
        (void)fixture.session.setCurrentTime(time(11, 1000));
        playback.play();
        expectations.expect(fixture.session.currentTime() == time(11, 1000),
                            "inside authored interval preserves immediate stored subframe time");
        clock.advance(40ms);
        playback.tick();
        expectations.expect(
            fixture.session.currentTime() == time(2, 25),
            "play origin clamps to valid grid even when alignment exceeds half-frame");
        playback.pause();
        finishFixture(fixture, expectations);
    }
    {
        SessionFixture fixture(makeTestProject("Extreme clock", time(4)));
        ManualClock clock;
        clock.now = std::chrono::steady_clock::time_point::min();
        ui::PlaybackController playback(fixture.session, fixture.controller,
                                        [&clock] { return clock.now; });
        playback.play();
        clock.now = std::chrono::steady_clock::time_point::max();
        playback.tick();
        expectations.expect(fixture.session.currentTime() == time(0),
                            "extreme difference is refused before signed subtraction");
        clock.now = std::chrono::steady_clock::time_point::min() + 40ms;
        playback.tick();
        expectations.expect(fixture.session.currentTime() == time(1, 25),
                            "valid clock recovery retains fixed origin");
        playback.pause();
        bool refused = false;
        try {
            ui::PlaybackController invalid(fixture.session, fixture.controller,
                                           ui::PlaybackController::ClockFunction{});
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        expectations.expect(refused, "empty injected clock rejected during construction");
        finishFixture(fixture, expectations);
    }
}

void testLiveWorkAreaEditsReanchorSafely(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Range edit clock", time(4)));
    ManualClock clock;
    ui::PlaybackController playback(fixture.session, fixture.controller,
                                    [&clock] { return clock.now; });
    playback.play();
    clock.advance(1200ms);
    playback.tick();
    const auto setArea = [&](const core::RationalTime start, const core::RationalTime end) {
        commands::Transaction edit("Work area", fixture.session.snapshot().revision());
        edit.emplace<commands::SetWorkArea>(fixture.session.compositionId(), start, end);
        expectations.expect(fixture.session.executeTransaction(std::move(edit)).changed(),
                            "live work-area edit is accepted");
    };
    setArea(time(2), time(3));
    expectations.expect(playback.state() == ui::PlaybackState::Playing &&
                            fixture.session.currentTime() == time(2),
                        "shift beyond old origin reanchors without unsigned offset underflow");
    clock.advance(40ms);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(51, 25), "shifted range progresses");
    setArea(time(1), time(3));
    clock.advance(40ms);
    playback.tick();
    expectations.expect(fixture.session.currentTime() == time(52, 25),
                        "range expansion preserves current origin without accumulated clock debt");
    playback.setLooping(false);
    setArea(time(1), time(53, 25));
    clock.advance(200ms);
    playback.tick();
    expectations.expect(playback.state() == ui::PlaybackState::Stopped &&
                            fixture.session.currentTime() == time(52, 25),
                        "trimmed nonloop range stops on exclusive-end predecessor");
    finishFixture(fixture, expectations);
}

// A cold, uncached range whose measured preparation does not fit the tick must still make progress
// when the preview controller is idle. The estimate is the maximum observed duration and is only
// refreshed by a successful completion, so a hard estimate gate with no idle escape starves every
// Playback tick forever: the playhead moves (tick() writes session time first) while the viewer
// keeps the previous frame. This is the admission-starvation regression.
void testSlowPlaybackStillSubmitsWhenIdle(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Slow Playback Admission", time(4)));
    expectations.expect(waitUntil([&] {
                            return fixture.controller.state().activity !=
                                   ui::PreviewActivity::Rendering;
                        }),
                        "the initial preview reaches a terminal activity before playback");

    const auto identity = fixture.controller.state().desiredIdentity;
    expectations.expect(identity.has_value(), "the initial preview published its identity");
    if (identity.has_value()) {
        const auto key = fixture.controller.cacheKeyForTime(identity->time);
        expectations.expect(key.has_value() &&
                                *key == ui::PreviewFrameCacheKey::forIdentity(*identity),
                            "the regression identity is one recordPreparationDuration accepts");
        // A measured preparation that cannot fit the 16 ms tick (half-tick threshold is 8 ms).
        fixture.controller.recordPreparationDuration(*identity, 40ms);
    }

    ManualClock clock;
    ui::PlaybackController playback(
        fixture.session, fixture.controller, [&clock] { return clock.now; }, 16ms);
    playback.play();

    clock.advance(40'000'000ns);
    playback.tick();
    const auto first = fixture.controller.state();
    expectations.expect(first.activity == ui::PreviewActivity::Rendering,
                        "a slow idle playback tick still submits a frame instead of starving");

    expectations.expect(waitUntil([&] {
                            return fixture.controller.state().activity !=
                                   ui::PreviewActivity::Rendering;
                        }),
                        "the slow playback frame reaches a terminal activity");

    // Slow but admitted: the cold frame still enters the RAM cache, so a later pass over the range
    // becomes cached hits rather than starving again.
    if (first.desiredIdentity.has_value()) {
        const auto firstKey = fixture.controller.cacheKeyForTime(first.desiredIdentity->time);
        expectations.expect(firstKey.has_value() &&
                                fixture.controller.frameCache().contains(*firstKey),
                            "an admitted slow playback frame fills the RAM cache");
    }

    clock.advance(40'000'000ns);
    playback.tick();
    const auto second = fixture.controller.state();
    expectations.expect(second.activity == ui::PreviewActivity::Rendering,
                        "slow non-cached playback keeps progressing on later idle ticks");
    expectations.expect(first.desiredIdentity.has_value() && second.desiredIdentity.has_value() &&
                            first.desiredIdentity->time != second.desiredIdentity->time &&
                            second.desiredIdentity->sourceRevision ==
                                fixture.session.snapshot().revision(),
                        "successive playback requests advance time and carry the live revision");

    playback.pause();
    finishFixture(fixture, expectations);
}

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

    playback.play();
    fixture.controller.beginInteractiveScrub();
    expectations.expect(playback.state() == ui::PlaybackState::Stopped,
                        "gesture press pauses playback before its first time change");
    (void)fixture.session.setCurrentTime(time(1, 25));
    fixture.controller.notifyScrubEnded();
    expectations.expect(fixture.controller.state().desiredIdentity.has_value() &&
                            fixture.controller.state().desiredIdentity->time == time(1, 25),
                        "the first scrub frame still requests a preview");

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

// Hold initial work on the worker: playback keeps one newest intent behind its retirement gate.
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

void testPendingDeadlineRetainsIntentAndCountsLateOnce(Expectations& expectations) {
    using namespace bloom;
    auto project = makeTestProject("Captured deadline", time(4));
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler); // Production100ms progress; completion drives delivery.
    PipelineFixture pipeline;
    WorkerGate first, second;
    auto prepare = [&](const document::Snapshot& snapshot,
                       const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                       const std::vector<runtime::SnapshotParameterOverride>& override,
                       runtime::TaskContext& context) {
        if (identity.time == time(1, 25))
            first.enterAndWait();
        if (identity.time == time(2, 25))
            second.enterAndWait();
        return pipeline.pipeline(snapshot, identity, limit, override, context);
    };
    ui::CompositionPreviewController controller(session, scheduler, bridge, prepare);
    expectations.expect(
        waitUntil([&] { return controller.state().freshness == ui::FrameFreshness::Current; }),
        "deadline fixture opening frame is ready");
    ManualClock clock;
    ui::PlaybackController playback(session, controller, [&clock] { return clock.now; });
    playback.play();
    clock.advance(40ms);
    const auto before = std::chrono::steady_clock::now();
    playback.tick();
    const auto after = std::chrono::steady_clock::now();
    const auto active = ui::CompositionPreviewDeadlineTestAccess::active(controller);
    expectations.expect(active && *active >= before + 40ms && *active <= after + 40ms,
                        "25fps intent has40ms rational budget independent of16ms wake cap");
    expectations.expect(waitUntil([&] { return first.entered(); }), "first producer held");
    clock.advance(40ms);
    playback.tick();
    const auto captured = ui::CompositionPreviewDeadlineTestAccess::pending(controller);
    expectations.expect(captured.has_value(), "newest pending intent captures absolute deadline");
    expectations.expect(captured &&
                            waitUntil([&] { return std::chrono::steady_clock::now() > *captured; }),
                        "held producer lasts beyond actual rational frame deadline without sleeps");
    first.release();
    expectations.expect(waitUntil([&] { return second.entered(); }),
                        "completion admits newest pending producer once");
    expectations.expect(ui::CompositionPreviewDeadlineTestAccess::active(controller) == captured,
                        "pending deadline survives queue retirement without rebasing at submit");
    second.release();
    expectations.expect(waitUntil([&] {
                            return controller.state().freshness == ui::FrameFreshness::Current &&
                                   controller.state().frame->desiredIdentity().time == time(2, 25);
                        }),
                        "late clean current product remains useful while deadline is accounting");
    expectations.expect(controller.droppedFrameCount() == 2,
                        "superseded first ask and late second delivery each count exactly once");
    bridge.wake();
    QCoreApplication::processEvents();
    expectations.expect(controller.droppedFrameCount() == 2, "extra drain cannot count late twice");
    playback.pause();
    controller.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "deadline fixture drains");
}

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

void testIntegrationPlaybackCoalescesWhileForegroundGateIsBusy(Expectations& expectations) {
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
            const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
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
                        "the tick keeps the busy foreground admission gate closed");

    firstRequest.release();
    expectations.expect(
        waitUntil([&] {
            return controller.state().freshness == ui::FrameFreshness::Current &&
                   controller.state().frame &&
                   controller.state().frame->desiredIdentity().requestGeneration == tickGeneration;
        }),
        "the one newest playback intent is delivered after cancelled foreground work drains");
    const auto tickSnapshot = snapshotForGeneration(scheduler, tickGeneration);
    expectations.expect(tickSnapshot.has_value() && invocationCount.load() == 2 &&
                            scheduler.isQuiescent(),
                        "only the original producer and one newest playback request are admitted");
    expectations.expect(
        controller.droppedFrameCount() >= 1 && controller.droppedFrameCount() <= 2,
        "the superseded foreground frame is dropped once; late playback may add one");

    playback.pause();
    controller.beginShutdown();
    bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                        "the interactive-gate fixture reaches asynchronous scheduler quiescence");
}

// Widget test: the play/pause toggle button and Space application shortcut flip transport state
// offscreen; Space while a QLineEdit has focus does NOT toggle. A plain QLineEdit sits as a sibling
// of the real editor inside one shown top-level window, so Qt::WindowShortcut's real per-window
// dispatch (not a synthetic focus check) decides whether Space reaches the action.
//
// ADAPTED for task VIEW-1 (enumerated in that task's report): the transport moved from the Timeline
// to the Viewer footer, so the panel built here is a ViewerEditor. Every behavioral claim below is
// unchanged -- the button's objectName, type, text() and isChecked() contract all moved with it.
void testPlaybackToggleButtonAndSpaceShortcut(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Playback Widget", time(4)));

    QWidget host;
    auto& sharedPlayback = fixture.controller.playbackController();
    sharedPlayback.installWindowShortcut(host);
    auto* layout = new QVBoxLayout(&host);
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller, nullptr, &host);
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

    auto* secondViewer = new ui::ViewerEditor(fixture.session, fixture.controller, nullptr, &host);
    layout->addWidget(secondViewer);
    auto* secondButton = secondViewer->findChild<QToolButton*>("playPauseButton");
    expectations.expect(secondButton != nullptr && secondButton->isChecked(),
                        "a second Viewer reflects the already playing shared transport");
    expectations.expect(host.findChildren<QAction*>("playPauseAction").size() == 1,
                        "multiple Viewer panels do not duplicate Space shortcuts");
    auto* viewer = new ui::ViewerEditor(fixture.session, fixture.controller, nullptr, &host);
    auto* nodes = new ui::NodeGraphEditor(fixture.session, &host);
    auto* plainButton = new QPushButton(QStringLiteral("Non-text button"), &host);
    layout->addWidget(viewer);
    layout->addWidget(nodes);
    layout->addWidget(plainButton);
    for (QWidget* panel : {static_cast<QWidget*>(viewer), static_cast<QWidget*>(nodes->graphView()),
                           static_cast<QWidget*>(plainButton)}) {
        panel->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        const auto before = sharedPlayback.state();
        QTest::keyClick(panel, Qt::Key_Space);
        QCoreApplication::processEvents();
        expectations.expect(sharedPlayback.state() != before,
                            "Space toggles from Viewer, Nodes, and non-text button focus");
    }
    auto* richText = new QTextEdit(&host);
    auto* plainText = new QPlainTextEdit(&host);
    layout->addWidget(richText);
    layout->addWidget(plainText);
    for (QWidget* text : {static_cast<QWidget*>(richText), static_cast<QWidget*>(plainText)}) {
        text->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
        const auto before = sharedPlayback.state();
        QTest::keyClick(text, Qt::Key_Space);
        QCoreApplication::processEvents();
        expectations.expect(sharedPlayback.state() == before, "multiline text entry keeps Space");
    }
    expectations.expect(richText->toPlainText() == QStringLiteral(" ") &&
                            plainText->toPlainText() == QStringLiteral(" "),
                        "both multiline editors receive the actual character");
    editor->hide();
    secondViewer->hide();
    viewer->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    const auto beforeHidden = sharedPlayback.state();
    QTest::keyClick(viewer, Qt::Key_Space);
    expectations.expect(sharedPlayback.state() != beforeHidden,
                        "hidden panels do not disable Space");
    delete editor;
    delete secondViewer;
    const auto beforeRemoved = sharedPlayback.state();
    QTest::keyClick(viewer, Qt::Key_Space);
    expectations.expect(sharedPlayback.state() != beforeRemoved,
                        "Space and transport survive removal of the panels that showed them");

    finishFixture(fixture, expectations);
}

// Frame stepping (issue #108, decision 1): Right from t=0 lands exactly on frame one's exact
// mapped time (1/25 s at this fixture's 25 fps rate), through the SAME CompositionSession::
// setCurrentTime() mutator the ruler/keyframe gestures already use. Exercised through the real
// stepForwardAction QAction (ViewerEditor::stepFrame()'s public seam -- task VIEW-1 moved the
// transport there from the Timeline), not a reimplementation of the frame math.
void testStepForwardFromZeroLandsOnFrameOne(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step Forward", time(4)));
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller);
    auto* stepForward = editor->findChild<QAction*>("stepForwardAction");
    expectations.expect(stepForward != nullptr, "the step-forward action is reachable by name");
    if (stepForward == nullptr) {
        delete editor;
        finishFixture(fixture, expectations);
        return;
    }

    expectations.expect(fixture.session.currentTime() == time(0), "session starts at exact zero");
    stepForward->trigger();
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "stepping forward from t=0 lands exactly on frame one's mapped time");

    delete editor;
    finishFixture(fixture, expectations);
}

// Left at frame 0 clamps to frame 0 -- landing on the SAME exact time CompositionSession::
// setCurrentTime() already refuses to re-emit for (composition_session.cpp's early-return-on-
// equal-time guard), so no signal churn reaches observers.
void testStepBackwardAtZeroClampsWithNoSignalChurn(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step Clamp", time(4)));
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller);
    auto* stepBackward = editor->findChild<QAction*>("stepBackwardAction");
    expectations.expect(stepBackward != nullptr, "the step-backward action is reachable by name");
    if (stepBackward == nullptr) {
        delete editor;
        finishFixture(fixture, expectations);
        return;
    }

    int changeCount = 0;
    QObject::connect(&fixture.session, &ui::CompositionSession::currentTimeChanged,
                     [&changeCount] { ++changeCount; });
    stepBackward->trigger();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "stepping backward at frame 0 clamps to frame 0");
    expectations.expect(changeCount == 0,
                        "clamping to the SAME exact time emits no currentTimeChanged -- no signal "
                        "churn");

    delete editor;
    finishFixture(fixture, expectations);
}

// End jumps to the exact maximum frame time (4 s at 25 fps == 100 frames, indices 0..99).
void testStepToEndLandsAtExactMaxFrame(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step End", time(4)));
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller);
    auto* stepToEnd = editor->findChild<QAction*>("stepToEndAction");
    expectations.expect(stepToEnd != nullptr, "the go-to-end action is reachable by name");
    if (stepToEnd == nullptr) {
        delete editor;
        finishFixture(fixture, expectations);
        return;
    }

    stepToEnd->trigger();
    expectations.expect(
        fixture.session.currentTime() == time(99, 25),
        "End lands exactly on the last frame's mapped time (index 99 at 25 fps over "
        "a 4 s duration)");

    delete editor;
    finishFixture(fixture, expectations);
}

// Home jumps to exact zero from anywhere.
void testStepToStartLandsAtExactZero(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step Home", time(4)));
    expectations.expect(fixture.session.setCurrentTime(time(50, 25)),
                        "session time moves away from zero first");
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller);
    auto* stepToStart = editor->findChild<QAction*>("stepToStartAction");
    expectations.expect(stepToStart != nullptr, "the go-to-start action is reachable by name");
    if (stepToStart == nullptr) {
        delete editor;
        finishFixture(fixture, expectations);
        return;
    }

    stepToStart->trigger();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "Home lands exactly on frame zero");

    delete editor;
    finishFixture(fixture, expectations);
}

// A step from a SUBFRAME time lands per FrameTimeMapping::nearestFrameIndex()'s own documented tie
// rule (frame_time_mapping.hpp: "Clamps to the valid frame range and rounds to nearest; an exact
// halfway result selects the greater frame index" -- restated by docs/architecture/
// animation-and-time.md, "Session Time And Scrubbing": "selects the nearest index with an exact
// halfway tie going to the greater index"). 1/50 s is EXACTLY halfway between frame 0 (0 s) and
// frame 1 (1/25 s == 2/50 s) at this fixture's 25 fps rate, so nearestFrameIndex(1/50 s) == 1 (the
// GREATER of the tied indices) -- both step directions below are computed from that index 1 base.
void testStepFromSubframeTimeUsesNearestIndexTieRule(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step Subframe Tie", time(4)));
    expectations.expect(fixture.session.setCurrentTime(time(1, 50)),
                        "session time moves to the exact halfway-tie subframe time");
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller);
    auto* stepBackward = editor->findChild<QAction*>("stepBackwardAction");
    auto* stepForward = editor->findChild<QAction*>("stepForwardAction");
    expectations.expect(stepBackward != nullptr && stepForward != nullptr,
                        "both step actions are reachable by name");
    if (stepBackward == nullptr || stepForward == nullptr) {
        delete editor;
        finishFixture(fixture, expectations);
        return;
    }

    stepForward->trigger();
    expectations.expect(
        fixture.session.currentTime() == time(2, 25),
        "stepping forward from the tied subframe time steps from index 1 (the tie's "
        "greater index), landing on frame 2's exact time -- not frame 1");

    expectations.expect(fixture.session.setCurrentTime(time(1, 50)),
                        "session time returns to the same tied subframe time");
    stepBackward->trigger();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "stepping backward from the SAME tied subframe time steps from the SAME "
                        "index 1 base, landing on frame 0 -- confirming both directions read the "
                        "tie identically rather than each independently floor/ceiling-ing the "
                        "subframe time");

    delete editor;
    finishFixture(fixture, expectations);
}

// Stepping while playing pauses playback FIRST through PlaybackController's own public pause()
// (design decision 1), asserted via the SAME play/pause button state the Space test above already
// uses as its transport-state proxy (TimelineEditor owns playback_ privately -- this button is the
// one observable seam).
void testStepDuringPlaybackPausesThenSteps(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step Playback Pause", time(4)));
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller);
    auto* button = editor->findChild<QToolButton*>("playPauseButton");
    auto* stepForward = editor->findChild<QAction*>("stepForwardAction");
    expectations.expect(button != nullptr && stepForward != nullptr,
                        "the play/pause button and step-forward action are reachable by name");
    if (button == nullptr || stepForward == nullptr) {
        delete editor;
        finishFixture(fixture, expectations);
        return;
    }

    button->click();
    expectations.expect(button->isChecked(), "playback starts Playing");
    expectations.expect(fixture.session.currentTime() == time(0),
                        "play() itself has not moved session time (no tick has fired)");

    stepForward->trigger();
    expectations.expect(!button->isChecked(), "stepping while playing pauses the transport FIRST");
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "the step itself still lands exactly on frame one's mapped time");

    delete editor;
    finishFixture(fixture, expectations);
}

// Shortcuts (design decision 2): QActions mirroring the Space idiom exactly -- Left/Right/Home/End
// move session time when no text-entry widget has focus; a focused QLineEdit keeps the keys for
// its own cursor movement via the SAME Qt ShortcutOverride mechanism the Space test above already
// exercises for real.
void testFrameStepShortcutsMoveTimeAndTextEntryFocusWins(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step Shortcuts", time(4)));

    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller, nullptr, &host);
    auto* probeLineEdit = new QLineEdit(&host);
    probeLineEdit->setObjectName("frameStepTestProbeLineEdit");
    layout->addWidget(editor);
    layout->addWidget(probeLineEdit);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    editor->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(editor, Qt::Key_Right);
    QCoreApplication::processEvents();
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "the Right application shortcut steps forward one frame when no text-entry "
                        "widget has focus");

    QTest::keyClick(editor, Qt::Key_Home);
    QCoreApplication::processEvents();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "the Home application shortcut jumps to frame zero");

    QTest::keyClick(editor, Qt::Key_End);
    QCoreApplication::processEvents();
    expectations.expect(fixture.session.currentTime() == time(99, 25),
                        "the End application shortcut jumps to the last frame");

    probeLineEdit->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(QApplication::focusWidget() == probeLineEdit,
                        "the probe line edit genuinely holds keyboard focus");
    probeLineEdit->setText(QStringLiteral("ab"));
    probeLineEdit->setCursorPosition(2);
    QTest::keyClick(probeLineEdit, Qt::Key_Left);
    QCoreApplication::processEvents();
    expectations.expect(fixture.session.currentTime() == time(99, 25),
                        "Left is NOT stolen from a focused text-entry widget: session time is "
                        "unchanged by this keystroke");
    expectations.expect(probeLineEdit->cursorPosition() == 1,
                        "the focused line edit consumed Left as ordinary cursor movement, "
                        "confirming it -- not a dropped/ignored event -- is what won the key");

    finishFixture(fixture, expectations);
}

// Arrow-key conflict finding (issue #108's own investigation, verified with a standalone Qt harness
// before the reconciliation was written): the layer stack already consumes arrow/Home/End keys for
// its OWN row navigation (Home/End jump to the first/last row), but -- unlike a text-entry widget
// -- does not claim the ShortcutOverride event for those keys, so a same-key WindowShortcut action
// would otherwise silently swallow that navigation the instant it existed. The reconciliation rule
// implemented in timeline_editor.cpp: widget-focus wins -- stepping is suppressed while the stack
// has focus, and the stack's own navigation runs completely unchanged; step fires again once focus
// leaves.
//
// ADAPTED for task T1 (enumerated in that task's report): the layer stack is no longer a
// QTreeWidget -- it is a painted row grid (bloom::ui::TimelineLayerStack, same "layerStackView"
// objectName, same role) -- so the SAME assertions are made through its
// currentRow()/setCurrentRow()/rowCount() seam instead of
// currentItem()/setCurrentItem()/topLevelItemCount(). Every behavioral claim below is
// byte-identical to the pre-T1 version; only the primitive the claim is read off changed.
//
// ADAPTED AGAIN for task VIEW-1: the four step actions now belong to the Viewer, so the fixture
// holds BOTH panels in one window -- which is also the real arrangement the rule has to work in,
// since the two panels no longer share a class. The reconciliation moved with the actions and is
// keyed on kDefersTransportKeysProperty, which TimelineLayerStack sets on itself; every claim
// below is unchanged.
void testArrowKeysOnLayerStackStillNavigateAndStepIsSuppressed(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Frame Step Tree Conflict", time(4)));
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.0, 0.0, 0.0, 1.0}),
        "first layer added");
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0.0, 0.0, 0.0, 1.0}),
        "second layer added");
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("C"), core::Color4d{0.0, 0.0, 0.0, 1.0}),
        "third layer added");

    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* timeline = new ui::TimelineEditor(fixture.session, fixture.controller, &host);
    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller, nullptr, &host);
    layout->addWidget(timeline);
    layout->addWidget(editor);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();

    auto* stack = timeline->layerStackForTest();
    expectations.expect(stack != nullptr && stack->rowCount() == 3,
                        "the layer stack has all three rows");
    if (stack == nullptr || stack->rowCount() != 3) {
        finishFixture(fixture, expectations);
        return;
    }
    expectations.expect(timeline->findChild<QWidget*>("layerStackView") == stack,
                        "and it is still the widget the layerStackView objectName names");

    editor->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(editor, Qt::Key_Right);
    QCoreApplication::processEvents();
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "stepping works normally before the stack has focus");

    stack->setFocus(Qt::OtherFocusReason);
    stack->setCurrentRow(1);
    QCoreApplication::processEvents();
    expectations.expect(QApplication::focusWidget() == stack, "the stack genuinely holds focus");

    QTest::keyClick(stack, Qt::Key_Home);
    QCoreApplication::processEvents();
    expectations.expect(stack->currentRow() == 0,
                        "the stack's OWN Home navigation (jump to the first row) still runs "
                        "unchanged while it has focus");
    expectations.expect(
        fixture.session.currentTime() == time(1, 25),
        "the step-to-start action is suppressed while the stack has focus -- session "
        "time is untouched");

    QTest::keyClick(stack, Qt::Key_End);
    QCoreApplication::processEvents();
    expectations.expect(
        stack->currentRow() == 2,
        "the stack's OWN End navigation (jump to the last row) still runs unchanged "
        "while it has focus");
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "the step-to-end action is suppressed while the stack has focus -- session "
                        "time is still untouched");

    editor->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    QTest::keyClick(editor, Qt::Key_Home);
    QCoreApplication::processEvents();
    expectations.expect(fixture.session.currentTime() == time(0),
                        "stepping resumes once focus leaves the stack");

    finishFixture(fixture, expectations);
}

// Frame preference follows session time and resets with the active composition.
void testTimeReadoutFormatsFrameExactTimeAndResetsOnCompositionSwitch(Expectations& expectations) {
    using namespace bloom;
    QSettings{}.setValue(QStringLiteral("timeline/time-format"), QStringLiteral("frames"));
    auto newProject = makeTestProject("Readout Format", time(4));
    expectations.expect(newProject.project.addComposition(secondComposition(time(4), testFormat())),
                        "a second composition is added to the project");
    SessionFixture fixture(std::move(newProject));

    auto* editor = new ui::ViewerEditor(fixture.session, fixture.controller);
    auto* readout = editor->findChild<QLabel*>("timelineTimeReadout");
    expectations.expect(readout != nullptr, "the time readout label is reachable by name");
    if (readout == nullptr) {
        delete editor;
        finishFixture(fixture, expectations);
        return;
    }

    expectations.expect(readout->text() == QStringLiteral("0"),
                        "the readout starts at exact frame 0 / 0.000s");

    expectations.expect(fixture.session.setCurrentTime(time(1, 25)),
                        "session time advances to exactly frame one's time");
    expectations.expect(readout->text() == QStringLiteral("1"),
                        "the readout tracks currentTimeChanged() at an exact frame time");

    expectations.expect(fixture.session.setCurrentTime(time(1, 3)),
                        "session time moves to a genuinely subframe exact time (1/3 s)");
    expectations.expect(readout->text() == QStringLiteral("8"),
                        "frame preference shows the nearest frame index at 25 fps");

    expectations.expect(fixture.session.setComposition(document::CompositionId::fromRaw(2)),
                        "switching composition succeeds");
    expectations.expect(readout->text() == QStringLiteral("0"),
                        "the readout resets to frame 0 / 0.000s across a composition switch, "
                        "matching CompositionSession's own session-time reset");

    delete editor;
    finishFixture(fixture, expectations);
}

// Opt-in bounded clock evidence. Five competing 4x3 CPU-reference sessions use production
// transport timers and the normal 100ms Jobs interval. This measures desired time, not painting.
void testRealClockProof(Expectations& expectations) {
    using namespace bloom;
    constexpr auto rates = std::array{std::pair{24U, 1U}, std::pair{25U, 1U}, std::pair{30U, 1U},
                                      std::pair{60U, 1U}, std::pair{30000U, 1001U}};
    struct Sample final {
        std::optional<std::chrono::steady_clock::time_point> origin;
        std::chrono::steady_clock::time_point read{};
        std::chrono::nanoseconds maximumGap{};
        std::uint64_t reads = 0;
        std::uint64_t changes = 0;
        std::uint64_t mismatches = 0;
    };
    std::array<Sample, rates.size()> samples;
    std::array<std::unique_ptr<SessionFixture>, rates.size()> fixtures;
    std::array<std::unique_ptr<ui::PlaybackController>, rates.size()> transports;
    QEventLoop loop;
    for (std::size_t index = 0; index < rates.size(); ++index) {
        const auto [p, q] = rates[index];
        fixtures[index] = std::make_unique<SessionFixture>(
            document::makeNewProject("Real clock", "Main", time(120), testFormat(p, q)), 100ms);
        auto& fixture = *fixtures[index];
        expectations.expect(waitUntil([&] {
                                return fixture.controller.state().freshness ==
                                       ui::FrameFreshness::Current;
                            }),
                            "clock proof opening product is ready");
        transports[index] = std::make_unique<ui::PlaybackController>(
            fixture.session, fixture.controller, [&, index] {
                auto& sample = samples[index];
                const auto now = std::chrono::steady_clock::now();
                if (!sample.origin)
                    sample.origin = now;
                else
                    sample.maximumGap = std::max(
                        sample.maximumGap,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now - sample.read));
                sample.read = now;
                ++sample.reads;
                return now;
            });
        const auto mappingResult = core::FrameTimeMapping::create(time(120), p, q);
        const auto mapping = *mappingResult.value();
        QObject::connect(&fixture.session, &ui::CompositionSession::currentTimeChanged, &loop,
                         [&, index, mapping, p, q] {
                             auto& sample = samples[index];
                             const auto origin = sample.origin;
                             if (!origin) {
                                 ++sample.mismatches;
                                 return;
                             }
                             const auto elapsed =
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(sample.read -
                                                                                      *origin);
                             // This 60s bounded reference fits uint64, independently of the
                             // production portable wide-integer implementation.
                             const auto expected = static_cast<std::uint64_t>(elapsed.count()) * p /
                                                   (std::uint64_t{1'000'000'000} * q);
                             const auto actual =
                                 mapping.nearestFrameIndex(fixtures[index]->session.currentTime());
                             ++sample.changes;
                             if (actual != expected)
                                 ++sample.mismatches;
                         });
    }
    for (auto& transport : transports)
        transport->play();
    bool timedOut = false;
    QTimer finish;
    finish.setSingleShot(true);
    finish.setTimerType(Qt::PreciseTimer);
    QObject::connect(&finish, &QTimer::timeout, &loop, [&] {
        for (auto& transport : transports) {
            transport->tick();
            transport->pause();
        }
        loop.quit();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    finish.start(60'000);
    watchdog.start(65'000);
    loop.exec();
    expectations.expect(!timedOut, "bounded real clock proof completes before watchdog");
    for (std::size_t index = 0; index < rates.size(); ++index) {
        const auto& sample = samples[index];
        const auto origin = sample.origin;
        if (!origin) {
            expectations.expect(false, "real-clock playback captured an origin");
            transports[index]->pause();
            finishFixture(*fixtures[index], expectations);
            continue;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(sample.read - *origin);
        expectations.expect(
            elapsed >= 60s && sample.changes > 0 && sample.mismatches == 0,
            "all genuine real-clock desired frame changes follow exact elapsed grid");
        const auto expectedFinal = static_cast<std::uint64_t>(elapsed.count()) *
                                   rates[index].first /
                                   (std::uint64_t{1'000'000'000} * rates[index].second);
        expectations.expect(
            fixtures[index]->session.currentTime() ==
                time(static_cast<std::int64_t>(expectedFinal * rates[index].second),
                     rates[index].first),
            "final desired time equals exact elapsed frame even after a late freeze");
        const auto& state = fixtures[index]->controller.state();
        std::cout << "REAL_CLOCK rate=" << rates[index].first << '/' << rates[index].second
                  << " elapsedNs=" << elapsed.count() << " reads=" << sample.reads
                  << " desiredChanges=" << sample.changes << " mismatches=" << sample.mismatches
                  << " maxWakeGapNs=" << sample.maximumGap.count()
                  << " desiredSeconds=" << fixtures[index]->session.currentTime().toSeconds()
                  << " preparedSeconds="
                  << (state.frame ? state.frame->desiredIdentity().time.toSeconds() : -1.0)
                  << " paintedEvidence=none provider=CPU-reference\n";
        transports[index]->pause();
        finishFixture(*fixtures[index], expectations);
    }
}

} // namespace

int main(int argc, char** argv) try {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    if (argc == 2 && std::string_view(argv[1]) == "--real-clock-proof") {
        testRealClockProof(expectations);
        return expectations.failures() == 0 ? 0 : 1;
    }
    testWorkAreaLoop(expectations);
    testPlayAdvancesExactFrameTimesAndDropsFrames(expectations);
    testCachedFramesUseElapsedClock(expectations);
    testTransportRationalRateBoundaries(expectations);
    testAudioMasterFloorLoopAndDisableReanchor(expectations);
    testSharedSubframeOriginAndNonOneAudioRates(expectations);
    testRateChangesEmptyGridAndInvalidClock(expectations);
    testLiveWorkAreaEditsReanchorSafely(expectations);
    testPendingDeadlineRetainsIntentAndCountsLateOnce(expectations);
    testSlowPlaybackStillSubmitsWhenIdle(expectations);
    testLoopWrapExactAfterManyWraps(expectations);
    testPauseFreezesAndResumeUsesCurrentSessionTime(expectations);
    testScrubDuringPlaybackPauses(expectations);
    testStopsOnCompositionSwitch(expectations);
    testOverflowingDurationCompositionPlaybackIsNoOp(expectations);
    testIntegrationPlaybackCoalescesWhileForegroundGateIsBusy(expectations);
    testPlaybackToggleButtonAndSpaceShortcut(expectations);
    testStepForwardFromZeroLandsOnFrameOne(expectations);
    testStepBackwardAtZeroClampsWithNoSignalChurn(expectations);
    testStepToEndLandsAtExactMaxFrame(expectations);
    testStepToStartLandsAtExactZero(expectations);
    testStepFromSubframeTimeUsesNearestIndexTieRule(expectations);
    testStepDuringPlaybackPausesThenSteps(expectations);
    testFrameStepShortcutsMoveTimeAndTextEntryFocusWins(expectations);
    testArrowKeysOnLayerStackStillNavigateAndStepIsSuppressed(expectations);
    testTimeReadoutFormatsFrameExactTimeAndResetsOnCompositionSwitch(expectations);
    return expectations.failures() == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "FAIL: playback regression threw: " << error.what() << '\n';
    return 1;
}
