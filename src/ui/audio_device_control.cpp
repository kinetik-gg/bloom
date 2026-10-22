#include <bloom/ui/audio_device_control.hpp>

#include <bloom/ui/task_ui_bridge.hpp>

#include <QThread>
#include <QDebug>

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace bloom::ui {

struct AudioDeviceControl::State final {
    explicit State(std::unique_ptr<media::audio::playback::AudioEngine> owned)
        : engine(std::move(owned)) {}
    // Access to ownership and these serials is serialized by one active BlockingIo task. The UI
    // may call the engine's atomic requestSilence/clock methods until closing is set.
    std::unique_ptr<media::audio::playback::AudioEngine> engine;
    std::uint64_t mixSerial = 0;
    std::uint64_t transportSerial = 0;
    std::atomic<bool> retired{false};
};

struct AudioDeviceControl::Completion final {
    std::uint64_t generation = 0;
    bool playing = false;
    bool transportStarted = false;
    media::audio::playback::AudioStatus error;
};

AudioDeviceControl::AudioDeviceControl(
    runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
    std::unique_ptr<media::audio::playback::AudioEngine> engine, QObject* parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge),
      state_(std::make_shared<State>(std::move(engine))) {
    desired_.clips =
        std::make_shared<const std::vector<media::audio::playback::AudioClip>>();
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, [this] { consumeReady(); });
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, [this] {
        consumeReady();
        if (closing_)
            submitTerminal();
        else
            submitPending();
    });
}

AudioDeviceControl::~AudioDeviceControl() {
    completionHandler_ = {};
    beginShutdown();
    // Production's explicit process guard has already drained this owner. A caller constructing
    // this boundary independently must also invoke drainAfterEventLoop after scheduler quiescence.
    Q_ASSERT(state_->engine == nullptr);
}

void AudioDeviceControl::setCompletionHandler(CompletionHandler handler) {
    Q_ASSERT(QThread::currentThread() == thread());
    completionHandler_ = std::move(handler);
}

void AudioDeviceControl::publishDesired() {
    if (closing_)
        return;
    ++nextGeneration_;
    desired_.generation = nextGeneration_;
    pending_ = true;
    submitPending();
}

std::uint64_t AudioDeviceControl::replaceClips(
    std::vector<media::audio::playback::AudioClip> clips) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_)
        return desired_.generation;
    desired_.clips = std::make_shared<const std::vector<media::audio::playback::AudioClip>>(
        std::move(clips));
    ++desired_.mixSerial;
    publishDesired();
    return desired_.generation;
}

std::uint64_t AudioDeviceControl::requestPlay(const core::RationalTime at) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_)
        return desired_.generation;
    desired_.at = at;
    desired_.playing = true;
    desired_.silenceGeneration = state_->engine ? state_->engine->silenceGeneration() : 0;
    ++desired_.transportSerial;
    publishDesired();
    return desired_.generation;
}

std::uint64_t AudioDeviceControl::requestSeek(const core::RationalTime at) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_)
        return desired_.generation;
    requestSilence();
    desired_.at = at;
    desired_.playing = true;
    desired_.silenceGeneration = state_->engine ? state_->engine->silenceGeneration() : 0;
    ++desired_.transportSerial;
    publishDesired();
    return desired_.generation;
}

std::uint64_t AudioDeviceControl::requestStop() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_)
        return desired_.generation;
    requestSilence();
    desired_.playing = false;
    ++desired_.transportSerial;
    publishDesired();
    return desired_.generation;
}

void AudioDeviceControl::requestSilence() noexcept {
    if (!closing_ && state_->engine)
        state_->engine->requestSilence();
}

core::RationalTime AudioDeviceControl::positionNow() const noexcept {
    return !closing_ && state_->engine ? state_->engine->positionNow() : core::RationalTime{};
}

core::RationalTime AudioDeviceControl::playbackRateRatio() const noexcept {
    return !closing_ && state_->engine ? state_->engine->playbackRateRatio()
                                       : core::RationalTime::fromInteger(1);
}

bool AudioDeviceControl::isPlaying() const noexcept {
    return !closing_ && state_->engine && state_->engine->isPlaying();
}

