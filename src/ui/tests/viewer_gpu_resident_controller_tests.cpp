// CPU-only tests for the GPU-resident viewer helper: the off-UI overlay raster
// (premultiplied, bounded, cancellation-safe) and the controller's
// inert/no-live-target semantics. The actual ViewerEditor and native device
// tests live in viewer_editor_resident_tests.cpp and
// viewer_gpu_resident_native_tests.cpp.

#include <bloom/ui/viewer_gpu_resident.hpp>
#include <bloom/ui/viewer_gpu_resident_overlay.hpp>

#include <bloom/render/gpu_present_image.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_resident_preview_product.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <QColor>
#include <QPainter>
#include <QPicture>
#include <QRectF>
#include <QSizeF>

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace bloom;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "Failure: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

void testOverlayRasterIsPremultiplied(Expectations& expectations) {
    QPicture picture;
    QPainter painter(&picture);
    painter.fillRect(QRectF(0.0, 0.0, 10.0, 10.0), QColor(255, 0, 0, 128));
    painter.end();

    ui::OverlayRasterRequest request;
    request.picture = picture;
    request.logicalSize = QSizeF(10.0, 10.0);
    request.width = 10;
    request.height = 10;
    request.token = 1;
    const auto overlay = ui::rasterizeResidentOverlay(request, [] { return false; });
    expectations.expect(overlay != nullptr, "a translucent overlay rasterizes");
    if (overlay == nullptr) {
        return;
    }
    // Premultiplied: 50% red is roughly (128, 0, 0, 128), never (255, 0, 0, 128).
    const std::uint8_t* pixel = overlay->pixels();
    expectations.expect(pixel[3] >= 120 && pixel[3] <= 136, "alpha is preserved");
    expectations.expect(pixel[0] >= 120 && pixel[0] <= 136,
                        "red is premultiplied by alpha, not left straight");
    expectations.expect(pixel[0] <= pixel[3], "premultiplied red never exceeds alpha");
}

void testOverlayRasterRejectsBadExtents(Expectations& expectations) {
    QPicture picture;
    QPainter painter(&picture);
    painter.fillRect(QRectF(0.0, 0.0, 4.0, 4.0), QColor(255, 255, 255, 255));
    painter.end();

    ui::OverlayRasterRequest zero;
    zero.picture = picture;
    zero.logicalSize = QSizeF(4.0, 4.0);
    zero.width = 0;
    zero.height = 4;
    zero.token = 1;
    expectations.expect(ui::rasterizeResidentOverlay(zero, [] { return false; }) == nullptr,
                        "a zero extent is rejected before allocation");

    ui::OverlayRasterRequest overBudget;
    overBudget.picture = picture;
    overBudget.logicalSize = QSizeF(4.0, 4.0);
    overBudget.width = 8192;
    overBudget.height = 8192;
    overBudget.token = 1;
    overBudget.byteBudget = 4ULL * 1024ULL * 1024ULL;
    expectations.expect(ui::rasterizeResidentOverlay(overBudget, [] { return false; }) == nullptr,
                        "an extent over the byte budget is rejected before allocation");

    ui::OverlayRasterRequest cancelled;
    cancelled.picture = picture;
    cancelled.logicalSize = QSizeF(4.0, 4.0);
    cancelled.width = 4;
    cancelled.height = 4;
    cancelled.token = 1;
    expectations.expect(ui::rasterizeResidentOverlay(cancelled, [] { return true; }) == nullptr,
                        "a cancelled raster returns no overlay");
}

// The private-storage builder refuses an invalid lease, so an invalid-lease
// resident frame can never reach the viewer from the product path. The viewer's
// stale/invalid-lease CPU fallback is exercised by the native device test.
void testResidentBuilderRefusesInvalidLease(Expectations& expectations) {
    runtime::PreviewRequestIdentity identity;
    identity.requestGeneration = 1;
    const runtime::ProcessFrameIdentity process =
        runtime::GpuResidentDisplayProductRequest{}.processIdentity;
    const auto built = runtime::detail::buildResidentPreviewFrame(
        identity, process, runtime::GpuResidentFrameLease{}, nullptr, {});
    expectations.expect(!built.has_value(), "the resident builder refuses an invalid lease (no "
                                            "invalid frame escapes)");
}

