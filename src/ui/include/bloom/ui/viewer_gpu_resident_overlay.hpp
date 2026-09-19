#pragma once

// Off-UI overlay raster helper for the GPU-resident viewer route.
//
// The UI thread records one immutable QPicture of the viewer overlays (safe
// areas, thirds, rulers, pixel grid, selected-layer handles/paths, ROI,
// creation/text). This helper replays that picture into ONE premultiplied RGBA8
// buffer on the existing TaskScheduler worker and wraps it in the runtime's
// immutable GpuPresentationOverlay. It reads no widget and no session state, so
// it is safe off the UI thread.
//
// The overlay is premultiplied by construction: the buffer is rasterized into
// QImage::Format_RGBA8888_Premultiplied, exactly matching
// GpuPresentationOverlay's documented premultiplied contract (the presenter
// composes it with source-over).

#include <bloom/runtime/gpu_presentation_coordinator.hpp>

#include <QPicture>
#include <QSizeF>

#include <cstdint>
#include <functional>
#include <memory>

namespace bloom::ui {

// The largest overlay this viewer will rasterize. Bounded so a malformed extent
// can never allocate an unbounded buffer; the coordinator enforces its own
// per-frame budget as well.
inline constexpr std::uint64_t kResidentOverlayByteBudget = 64ULL * 1024ULL * 1024ULL;

// Validates a logical size and device pixel ratio into a bounded physical
// device extent, with the same ceil/fractional semantics the raster uses.
// Rejects a non-finite or non-positive size/ratio, an int/uint32 overflow, and
// a device pixel count over `byteBudget / 4`. On refusal the outputs are left
// untouched. Shared by the overlay raster and the CPU cover snapshot.
[[nodiscard]] bool checkedOverlayDeviceExtent(double logicalWidth, double logicalHeight,
                                              double devicePixelRatio, std::uint64_t byteBudget,
                                              std::uint32_t& deviceWidth,
                                              std::uint32_t& deviceHeight) noexcept;

struct OverlayRasterRequest final {
    QPicture picture;
    // The logical coordinate space the picture was recorded in. Never zero for a
    // valid request.
    QSizeF logicalSize;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Overlay generation identity; 0 means "no overlay" and is rejected.
    std::uint64_t token = 0;
    std::uint64_t byteBudget = kResidentOverlayByteBudget;
};

// Replays `request.picture` into a premultiplied RGBA8 buffer and wraps it.
// Returns nullptr (never throws) for: a non-positive extent, an extent that
// overflows int/checked byte accounting, a byte size over `byteBudget`, an
// allocation failure, a cancelled worker, or a malformed output. The caller
// keeps the previous good overlay / CPU paint on nullptr.
[[nodiscard]] std::shared_ptr<const runtime::GpuPresentationOverlay>
rasterizeResidentOverlay(const OverlayRasterRequest& request,
                         const std::function<bool()>& cancellationRequested);

} // namespace bloom::ui
