#define MINIAUDIO_IMPLEMENTATION
#include "../../../third_party/miniaudio/miniaudio.h"

#include <bloom/media/audio/playback/audio_engine.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace bloom::media::audio::playback {

namespace {

struct CallbackState final {
    Backend::Callback callback = nullptr;
    void* userData = nullptr;
    std::uint32_t channels = 0;
};

void dataCallback(ma_device* device, void* output, const void*, const ma_uint32 frameCount) {
    auto& state = *static_cast<CallbackState*>(device->pUserData);
    const auto sampleCount = static_cast<std::size_t>(frameCount) * state.channels;
    state.callback(state.userData, std::span<float>{static_cast<float*>(output), sampleCount});
}

[[nodiscard]] AudioStatus backendError(const ma_result result) noexcept {
    return AudioStatus{
        AudioError{AudioErrorCode::BackendStartFailed, static_cast<std::uint64_t>(result), 0}};
}

} // namespace

class MiniaudioBackend::Impl final {
  public:
    ma_device device{};
    CallbackState callbackState{};
    bool initialized = false;
};

MiniaudioBackend::MiniaudioBackend() : impl_(std::make_unique<Impl>()) {}

MiniaudioBackend::~MiniaudioBackend() { stop(); }

MiniaudioBackend::MiniaudioBackend(MiniaudioBackend&& other) noexcept = default;

MiniaudioBackend& MiniaudioBackend::operator=(MiniaudioBackend&& other) noexcept = default;

AudioStatus MiniaudioBackend::start(const std::uint32_t rate, const std::uint32_t channels,
                                    const Backend::Callback callback, void* userData) noexcept {
    if (rate == 0 || channels == 0 || callback == nullptr) {
        return AudioStatus{AudioError::codeOnly(AudioErrorCode::InvalidAudioFormat)};
    }
    stop();
    impl_->callbackState.callback = callback;
    impl_->callbackState.userData = userData;
    impl_->callbackState.channels = channels;

    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = channels;
    config.sampleRate = rate;
    config.dataCallback = &dataCallback;
    config.pUserData = &impl_->callbackState;
    const auto initResult = ma_device_init(nullptr, &config, &impl_->device);
    if (initResult != MA_SUCCESS) {
        impl_->callbackState = CallbackState{};
        return backendError(initResult);
    }
    impl_->initialized = true;
    const auto startResult = ma_device_start(&impl_->device);
    if (startResult != MA_SUCCESS) {
        stop();
        return backendError(startResult);
    }
    return std::nullopt;
}

void MiniaudioBackend::stop() noexcept {
    if (!impl_ || !impl_->initialized) {
        return;
    }
    ma_device_uninit(&impl_->device);
    impl_->initialized = false;
    impl_->callbackState = CallbackState{};
}

std::unique_ptr<Backend> makeMiniaudioBackend() { return std::make_unique<MiniaudioBackend>(); }

} // namespace bloom::media::audio::playback