void testControllerNoDependenciesIsInert(Expectations& expectations) {
    ui::ViewerGpuResidentController controller;
    expectations.expect(!controller.configured(), "an unconfigured controller is inert");
    expectations.expect(!controller.hasLiveTarget(), "no live target without a client");
    expectations.expect(!controller.presentationAcknowledged(),
                        "nothing is acknowledged without a present");
    bool called = false;
    bool safe = false;
    const auto outcome = controller.prepareForMutation(
        11, [&](const std::uint64_t generation, const ui::EditorNativeSurface::PrepareResult& r) {
            called = true;
            safe = r.safeToMutate;
            expectations.expect(generation == 11, "the generation is echoed");
        });
    expectations.expect(outcome == ui::EditorNativeSurface::PrepareOutcome::NoLiveTarget,
                        "no live target means the host may mutate synchronously");
    expectations.expect(called && safe, "the completion reports safe-to-mutate");
}

void testOverlayDeviceExtentBounds(Expectations& expectations) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    constexpr std::uint64_t kBudget = ui::kResidentOverlayByteBudget;
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // Ordinary HiDPI is accepted.
    expectations.expect(ui::checkedOverlayDeviceExtent(320.0, 240.0, 2.0, kBudget, width, height) &&
                            width == 640U && height == 480U,
                        "an ordinary 2x HiDPI cover extent is accepted");

    // Fractional DPR uses ceil (device pixels cover the logical area)
    // consistently.
    expectations.expect(ui::checkedOverlayDeviceExtent(100.5, 50.25, 1.5, kBudget, width, height) &&
                            width == 151U && height == 76U,
                        "fractional DPR uses ceil device pixels");

    // Logical fits but the physical extent is over the budget: rejected before
    // allocation.
    expectations.expect(
        !ui::checkedOverlayDeviceExtent(3000.0, 3000.0, 2.0, kBudget, width, height),
        "a logical extent that fits but is physically oversized is rejected");

    // NaN / infinite / non-positive / zero inputs are rejected.
    expectations.expect(!ui::checkedOverlayDeviceExtent(320.0, 240.0, nan, kBudget, width, height),
                        "a NaN DPR is rejected");
    expectations.expect(!ui::checkedOverlayDeviceExtent(320.0, 240.0, inf, kBudget, width, height),
                        "an infinite DPR is rejected");
    expectations.expect(!ui::checkedOverlayDeviceExtent(inf, 240.0, 1.0, kBudget, width, height),
                        "an infinite logical size is rejected");
    expectations.expect(!ui::checkedOverlayDeviceExtent(320.0, 240.0, 0.0, kBudget, width, height),
                        "a zero DPR is rejected");
    expectations.expect(!ui::checkedOverlayDeviceExtent(0.0, 240.0, 1.0, kBudget, width, height),
                        "a zero logical size is rejected");

    // Extreme logical extent is rejected (over int/uint32 and the device-pixel
    // budget).
    expectations.expect(!ui::checkedOverlayDeviceExtent(1.0e9, 1.0e9, 1.0, kBudget, width, height),
                        "an extreme logical extent is rejected before any cast");
}

void testPresentSequenceAcknowledgement(Expectations& expectations) {
    // An older in-flight present advances presentCount but its applied sequence is below a newer
    // pending request: it must NOT acknowledge the newer one.
    expectations.expect(!ui::presentSequenceAcknowledged(1U, 2U),
                        "an older applied sequence never acknowledges a newer request");
    expectations.expect(!ui::presentSequenceAcknowledged(0U, 0U),
                        "no request sequence is never acknowledged");
    expectations.expect(ui::presentSequenceAcknowledged(2U, 2U),
                        "the current request's own sequence acknowledges it");
    expectations.expect(ui::presentSequenceAcknowledged(3U, 2U),
                        "a newer applied sequence acknowledges the request");
}

} // namespace

int main() {
    Expectations expectations;
    testOverlayRasterIsPremultiplied(expectations);
    testOverlayRasterRejectsBadExtents(expectations);
    testOverlayDeviceExtentBounds(expectations);
    testResidentBuilderRefusesInvalidLease(expectations);
    testControllerNoDependenciesIsInert(expectations);
    testPresentSequenceAcknowledgement(expectations);
    if (expectations.failures() == 0) {
        std::cout << "PASS: GPU-resident controller/overlay CPU semantics\n";
        return 0;
    }
    std::cerr << expectations.failures() << " failure(s)\n";
    return 1;
}
