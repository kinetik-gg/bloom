// #400 looped-playback audio: PlaybackController hands the work-area loop to the mixer, so every
// pass is audible, both master clocks wrap without drift, and live work-area and looping changes
// keep audio and pictures on one period. A NullBackend engine is the device: renders are pulled by
// the test, so the audio clock advances only when the test says so and no sound is produced.
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/audio_device_control.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace bloom;
namespace audio = media::audio::playback;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
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
        if (std::invoke(predicate))
            return true;
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value)
        std::abort();
    return *value;
}

[[nodiscard]] document::NewProject makeProject(const std::uint32_t rateNumerator = 25,
                                               const std::uint32_t rateDenominator = 1) {
    const auto rate = document::FrameRate::create(rateNumerator, rateDenominator);
    const auto format =
        rate ? document::CompositionFormat::create(4, 3, core::PixelAspectRatio::square(), *rate)
             : std::nullopt;
    if (!format)
        std::abort();
    return document::makeNewProject("Audio loop", "Main", time(2), *format);
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction preparation;

    Pipeline() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        preparation = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                         qualifiedProcessorProvider);
    }
};

// One composition at 1 kHz audio: the clip's sample i reads i + 1, so the frame a pass plays and
// any silence are both explicit in the captured output.
struct Fixture final {
    document::Document document;
    commands::CommandStack commands;
    ui::CompositionSession session;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge;
    Pipeline pipeline;
    ui::CompositionPreviewController preview;
    audio::NullBackend* device = nullptr;
    audio::AudioEngine* engine = nullptr;
    std::unique_ptr<ui::AudioDeviceControl> control;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::unique_ptr<ui::PlaybackController> playback;

    explicit Fixture(document::NewProject project)
        : document(std::move(project.project)), commands(document),
          session(document, commands, project.initialCompositionId),
          scheduler(runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                 .blockingIoWorkerCount = 1,
                                                 .cpuQueueCapacity = 16,
                                                 .blockingIoQueueCapacity = 4,
                                                 .terminalHistoryCapacity = 32,
                                                 .diagnosticsPerTask = 8,
                                                 .groupRegistryCapacity = 8}),
          bridge(scheduler, nullptr, 1ms),
          preview(session, scheduler, bridge, pipeline.preparation) {
        auto backend = std::make_unique<audio::NullBackend>();
        device = backend.get();
        auto owned = std::make_unique<audio::AudioEngine>(
            std::move(backend), audio::AudioEngine::Config{1000, 1, 4096});
        engine = owned.get();
        control = std::make_unique<ui::AudioDeviceControl>(scheduler, bridge, std::move(owned));
        playback = std::make_unique<ui::PlaybackController>(
            session, preview, [this] { return now; }, 16ms);
        playback->setAudioDeviceControl(control.get());
        std::vector<float> ramp(2000);
        for (std::size_t index = 0; index < ramp.size(); ++index)
            ramp[index] = static_cast<float>(index + 1);
        runtime::AudioMixDescription mix;
        mix.clips.push_back(
            {document::NodeId::fromRaw(1), document::AssetId::fromRaw(1), time(0), time(2)});
        playback->setAudioMix(
            std::move(mix),
            {audio::AudioClip{std::make_shared<const media::audio::AudioBuffer>(
                                  media::audio::AudioBuffer{1000, 1, ramp.size(), {ramp}}),
                              time(0)}});
    }

    ~Fixture() {
        playback->pause();
        playback.reset();
        preview.beginShutdown();
        bridge.beginShutdown();
        (void)waitUntil([&] { return scheduler.isQuiescent(); });
        control->beginShutdown();
        control->drainAfterEventLoop();
    }

    bool setWorkArea(const core::RationalTime start, const core::RationalTime end) {
        commands::Transaction range("Work area", session.snapshot().revision());
        range.emplace<commands::SetWorkArea>(session.compositionId(), start, end);
        return session.executeTransaction(std::move(range)).changed();
    }

    [[nodiscard]] bool settled() {
        return waitUntil([&] { return control->isIdle() && playback->audioClockActive(); });
    }

    // Pulls `frames` device frames and returns exactly what the device played.
    std::vector<float> render(const std::size_t frames) {
        device->clearCaptured();
        if (engine->renderForTesting(frames))
            return {};
        return {device->captured().begin(), device->captured().end()};
    }
};

// Samples a 1 kHz render plays for clip frames [first, first + count).
[[nodiscard]] std::vector<float> clipFrames(const std::int64_t first, const std::size_t count) {
    std::vector<float> values;
    for (std::size_t index = 0; index < count; ++index)
        values.push_back(static_cast<float>(first + static_cast<std::int64_t>(index) + 1));
    return values;
}

