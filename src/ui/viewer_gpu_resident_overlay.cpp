#include <bloom/ui/viewer_gpu_resident_overlay.hpp>

#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace bloom::ui {

bool checkedOverlayDeviceExtent(const double logicalWidth, const double logicalHeight,
                                const double devicePixelRatio, const std::uint64_t byteBudget,
                                std::uint32_t& deviceWidth, std::uint32_t& deviceHeight) noexcept {
    if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) ||
        !std::isfinite(devicePixelRatio) || !(logicalWidth > 0.0) || !(logicalHeight > 0.0) ||
        !(devicePixelRatio > 0.0)) {
        return false;
    }
    const double widthValue = std::ceil(logicalWidth * devicePixelRatio);
    const double heightValue = std::ceil(logicalHeight * devicePixelRatio);
    const double maxUint32 = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    const double maxInt = static_cast<double>(std::numeric_limits<int>::max());
    if (!std::isfinite(widthValue) || !std::isfinite(heightValue) || widthValue < 1.0 ||
        heightValue < 1.0 || widthValue > maxUint32 || heightValue > maxUint32 ||
        widthValue > maxInt || heightValue > maxInt) {
        return false;
    }
    const std::uint64_t width = static_cast<std::uint64_t>(widthValue);
    const std::uint64_t height = static_cast<std::uint64_t>(heightValue);
    // Both are <= INT_MAX, so width*height cannot overflow uint64.
    if (width * height > byteBudget / 4U) {
        return false;
    }
    deviceWidth = static_cast<std::uint32_t>(width);
    deviceHeight = static_cast<std::uint32_t>(height);
    return true;
}

std::shared_ptr<const runtime::GpuPresentationOverlay>
rasterizeResidentOverlay(const OverlayRasterRequest& request,
                         const std::function<bool()>& cancellationRequested) {
    const auto cancelled = [&cancellationRequested] {
        return cancellationRequested && cancellationRequested();
    };
    if (request.token == 0U || request.width == 0U || request.height == 0U) {
        return nullptr;
    }
    constexpr std::uint32_t kMaxExtent =
        static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (request.width > kMaxExtent || request.height > kMaxExtent) {
        return nullptr;
    }
    // Checked byte accounting BEFORE any allocation. width*height cannot overflow
    // uint64 for two int-sized extents, and dividing the budget first avoids the
    // x4 overflow.
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(request.width) * static_cast<std::uint64_t>(request.height);
    if (pixelCount == 0U || pixelCount > request.byteBudget / 4U) {
        return nullptr;
    }
    const std::uint64_t byteSize = pixelCount * 4U;
    if (byteSize > request.byteBudget ||
        byteSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return nullptr;
    }
    if (cancelled()) {
        return nullptr;
    }

    // ONE owning buffer: the QImage borrows this vector's storage, so
    // rasterization never allocates a second full-frame image. The vector is then
    // moved into the overlay, so the peak is a single buffer.
    std::vector<std::uint8_t> pixels;
    try {
        pixels.resize(static_cast<std::size_t>(byteSize));
    } catch (...) {
        return nullptr;
    }
    if (pixels.size() != static_cast<std::size_t>(byteSize)) {
        return nullptr;
    }

    {
        // Premultiplied by construction: this is the exact format
        // GpuPresentationOverlay documents (the presenter composites it
        // source-over).
        QImage image(pixels.data(), static_cast<int>(request.width),
                     static_cast<int>(request.height), static_cast<qsizetype>(request.width) * 4,
                     QImage::Format_RGBA8888_Premultiplied);
        if (image.isNull()) {
            return nullptr;
        }
        image.fill(Qt::transparent);
        if (!request.picture.isNull() && request.logicalSize.width() > 0.0 &&
            request.logicalSize.height() > 0.0) {
            QPainter painter(&image);
            if (!painter.isActive()) {
                return nullptr;
            }
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.scale(static_cast<double>(request.width) / request.logicalSize.width(),
                          static_cast<double>(request.height) / request.logicalSize.height());
            painter.drawPicture(QPointF(0.0, 0.0), request.picture);
            painter.end();
        }
        if (cancelled()) {
            return nullptr;
        }
    }

    return runtime::GpuPresentationOverlay::create(
        std::move(pixels), request.width, request.height,
        static_cast<std::uint32_t>(static_cast<std::size_t>(request.width) * 4U), request.token,
        request.byteBudget);
}

} // namespace bloom::ui
