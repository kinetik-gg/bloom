#pragma once

#include <bloom/ui/audio_device_control.hpp>

namespace bloom::app {

// Declared immediately after AudioDeviceControl and before its UI consumers. This guard runs
// only after the application event loop, including early setup returns and exceptions; the
// control and scheduler still exist while it drains any rejected terminal device cleanup.
class AudioDeviceProcessTeardown final {
  public:
    explicit AudioDeviceProcessTeardown(bloom::ui::AudioDeviceControl& control)
        : control_(control) {}
    ~AudioDeviceProcessTeardown() {
        control_.beginShutdown();
        control_.drainAfterEventLoop();
    }

  private:
    bloom::ui::AudioDeviceControl& control_;
};

} // namespace bloom::app
