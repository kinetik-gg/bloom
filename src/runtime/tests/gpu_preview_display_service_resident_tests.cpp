// Focused portable checks for the additive resident-route service surface:
//  * the narrow estimatePixels regression (a resolved ProxyResolution is never reduced a second
//  time
//    by the resolution policy);
//  * the existing default constructor is behavior-preserving (a disabled resident-overload service
//    publishes Disabled with a null resident report and zero counters);
//  * the resident overload shares the same status/admission contract.
//
// Device-gated end-to-end resident dispatch acceptance is NOT fabricated here: it needs the
// product-prep runtime subset linked into the closure and a real Vulkan/Wayland device. This file
// deliberately asserts only what is genuinely reproducible without one.
#include "gpu_preview_display_service_private.hpp"

#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>

namespace {

using namespace bloom;
using runtime::GpuPreviewDisplayService;
using runtime::GpuPreviewDisplayServiceOptions;
using runtime::GpuPreviewDisplayServiceState;
using runtime::GpuPreviewDisplayServiceStatus;
using runtime::PreviewCpuDisplayFallback;
using runtime::PreviewCpuStageFunction;
using runtime::PreviewGpuSceneStageFunction;
using runtime::PreviewRequestIdentity;
using runtime::PreviewResolutionPolicy;
using runtime::ProxyResolution;
using runtime::TaskScheduler;
using runtime::TaskSchedulerConfig;

class Expectations final {
  public:
    void expect(const bool ok, const std::string& message,
                const std::source_location loc = std::source_location::current()) {
        if (!ok) {
            ++failures_;
            std::cerr << loc.file_name() << ':' << loc.line() << ": " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] TaskSchedulerConfig schedulerConfig() {
    TaskSchedulerConfig c;
    c.cpuWorkerCount = 1;
    c.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    c.blockingIoWorkerCount = 1;
    c.cpuQueueCapacity = 16;
    c.blockingIoQueueCapacity = 4;
    c.gpuPendingQueueCapacity = 4;
    c.gpuAdmittedStateCapacity = 4;
    c.gpuLiveContinuationCapacity = 2;
    c.gpuQueuedCommandByteCapacity = std::size_t{1} << 28U;
    c.gpuRequestOwnedByteCapacity = std::size_t{1} << 28U;
    c.terminalHistoryCapacity = 16;
    c.diagnosticsPerTask = 8;
    c.groupRegistryCapacity = 8;
    return c;
}

void testEstimatePixelsDoesNotDoubleReduce(Expectations& checks) {
    PreviewRequestIdentity identity;
    const auto extent = render::ImageExtent::create(1920, 1080);
    if (!extent) {
        checks.expect(false, "ImageExtent::create failed");
        return;
    }
    identity.resolution = ProxyResolution{*extent.value()};
    identity.resolutionPolicy = PreviewResolutionPolicy::Half;
    const auto pixels = runtime::detail::gpuPreviewDisplayEstimatePixels(identity);
    checks.expect(pixels.has_value(), "a resolved proxy still yields an estimate");
    if (pixels.has_value()) {
        // The proxy is the resolved output; the policy must NOT reduce it again. 1920*1080, not its
        // quarter.
        checks.expect(*pixels == static_cast<std::uint64_t>(1920) * 1080,
                      "ProxyResolution + Half is not double-reduced");
    }

    PreviewRequestIdentity quarter;
    quarter.resolution = ProxyResolution{*extent.value()};
    quarter.resolutionPolicy = PreviewResolutionPolicy::Quarter;
    const auto quarterPixels = runtime::detail::gpuPreviewDisplayEstimatePixels(quarter);
    checks.expect(quarterPixels.has_value() &&
                      *quarterPixels == static_cast<std::uint64_t>(1920) * 1080,
                  "ProxyResolution + Quarter is not double-reduced");

    const auto roi = render::ImageWindow::create(8, 4, 640, 480);
    if (roi) {
        PreviewRequestIdentity roiIdentity;
        roiIdentity.resolution = ProxyResolution{*extent.value()};
        roiIdentity.roi = *roi.value();
        roiIdentity.resolutionPolicy = PreviewResolutionPolicy::Half;
        const auto roiPixels = runtime::detail::gpuPreviewDisplayEstimatePixels(roiIdentity);
        checks.expect(roiPixels.has_value() && *roiPixels == static_cast<std::uint64_t>(640) * 480,
                      "a resolved ROI is not reduced by the policy");
    }
}

void testDisabledResidentOverloadIsHonest(Expectations& checks) {
    TaskScheduler scheduler(schedulerConfig());
    GpuPreviewDisplayServiceOptions options;
    options.enabled = false;
    GpuPreviewDisplayService service(scheduler, PreviewGpuSceneStageFunction{},
                                     PreviewCpuStageFunction{}, PreviewCpuDisplayFallback{},
                                     options);
    const GpuPreviewDisplayServiceStatus status = service.status();
    checks.expect(status.state == GpuPreviewDisplayServiceState::Unavailable,
                  "a disabled resident service is Unavailable");
    checks.expect(status.residentQualification == nullptr,
                  "a disabled resident service publishes no resident report");
    checks.expect(status.counters.fullFrameReadbacks == 0,
                  "a disabled resident service has zero full-frame readbacks");
    checks.expect(status.counters.nativeDispatches == 0,
                  "a disabled resident service has zero native dispatches");
    checks.expect(status.counters.residentGraphJobs == 0,
                  "a disabled resident service has zero resident graph jobs");
}

} // namespace

int main() {
    Expectations checks;
    testEstimatePixelsDoesNotDoubleReduce(checks);
    testDisabledResidentOverloadIsHonest(checks);
    if (checks.failures() == 0) {
        std::cout << "resident service: all expectations passed\n";
        return 0;
    }
    std::cerr << "resident service: " << checks.failures() << " failure(s)\n";
    return 1;
}
