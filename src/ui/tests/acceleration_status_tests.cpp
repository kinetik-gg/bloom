#include <bloom/ui/acceleration_status.hpp>

#include <QApplication>
#include <QString>

#include <iostream>
#include <source_location>
#include <string_view>

namespace {

// The small "Expectations" idiom every Bloom test file already uses.
class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
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

namespace ui = bloom::ui;
namespace render = bloom::render;
namespace rt = bloom::runtime;

[[nodiscard]] rt::GpuPreviewDisplayServiceStatus
serviceStatus(const rt::GpuPreviewDisplayServiceState state) {
    rt::GpuPreviewDisplayServiceStatus status;
    status.state = state;
    return status;
}

void testCpuOnlyDefault(Expectations& check) {
    const auto status = ui::cpuOnlyAccelerationStatus();
    check.expect(status.backend == QStringLiteral("CPU reference"),
                 "the CPU-only default names the CPU reference backend");
    check.expect(status.deviceState == QStringLiteral("Unavailable"),
                 "the CPU-only default is Unavailable, not Ready");
    check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                 "the CPU-only default takes no GPU route");
    check.expect(status.summary.contains(QStringLiteral("unavailable")) &&
                     !status.summary.contains(QStringLiteral("not built")),
                 "the CPU-only default says unavailable, never that the build lacks GPU support");
}

void testUnavailableDisabled(Expectations& check) {
    auto service = serviceStatus(rt::GpuPreviewDisplayServiceState::Unavailable);
    service.diagnostic.code = rt::GpuPreviewDisplayServiceDiagnosticCode::Disabled;
    service.diagnostic.message = "The GPU preview display service is disabled.";

    const auto status = ui::accelerationStatusFromServiceStatus(service);
    check.expect(status.backend == QStringLiteral("CPU reference"),
                 "a disabled service reports the CPU reference backend");
    check.expect(status.deviceState == QStringLiteral("Unavailable"),
                 "a disabled service reports Unavailable");
    check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                 "a disabled service routes to the CPU fallback");
    check.expect(status.deviceName.isEmpty() && status.driver.isEmpty(),
                 "no device text is invented without a qualification report");
    check.expect(status.summary.contains(QStringLiteral("disabled")) &&
                     !status.summary.contains(QStringLiteral("not built")),
                 "a disabled service says disabled, never that the backend is not built");
}

void testUnavailableLoaderMissing(Expectations& check) {
    auto service = serviceStatus(rt::GpuPreviewDisplayServiceState::Unavailable);
    service.diagnostic.code = rt::GpuPreviewDisplayServiceDiagnosticCode::LoaderUnavailable;

    const auto status = ui::accelerationStatusFromServiceStatus(service);
    check.expect(status.backend == QStringLiteral("CPU reference"),
                 "a missing loader reports the CPU reference backend");
    check.expect(status.summary.contains(QStringLiteral("loader")) &&
                     !status.summary.contains(QStringLiteral("not built")),
                 "a missing loader says loader unavailable, never that the backend is not built");
}

void testInitializing(Expectations& check) {
    const auto service = serviceStatus(rt::GpuPreviewDisplayServiceState::Initializing);
    const auto status = ui::accelerationStatusFromServiceStatus(service);
    check.expect(status.backend == QStringLiteral("Vulkan (initializing)"),
                 "initializing reports the Vulkan backend as initializing");
    check.expect(status.deviceState == QStringLiteral("Initializing"),
                 "initializing reports the Initializing device state");
    check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                 "nothing routes to the GPU before qualification");
    check.expect(status.summary.contains(QStringLiteral("Checking GPU acceleration")),
                 "initializing explains that it is still checking");
}

void testReadyWithoutResidentReport(Expectations& check) {
    auto service = serviceStatus(rt::GpuPreviewDisplayServiceState::Ready);
    service.gpuAvailable = true;

    const auto status = ui::accelerationStatusFromServiceStatus(service);
    check.expect(status.backend == QStringLiteral("Vulkan"),
                 "Ready reports Vulkan as a capability");
    check.expect(status.deviceState == QStringLiteral("Ready"), "Ready reports the Ready state");
    check.expect(status.deviceName.isEmpty() && status.driver.isEmpty(),
                 "Ready alone never invents a device name or driver");
    check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                 "Ready without an eligible report is not a resident/presenting claim");
    check.expect(status.operationStatus.isEmpty(),
                 "no eligible report means no operation line is claimed");
    check.expect(status.summary.contains(QStringLiteral("no eligible resident preview route")),
                 "Ready without an eligible report says so honestly");
}

