#include <bloom/ui/audio_device_control.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>
#include <QEventLoop>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using namespace std::chrono_literals;

void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (std::invoke(predicate))
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return std::invoke(predicate);
}

struct Gate final {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false;
    bool open = false;
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<bool> stoppedOnUi{false};
    std::thread::id uiThread;
    std::atomic<bool> destroyed{false};
    std::thread::id destroyedOn;
};

class HeldBackend final : public media::audio::playback::Backend {
  public:
    explicit HeldBackend(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    ~HeldBackend() override {
        gate_->destroyedOn = std::this_thread::get_id();
        gate_->destroyed.store(true, std::memory_order_release);
    }
    media::audio::playback::AudioStatus start(std::uint32_t, std::uint32_t, Callback,
                                               void*) noexcept override {
        gate_->starts.fetch_add(1);
        std::unique_lock lock(gate_->mutex);
        gate_->entered = true;
        gate_->ready.notify_all();
        gate_->ready.wait(lock, [&] { return gate_->open; });
        return std::nullopt;
    }
    void stop() noexcept override {
        gate_->stops.fetch_add(1);
        if (std::this_thread::get_id() == gate_->uiThread)
            gate_->stoppedOnUi.store(true);
    }

  private:
    std::shared_ptr<Gate> gate_;
};

void heldStartAndLatestStop() {
    runtime::TaskScheduler scheduler({.cpuWorkerCount = 1,
                                      .blockingIoWorkerCount = 1,
                                      .cpuQueueCapacity = 2,
                                      .blockingIoQueueCapacity = 2});
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    auto gate = std::make_shared<Gate>();
    gate->uiThread = std::this_thread::get_id();
    auto engine = std::make_unique<media::audio::playback::AudioEngine>(
        std::make_unique<HeldBackend>(gate));
    ui::AudioDeviceControl control(scheduler, bridge, std::move(engine));
    std::uint64_t completedGeneration = 0;
    bool completedPlaying = true;
    control.setCompletionHandler([&](const std::uint64_t generation, const bool playing,
                                     const bool) {
        completedGeneration = generation;
        completedPlaying = playing;
    });
    (void)control.requestPlay(core::RationalTime{});
    require(waitUntil([&] {
                const std::lock_guard lock(gate->mutex);
                return gate->entered;
            }),
            "held backend entered worker start");
    const auto before = std::chrono::steady_clock::now();
    (void)control.requestSeek(core::RationalTime::fromInteger(1));
    const auto stopped = control.requestStop();
    require(std::chrono::steady_clock::now() - before < 100ms,
            "held device never blocks UI seek/pause");
    {
        const std::lock_guard lock(gate->mutex);
        gate->open = true;
    }
    gate->ready.notify_all();
    require(waitUntil([&] { return completedGeneration == stopped && !completedPlaying; }),
            "only newest stop completion is published");
    require(gate->starts.load() == 1 && !control.isPlaying(),
            "intermediate seek did not start a second device operation");
    require(!gate->stoppedOnUi.load(), "all backend stop calls stayed off UI");
    control.beginShutdown();
    require(waitUntil([&] { return gate->destroyed.load(std::memory_order_acquire); }),
            "terminal task retires held device");
    require(control.retirementComplete() && gate->destroyedOn != std::this_thread::get_id(),
            "device destructor ran off the UI thread");
    control.drainAfterEventLoop();
}

struct PullGate final {
    std::mutex mutex;
    std::condition_variable ready;
    bool blockerEntered = false;
    bool blockerOpen = false;
    bool started = false;
    int stops = 0;
    media::audio::playback::Backend::Callback callback = nullptr;
    void* userData = nullptr;
};

class PullBackend final : public media::audio::playback::Backend {
  public:
    explicit PullBackend(std::shared_ptr<PullGate> gate) : gate_(std::move(gate)) {}
    media::audio::playback::AudioStatus start(std::uint32_t, std::uint32_t, Callback callback,
                                               void* userData) noexcept override {
        std::lock_guard lock(gate_->mutex);
        gate_->callback = callback;
        gate_->userData = userData;
        gate_->started = true;
        return std::nullopt;
    }
    void stop() noexcept override {
        std::lock_guard lock(gate_->mutex);
        ++gate_->stops;
        gate_->started = false;
    }

