#pragma once

// What the Export dialog knows about H.264 (lane C). The platform's encoder comes from the
// export provider (output::h264EncoderSupportV1(), a pure answer); everything that touches the
// disk -- verifying the installed OpenH264 binary (a SHA-256), looking for a VA-API render node,
// and Locate's hash-and-copy of a downloaded binary -- runs as a task on the shared scheduler and
// reaches the dialog through this service's published state. Nothing here hashes, copies or lists
// a directory on the UI thread.

#include <bloom/output/media_output.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace bloom::ui {

class TaskUiBridge;

// A worker's look at the machine.
struct H264EncoderProbe final {
    bool vaapiDevice = false;
    bool openh264Installed = false;
    std::string detail;
};

class H264EncoderService final : public QObject {
    Q_OBJECT

  public:
    // Whether the software encoder still needs OpenH264, and what the last check found.
    enum class Runtime : std::uint8_t {
        // VideoToolbox or no software encoder: nothing to install.
        NotNeeded,
        Checking,
        Installed,
        NotInstalled,
    };
    struct State final {
        output::H264EncoderSupportV1 support;
        bool vaapiDevice = false;
        Runtime runtime = Runtime::Checking;
        QString detail;
        // True once the first worker probe has answered.
        bool probed = false;
    };
    // Both run on a worker. The defaults call the output layer; tests inject their own.
    using Probe = std::function<H264EncoderProbe(const output::H264EncoderSupportV1&)>;
    using Locate = std::function<output::H264RuntimeAvailabilityV1(
        const std::filesystem::path&, const std::function<bool()>& cancelled)>;

    H264EncoderService(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                       output::H264EncoderSupportV1 support = output::h264EncoderSupportV1(),
                       Probe probe = {}, Locate locate = {}, QObject* parent = nullptr);
    ~H264EncoderService() override;

    [[nodiscard]] const State& state() const noexcept { return state_; }
    // Starts a worker probe unless one is running. The dialog calls it on open.
    void refresh();
    // Starts Locate on a worker; false while one is already running.
    bool locate(const std::filesystem::path& file);
    void cancelLocate();
    [[nodiscard]] bool locating() const noexcept { return locateTask_.has_value(); }
    [[nodiscard]] bool probing() const noexcept { return probeTask_.has_value(); }

  Q_SIGNALS:
    void stateChanged();
    void locateFinished(bool installed, const QString& detail);

  private:
    void poll();

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    Probe probe_;
    Locate locate_;
    State state_;
    std::optional<runtime::TaskHandle<std::shared_ptr<H264EncoderProbe>>> probeTask_;
    std::optional<runtime::TaskHandle<std::shared_ptr<output::H264RuntimeAvailabilityV1>>>
        locateTask_;
};

} // namespace bloom::ui