void testReadyNoPresentationGeneration(Expectations& check) {
    auto service = serviceStatus(rt::GpuPreviewDisplayServiceState::Ready);
    service.gpuAvailable = true;
    // Requested, but the generation has no usable client: capability is retained, presentation is
    // not. This must read as a CPU fallback, never GPU-active.
    service.presentationAvailability = render::GpuPresentationAvailability::Ready;

    const auto status = ui::accelerationStatusFromServiceStatus(service);
    check.expect(status.backend == QStringLiteral("Vulkan"),
                 "device capability is retained when presentation has no client");
    check.expect(status.presentationStatus == QStringLiteral("Ready (no service client)"),
                 "a Ready capability without a client is reported as such");
    check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                 "a Ready capability with no client never claims presenting");
}

void testLostPresentationCapability(Expectations& check) {
    auto service = serviceStatus(rt::GpuPreviewDisplayServiceState::Ready);
    service.gpuAvailable = true;
    service.presentationAvailability = render::GpuPresentationAvailability::Unavailable;
    service.presentationDetail = "the required surface capability is absent";

    const auto status = ui::accelerationStatusFromServiceStatus(service);
    check.expect(status.presentationStatus == QStringLiteral("Unavailable"),
                 "an unavailable presentation generation is reported as unavailable");
    check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                 "a lost presentation capability keeps the CPU route");
    check.expect(status.deviceName.isEmpty() && status.driver.isEmpty(),
                 "a lost presentation capability does not invent device text");
    // Without a genuine resident report the route is already the CPU fallback; the summary must
    // still not claim GPU presentation is available.
    check.expect(status.summary.contains(QStringLiteral("CPU reference")) &&
                     !status.summary.contains(QStringLiteral("available for eligible previews")),
                 "a lost presentation capability never claims an available GPU presentation");
}

void testUnavailableAfterDeviceLoss(Expectations& check) {
    auto service = serviceStatus(rt::GpuPreviewDisplayServiceState::Unavailable);
    service.gpuAvailable = false;
    service.diagnostic.code = rt::GpuPreviewDisplayServiceDiagnosticCode::DeviceUnavailable;

    const auto status = ui::accelerationStatusFromServiceStatus(service);
    check.expect(status.backend == QStringLiteral("CPU reference"),
                 "a device loss reports the CPU reference backend");
    check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                 "a device loss routes to the CPU fallback");
    check.expect(status.summary.contains(QStringLiteral("unavailable")) &&
                     !status.summary.contains(QStringLiteral("not built")),
                 "a device loss says unavailable, never that the backend is not built");
}

void testShuttingDown(Expectations& check) {
    for (const auto state : {rt::GpuPreviewDisplayServiceState::Stopping,
                             rt::GpuPreviewDisplayServiceState::Stopped}) {
        const auto service = serviceStatus(state);
        const auto status = ui::accelerationStatusFromServiceStatus(service);
        check.expect(status.previewRoute == QStringLiteral("CPU fallback"),
                     "a shutting-down service routes to the CPU fallback");
        check.expect(status.summary.contains(QStringLiteral("shutting down")),
                     "a shutting-down service says it is shutting down");
        check.expect(status.backend == QStringLiteral("CPU reference"),
                     "a shutting-down service reports the CPU reference backend");
    }
}

void testCachedProvider(Expectations& check) {
    ui::CachedAccelerationStatusProvider provider;
    check.expect(provider.accelerationStatus() == ui::cpuOnlyAccelerationStatus(),
                 "a fresh cached provider reports the CPU-only truth");

    provider.setServiceStatus(serviceStatus(rt::GpuPreviewDisplayServiceState::Initializing));
    check.expect(provider.accelerationStatus().deviceState == QStringLiteral("Initializing"),
                 "the cached provider updates from one published status read");

    auto ready = serviceStatus(rt::GpuPreviewDisplayServiceState::Ready);
    ready.gpuAvailable = true;
    provider.setServiceStatus(ready);
    const auto cached = provider.accelerationStatus();
    check.expect(cached.backend == QStringLiteral("Vulkan") &&
                     cached.deviceState == QStringLiteral("Ready"),
                 "a later status replaces the cached value synchronously");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations check;
    testCpuOnlyDefault(check);
    testUnavailableDisabled(check);
    testUnavailableLoaderMissing(check);
    testInitializing(check);
    testReadyWithoutResidentReport(check);
    testReadyNoPresentationGeneration(check);
    testLostPresentationCapability(check);
    testUnavailableAfterDeviceLoss(check);
    testShuttingDown(check);
    testCachedProvider(check);
    return check.failures() == 0 ? 0 : 1;
}