  private:
    std::shared_ptr<PullGate> gate_;
};

void heldStopThenPlayIsAudible() {
    runtime::TaskScheduler scheduler({.cpuWorkerCount = 1,
                                      .blockingIoWorkerCount = 1,
                                      .cpuQueueCapacity = 2,
                                      .blockingIoQueueCapacity = 2});
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    auto gate = std::make_shared<PullGate>();
    auto engine = std::make_unique<media::audio::playback::AudioEngine>(
        std::make_unique<PullBackend>(gate));
    ui::AudioDeviceControl control(scheduler, bridge, std::move(engine));
    auto buffer = std::make_shared<media::audio::AudioBuffer>();
    buffer->rate = 48'000;
    buffer->channels = 1;
    buffer->frames = 512;
    buffer->planes = {std::vector<float>(512, 0.75F)};
    (void)control.replaceClips({media::audio::playback::AudioClip{.buffer = buffer}});
    require(waitUntil([&] { return control.isIdle(); }), "initial clip reached worker");
    std::uint64_t completed = 0;
    bool playing = false;
    control.setCompletionHandler([&](const std::uint64_t generation, const bool nowPlaying,
                                     const bool) {
        completed = generation;
        playing = nowPlaying;
    });
    const auto firstPlay = control.requestPlay(core::RationalTime{});
    require(waitUntil([&] { return completed == firstPlay && playing; }), "first play started");
    // Occupy BlockingIo before Stop reaches AudioEngine. The newer Play must capture its token
    // before the older Stop executes, or an extra worker-side generation bump is not tested.
    auto blocker = scheduler.submit<void>(
        runtime::TaskRequest("Hold audio device worker",
                             {.kind = runtime::TaskOwnerKind::Application,
                              .id = runtime::TaskOwnerId::fromRaw(316)},
                             runtime::TaskPriority::Interactive,
                             runtime::TaskExecutor::BlockingIo),
        [gate](runtime::TaskContext&) {
            std::unique_lock lock(gate->mutex);
            gate->blockerEntered = true;
            gate->ready.notify_all();
            gate->ready.wait(lock, [&] { return gate->blockerOpen; });
            return runtime::TaskResult<void>::succeeded();
        });
    struct ReleaseBlocker final {
        std::shared_ptr<PullGate> gate;
        ~ReleaseBlocker() {
            {
                const std::lock_guard lock(gate->mutex);
                gate->blockerOpen = true;
            }
            gate->ready.notify_all();
        }
    } releaseBlocker{gate};
    require(blocker.accepted(), "independent worker blocker admitted");
    require(waitUntil([&] {
                const std::lock_guard lock(gate->mutex);
                return gate->blockerEntered;
            }),
            "independent blocker occupies the audio worker");
    int stopsBefore = 0;
    {
        const std::lock_guard lock(gate->mutex);
        stopsBefore = gate->stops;
    }
    (void)control.requestStop();
    const auto secondPlay = control.requestPlay(core::RationalTime{});
    {
        const std::lock_guard lock(gate->mutex);
        gate->blockerOpen = true;
    }
    gate->ready.notify_all();
    require(waitUntil([&] { return completed == secondPlay && playing; }),
            "newer play completed after queued worker stop");
    {
        const std::lock_guard lock(gate->mutex);
        require(gate->stops >= stopsBefore + 2, "older stop executed before newer play");
    }
    const bool audible = waitUntil([&] {
        media::audio::playback::Backend::Callback callback = nullptr;
        void* userData = nullptr;
        {
            const std::lock_guard lock(gate->mutex);
            if (!gate->started)
                return false;
            callback = gate->callback;
            userData = gate->userData;
        }
        std::vector<float> output(256 * 2);
        callback(userData, output);
        return std::any_of(output.begin(), output.end(), [](float value) { return value > 0.1F; });
    });
    require(audible, "Stop then Play produces nonzero output after worker stop");
    control.beginShutdown();
    require(waitUntil([&] { return control.retirementComplete(); }),
            "held-stop device retires");
    control.drainAfterEventLoop();
}

void closedAdmissionUsesRootFallback() {
    runtime::TaskScheduler scheduler({.cpuWorkerCount = 1,
                                      .blockingIoWorkerCount = 1,
                                      .cpuQueueCapacity = 2,
                                      .blockingIoQueueCapacity = 2});
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    auto gate = std::make_shared<Gate>();
    gate->uiThread = std::this_thread::get_id();
    auto engine = std::make_unique<media::audio::playback::AudioEngine>(
        std::make_unique<HeldBackend>(gate));
    ui::AudioDeviceControl control(scheduler, bridge, std::move(engine));
    scheduler.beginShutdown();
    control.beginShutdown();
    require(control.retirementComplete(), "closed admission selects explicit root fallback");
    control.drainAfterEventLoop();
    require(gate->destroyed.load(std::memory_order_acquire) &&
                gate->destroyedOn != std::this_thread::get_id(),
            "root fallback destroys device off UI after scheduler quiescence");
}

void stalePlayCannotUnsilence() {
    auto backend = std::make_unique<media::audio::playback::NullBackend>();
    auto* captured = backend.get();
    media::audio::playback::AudioEngine engine(std::move(backend));
    auto buffer = std::make_shared<media::audio::AudioBuffer>();
    buffer->rate = 48'000;
    buffer->channels = 1;
    buffer->frames = 512;
    buffer->planes = {std::vector<float>(512, 0.75F)};
    engine.replaceClips({media::audio::playback::AudioClip{.buffer = buffer}});
    const auto staleGeneration = engine.silenceGeneration();
    engine.requestSilence();
    require(!engine.play(core::RationalTime{}, staleGeneration),
            "stale device start itself succeeded");
    require(!engine.renderForTesting(128), "stale device start rendered for test");
    for (const float sample : captured->captured())
        require(sample == 0.0F, "newer pause wins after stale start completes");
    engine.stop();
}
} // namespace

int main(int argc, char** argv) try {
    QCoreApplication app(argc, argv);
    heldStartAndLatestStop();
    heldStopThenPlayIsAudible();
    closedAdmissionUsesRootFallback();
    stalePlayCannotUnsilence();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Audio device control: " << error.what() << '\n';
    return 1;
}