void AudioDeviceControl::submitPending() {
    if (closing_ || active_ || !pending_)
        return;
    const Desired desired = desired_;
    runtime::TaskRequest request("Apply audio device state",
                                 {.kind = runtime::TaskOwnerKind::Application,
                                  .id = runtime::TaskOwnerId::fromRaw(315)},
                                 runtime::TaskPriority::Interactive,
                                 runtime::TaskExecutor::BlockingIo);
    request.sourceVersion.requestGeneration = desired.generation;
    auto submission = scheduler_.submit<CompletionPtr>(
        std::move(request), [state = state_, desired](runtime::TaskContext&) {
            auto result = std::make_shared<Completion>();
            result->generation = desired.generation;
            if (state->engine) {
                if (state->mixSerial != desired.mixSerial) {
                    state->engine->replaceClips(*desired.clips);
                    state->mixSerial = desired.mixSerial;
                }
                if (state->transportSerial != desired.transportSerial) {
                    if (desired.playing) {
                        result->transportStarted = true;
                        const auto status =
                            state->engine->play(desired.at, desired.silenceGeneration);
                        result->playing = !status.has_value();
                        result->error = status;
                    } else {
                        state->engine->stopAfterSilenceRequest();
                    }
                    state->transportSerial = desired.transportSerial;
                } else {
                    result->playing = desired.playing && state->engine->isPlaying();
                }
            }
            return runtime::TaskResult<CompletionPtr>::succeeded(CompletionPtr{std::move(result)});
        });
    if (!submission.accepted()) {
        if (submission.status != runtime::TaskSubmissionStatus::QueueFull) {
            pending_ = false;
            if (completionHandler_)
                completionHandler_(desired.generation, false, false);
        }
        return;
    }
    pending_ = false;
    active_.emplace(Active{std::move(submission.handle), false});
    bridge_.wake();
}

void AudioDeviceControl::consumeReady() {
    if (!active_)
        return;
    auto result = active_->handle.tryTakeResult();
    if (!result)
        return;
    const bool terminal = active_->terminal;
    active_.reset();
    if (terminal)
        return;
    if (!closing_) {
        if (result->state() == runtime::TaskState::Succeeded && result->value().has_value() &&
            *result->value() && (*result->value())->generation == desired_.generation &&
            completionHandler_) {
            if (const auto& error = (*result->value())->error; error)
                qWarning("Audio playback could not start (error %d, observed %llu)",
                         static_cast<int>(error->code),
                         static_cast<unsigned long long>(error->observed));
            completionHandler_(desired_.generation, (*result->value())->playing,
                               (*result->value())->transportStarted);
        } else if (!pending_ && completionHandler_) {
            completionHandler_(desired_.generation, false, false);
        }
        submitPending();
    } else {
        submitTerminal();
    }
}

void AudioDeviceControl::beginShutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_)
        return;
    requestSilence();
    closing_ = true;
    completionHandler_ = {};
    pending_ = false;
    shutdownDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (active_)
        active_->handle.cancel();
    else
        submitTerminal();
}

void AudioDeviceControl::submitTerminal() {
    if (!closing_ || active_ || fallbackRequired_ || state_->retired.load(std::memory_order_acquire))
        return;
    runtime::TaskRequest request("Retire audio device",
                                 {.kind = runtime::TaskOwnerKind::Application,
                                  .id = runtime::TaskOwnerId::fromRaw(315)},
                                 runtime::TaskPriority::Interactive,
                                 runtime::TaskExecutor::BlockingIo);
    auto submission = scheduler_.submit<CompletionPtr>(
        std::move(request), [state = state_](runtime::TaskContext&) {
            if (state->engine) {
                state->engine->stop();
                state->engine.reset();
            }
            state->retired.store(true, std::memory_order_release);
            return runtime::TaskResult<CompletionPtr>::succeeded(CompletionPtr{});
        });
    if (submission.accepted()) {
        active_.emplace(Active{std::move(submission.handle), true});
        bridge_.wake();
    } else if (submission.status != runtime::TaskSubmissionStatus::QueueFull) {
        fallbackRequired_ = true;
    }
}

bool AudioDeviceControl::retirementComplete() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!closing_)
        return false;
    if (state_->retired.load(std::memory_order_acquire))
        return true;
    consumeReady();
    if (!active_ && !fallbackRequired_)
        submitTerminal();
    if (!state_->retired.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() >= shutdownDeadline_)
        fallbackRequired_ = true;
    return state_->retired.load(std::memory_order_acquire) || fallbackRequired_;
}

void AudioDeviceControl::drainAfterEventLoop() noexcept {
    scheduler_.beginShutdown();
    while (!scheduler_.isQuiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (state_->engine) {
        auto state = state_;
        std::thread disposal([state] {
            state->engine->stop();
            state->engine.reset();
            state->retired.store(true, std::memory_order_release);
        });
        disposal.join();
    }
}

} // namespace bloom::ui
