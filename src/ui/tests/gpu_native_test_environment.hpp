#pragma once

// Pre-QApplication environment gate shared by the native Wayland presentation fixtures
// (gpu_presentation_coordinator_tests.cpp, gpu_preview_display_service_presentation_tests.cpp,
// gpu_preview_display_service_resident_tests.cpp, viewer_gpu_presenter_fake_port_tests.cpp, and
// viewer_gpu_presenter_native_tests.cpp).
//
// Each of those fixtures constructs QGuiApplication/QApplication and then asks Qt for a Wayland
// QWindow/VkSurfaceKHR. On a headless runner with QT_QPA_PLATFORM=wayland and no compositor Qt
// aborts inside platform-plugin initialization, before the fixture can report the absent
// device/capability. Inspecting the environment first lets the fixture exit with the CTest skip
// code instead, while an externally forced offscreen platform can never stand in for real native
// acceptance. The helper only reads the process environment; it never mutates it.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace bloom::ui::test {

// Matches the SKIP_RETURN_CODE the five native Wayland CTest fixtures are registered with, so CTest
// reports them as Skipped rather than Passed on a runner that cannot host a compositor.
inline constexpr int kNativeWaylandSkipCode = 77;

class NativeWaylandEnvironment final {
  public:
    // Inspects QT_QPA_PLATFORM, WAYLAND_DISPLAY, and XDG_RUNTIME_DIR. No Qt object is created, so
    // this is safe to call before QGuiApplication/QApplication.
    [[nodiscard]] static NativeWaylandEnvironment inspect() {
        NativeWaylandEnvironment environment;
        const char* platform = std::getenv("QT_QPA_PLATFORM");
        const std::string_view platform_value =
            platform == nullptr ? std::string_view{} : std::string_view{platform};
        // Qt accepts a semicolon-separated preference list and selects the first entry. The
        // registered fixtures pin exactly "wayland", so anything whose primary plugin is not
        // Wayland (an externally forced "offscreen", in particular) must not be reported as native
        // success.
        const std::size_t separator = platform_value.find(';');
        const std::string_view primary = separator == std::string_view::npos
                                             ? platform_value
                                             : platform_value.substr(0, separator);
        if (primary != "wayland") {
            environment.reason_ =
                "QT_QPA_PLATFORM primary plugin is \"" + std::string(primary) +
                "\", not \"wayland\"; refusing to claim native Wayland acceptance";
            return environment;
        }
        const char* display = std::getenv("WAYLAND_DISPLAY");
        if (display == nullptr || *display == '\0') {
            environment.reason_ = "WAYLAND_DISPLAY is unset; no Wayland compositor is reachable";
            return environment;
        }
        std::filesystem::path socket(display);
        if (!socket.is_absolute()) {
            const char* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
            if (runtime_directory == nullptr || *runtime_directory == '\0') {
                environment.reason_ = "XDG_RUNTIME_DIR is unset; cannot resolve WAYLAND_DISPLAY=" +
                                      std::string(display);
                return environment;
            }
            socket = std::filesystem::path(runtime_directory) / socket;
        }
        std::error_code error;
        if (!std::filesystem::exists(socket, error) || error) {
            environment.reason_ = "the Wayland socket \"" + socket.string() + "\" does not exist";
            return environment;
        }
        environment.available_ = true;
        return environment;
    }

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    // The process exit status that honestly represents an unavailable native environment: a failure
    // (1) when the caller demanded a device through --require-device, otherwise the CTest skip
    // code. Prints the reason to the matching stream.
    [[nodiscard]] int exitStatus(const bool require_device) const {
        if (require_device) {
            std::cerr << "FAIL: native Wayland environment unavailable: " << reason_ << '\n';
            return 1;
        }
        std::cout << "SKIP: native Wayland environment unavailable: " << reason_ << '\n';
        return kNativeWaylandSkipCode;
    }

  private:
    bool available_ = false;
    std::string reason_;
};

} // namespace bloom::ui::test