[[nodiscard]] std::vector<float> join(std::vector<float> head, const std::vector<float>& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

// The owner's report: with repeat on, audio played once. Every pass must replay the work area.
void testAudioMasterLoopsEveryPass(Expectations& expectations) {
    Fixture fixture(makeProject());
    expectations.expect(fixture.setWorkArea(time(2, 5), time(6, 5)), "work area 0.4..1.2 s");
    fixture.playback->play();
    expectations.expect(fixture.settled(), "audio master starts");
    expectations.expect(fixture.engine->loopRange() == audio::LoopRange{time(2, 5), time(6, 5)},
                        "the mixer loops the work area's frame grid");
    for (int pass = 0; pass < 4; ++pass) {
        expectations.expect(fixture.render(800) == clipFrames(400, 800),
                            "pass " + std::to_string(pass) + " replays the whole work area");
        fixture.playback->tick();
        expectations.expect(fixture.session.currentTime() == time(2, 5),
                            "pass " + std::to_string(pass) + " wraps pictures to the exact start");
    }
    expectations.expect(fixture.render(840) == join(clipFrames(400, 800), clipFrames(400, 40)),
                        "a pull that straddles the wrap is gapless");
    fixture.playback->tick();
    expectations.expect(fixture.session.currentTime() == time(11, 25),
                        "after five passes the audio master is still on the exact frame");
    expectations.expect(fixture.playback->audioClockActive(), "audio stays the master clock");
}

// With audio unavailable the wall clock is the master and wraps on the same exact grid.
void testWallClockLoopsWithoutDrift(Expectations& expectations) {
    Fixture fixture(makeProject());
    expectations.expect(fixture.setWorkArea(time(2, 5), time(6, 5)), "work area 0.4..1.2 s");
    fixture.playback->setAudioEnabled(false);
    fixture.playback->play();
    const auto origin = fixture.now;
    for (int pass = 1; pass <= 4; ++pass) {
        fixture.now = origin + std::chrono::milliseconds(800 * pass);
        fixture.playback->tick();
        expectations.expect(fixture.session.currentTime() == time(2, 5),
                            "wall pass " + std::to_string(pass) + " lands on the exact start");
        fixture.now += 39ms;
        fixture.playback->tick();
        expectations.expect(fixture.session.currentTime() == time(2, 5),
                            "wall pass " + std::to_string(pass) + " holds the first frame");
        fixture.now += 1ms;
        fixture.playback->tick();
        expectations.expect(fixture.session.currentTime() == time(11, 25),
                            "wall pass " + std::to_string(pass) + " advances on the boundary");
    }
    expectations.expect(!fixture.playback->audioClockActive() && !fixture.engine->isPlaying(),
                        "a disabled audio path never starts the device");
}

// An NTSC rate with an off-grid work area: the published loop is the frame grid the pictures wrap
// on, not the authored endpoints.
void testOffGridWorkAreaLoopsOnTheFrameGrid(Expectations& expectations) {
    Fixture fixture(makeProject(30000, 1001));
    expectations.expect(fixture.setWorkArea(time(1, 10), time(1, 2)), "off-grid work area");
    fixture.playback->play();
    expectations.expect(fixture.settled(), "audio master starts");
    // First frame at or after 0.1 s is 3 (0.1001 s); the last before 0.5 s is 14 (0.467 s).
    expectations.expect(fixture.engine->loopRange() ==
                            audio::LoopRange{time(3 * 1001, 30000), time(15 * 1001, 30000)},
                        "the mixer loops frames 3..14 exactly");
    for (int pass = 0; pass < 3; ++pass) {
        // 12 frames of 1001/30000 s are 400.4 samples: passes alternate 400 and 401 samples.
        (void)fixture.render(pass % 2 == 0 ? 400 : 401);
        fixture.playback->tick();
    }
    (void)fixture.render(400);
    fixture.playback->tick();
    // 1.601 s past the 0.1001 s origin is frame 50: offset 47, modulo 12 is the last frame.
    expectations.expect(fixture.session.currentTime() == time(14 * 1001, 30000),
                        "four fractional-length pulls land on the exact frame");
    // 10 passes are exactly 4004 samples, so the clock lands on the loop start again.
    (void)fixture.render(4004 - 1601);
    fixture.playback->tick();
    expectations.expect(fixture.session.currentTime() == time(3 * 1001, 30000),
                        "ten passes land exactly on the first frame: no drift");
    // The play origin 0.1001 s starts the mixer on sample 100, so device sample 4104 starts 0.1
    // sample before the tenth wrap and still plays the last loop sample (500); the next sample
    // starts 0.9 sample into the new pass, which is loop sample 101.
    expectations.expect(fixture.render(4) == std::vector<float>{501.0F, 102.0F, 103.0F, 104.0F},
                        "and the audio wraps at that exact rational instant");
}

// Changing the work area mid-run relocates both clocks to the new range and loops it.
void testWorkAreaChangeWhilePlaying(Expectations& expectations) {
    Fixture fixture(makeProject());
    expectations.expect(fixture.setWorkArea(time(2, 5), time(6, 5)), "work area 0.4..1.2 s");
    fixture.playback->play();
    expectations.expect(fixture.settled(), "audio master starts");
    expectations.expect(fixture.render(1000) == join(clipFrames(400, 800), clipFrames(400, 200)),
                        "first range loops");
    fixture.playback->tick();
    expectations.expect(fixture.setWorkArea(time(4, 5), time(8, 5)), "work area 0.8..1.6 s");
    expectations.expect(fixture.playback->state() == ui::PlaybackState::Playing &&
                            fixture.session.currentTime() == time(4, 5),
                        "the run re-anchors on the new range's start");
    expectations.expect(fixture.settled(), "audio re-seeks to the new range");
    expectations.expect(fixture.engine->loopRange() == audio::LoopRange{time(4, 5), time(8, 5)},
                        "the mixer loops the new range");
    for (int pass = 0; pass < 3; ++pass) {
        expectations.expect(fixture.render(800) == clipFrames(800, 800),
                            "new range pass " + std::to_string(pass) + " is complete");
        fixture.playback->tick();
        expectations.expect(fixture.session.currentTime() == time(4, 5),
                            "new range pass " + std::to_string(pass) + " wraps exactly");
    }
}

// Looping off mid-run finishes the current pass (audio and pictures) instead of stopping at once;
// looping on mid-run wraps the audio in place with no seek and no gap.
void testLoopingToggledWhilePlaying(Expectations& expectations) {
    Fixture fixture(makeProject());
    expectations.expect(fixture.setWorkArea(time(2, 5), time(6, 5)), "work area 0.4..1.2 s");
    fixture.playback->play();
    expectations.expect(fixture.settled(), "audio master starts");
    (void)fixture.render(1600);
    fixture.playback->tick();
    expectations.expect(fixture.render(200) == clipFrames(400, 200), "third pass begins");
    fixture.playback->tick();
    expectations.expect(fixture.session.currentTime() == time(3, 5), "0.6 s in the third pass");

    fixture.playback->setLooping(false);
    expectations.expect(fixture.settled(), "the loop change reaches the mixer");
    expectations.expect(!fixture.engine->loopRange().has_value(), "the mixer loop is cleared");
    expectations.expect(fixture.render(200) == clipFrames(600, 200),
                        "the current pass keeps playing after looping is turned off");
    fixture.playback->tick();
    expectations.expect(fixture.playback->state() == ui::PlaybackState::Playing &&
                            fixture.session.currentTime() == time(4, 5),
                        "pictures keep playing the current pass rather than stopping at once");
    expectations.expect(fixture.render(400) == clipFrames(800, 400),
                        "audio plays the pass to the work-area end");
    fixture.playback->tick();
    expectations.expect(fixture.playback->state() == ui::PlaybackState::Stopped &&
                            fixture.session.currentTime() == time(29, 25),
                        "the run then stops on the work area's last frame");

    (void)fixture.session.setCurrentTime(time(2, 5));
    fixture.playback->play();
    expectations.expect(fixture.settled(), "a non-looping run starts");
    expectations.expect(fixture.render(400) == clipFrames(400, 400), "first half plays");
    fixture.playback->tick();
    fixture.playback->setLooping(true);
    expectations.expect(fixture.settled(), "looping on reaches the mixer without a restart");
    expectations.expect(fixture.render(800) == join(clipFrames(800, 400), clipFrames(400, 400)),
                        "turning looping on mid-run wraps the audio gaplessly");
    fixture.playback->tick();
    expectations.expect(fixture.playback->state() == ui::PlaybackState::Playing &&
                            fixture.session.currentTime() == time(4, 5),
                        "pictures wrap with the audio");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Expectations expectations;
    testAudioMasterLoopsEveryPass(expectations);
    testWallClockLoopsWithoutDrift(expectations);
    testOffGridWorkAreaLoopsOnTheFrameGrid(expectations);
    testWorkAreaChangeWhilePlaying(expectations);
    testLoopingToggledWhilePlaying(expectations);
    if (expectations.failures() == 0)
        std::cout << "audio loop transport ok\n";
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
