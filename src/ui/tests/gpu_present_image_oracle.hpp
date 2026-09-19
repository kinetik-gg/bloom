#pragma once

// Independent QPainter oracle for the present shader. It never mirrors the shader's GLSL algebra:
// it hands the exact resident-display RGBA8 bytes to QPainter and asks QPainter to resample and
// composite them under SmoothPixmapTransform (and nearest, for integer mappings), then the test
// compares the GPU attachment readback against QPainter's own output. Qt-only; render-side test TU.

#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/image_types.hpp>

#include <QBrush>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bloom::render::present_oracle {

using bloom::render::Rgba8;

struct OracleImage final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Rgba8> pixels;
};

[[nodiscard]] inline std::uint8_t toByte(const float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

[[nodiscard]] inline QColor toColor(const GpuPresentColor color) noexcept {
    return QColor(static_cast<int>(toByte(color.red)), static_cast<int>(toByte(color.green)),
                  static_cast<int>(toByte(color.blue)), static_cast<int>(toByte(color.alpha)));
}

[[nodiscard]] inline QImage imageFrom(const std::vector<Rgba8>& pixels, const std::uint32_t width,
                                      const std::uint32_t height,
                                      const QImage::Format format) noexcept {
    const auto* bytes = reinterpret_cast<const uchar*>(pixels.data());
    const QImage view(bytes, static_cast<int>(width), static_cast<int>(height),
                      static_cast<int>(width) * 4, format);
    return view.copy();
}

// Applies the viewer's channel remap to a straight RGBA8 image, exactly as the viewer does before
// compositing. This is a data transform, not the shader's filtering algebra.
[[nodiscard]] inline QImage remapChannel(const QImage& source, const GpuPresentChannel channel) {
    if (channel == GpuPresentChannel::Rgba) {
        return source;
    }
    QImage mapped = source.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < mapped.height(); ++y) {
        for (int x = 0; x < mapped.width(); ++x) {
            const QColor c = mapped.pixelColor(x, y);
            int r = c.red();
            int g = c.green();
            int b = c.blue();
            switch (channel) {
            case GpuPresentChannel::Rgb:
                break;
            case GpuPresentChannel::Red:
                g = r;
                b = r;
                break;
            case GpuPresentChannel::Green:
                r = g;
                b = g;
                break;
            case GpuPresentChannel::Blue:
                r = b;
                g = b;
                break;
            case GpuPresentChannel::Alpha:
                r = c.alpha();
                g = c.alpha();
                b = c.alpha();
                break;
            case GpuPresentChannel::Rgba:
            default:
                break;
            }
            mapped.setPixelColor(x, y, QColor(r, g, b, 255));
        }
    }
    return mapped;
}

[[nodiscard]] inline QImage checkerTile(const GpuPresentColor a, const GpuPresentColor b,
                                        const int tile) {
    const int span = 2 * tile;
    QImage checker(span, span, QImage::Format_RGBA8888);
    for (int y = 0; y < span; ++y) {
        for (int x = 0; x < span; ++x) {
            const int cx = x / tile;
            const int cy = y / tile;
            const bool raised = ((cx + cy) & 1) == 0;
            checker.setPixelColor(x, y, raised ? toColor(b) : toColor(a));
        }
    }
    return checker;
}

// Renders the oracle target with QPainter. `overlay` is empty for no overlay; otherwise it is a
// premultiplied RGBA8 overlay of overlayWidth x overlayHeight covering the whole target.
[[nodiscard]] inline OracleImage
renderOracle(const std::vector<Rgba8>& display, const std::uint32_t displayWidth,
             const std::uint32_t displayHeight, const GpuPresentImageParams& params,
             const std::vector<Rgba8>& overlay, const std::uint32_t overlayWidth,
             const std::uint32_t overlayHeight, const bool smooth) {
    const int targetWidth = static_cast<int>(params.targetWidth);
    const int targetHeight = static_cast<int>(params.targetHeight);
    QImage target(targetWidth, targetHeight, QImage::Format_RGBA8888);
    target.fill(QColor(0, 0, 0, 255));

    if (params.background == GpuPresentBackground::Black) {
        target.fill(QColor(0, 0, 0, 255));
    } else if (params.background == GpuPresentBackground::White) {
        target.fill(QColor(255, 255, 255, 255));
    } else if (params.background == GpuPresentBackground::Checkerboard) {
        const int tile = std::max(1, static_cast<int>(std::lround(params.checkerTilePixels)));
        const QImage checker = checkerTile(params.checkerColorA, params.checkerColorB, tile);
        QPainter backgroundPainter(&target);
        backgroundPainter.setBrushOrigin(
            QPointF(static_cast<double>(params.checkerOriginX) - 2.0 * static_cast<double>(tile),
                    static_cast<double>(params.checkerOriginY) - 2.0 * static_cast<double>(tile)));
        backgroundPainter.fillRect(QRectF(0.0, 0.0, targetWidth, targetHeight), QBrush(checker));
    } else {
        target.fill(toColor(params.backgroundColor));
    }

    QPainter painter(&target);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);

    const QImage sourceStraight =
        imageFrom(display, displayWidth, displayHeight, QImage::Format_RGBA8888);
    const QImage source = remapChannel(sourceStraight, params.channel);
    if (params.destination.width > 0.0F && params.destination.height > 0.0F &&
        params.source.width > 0.0 && params.source.height > 0.0) {
        const QRectF destination(static_cast<double>(params.destination.x),
                                 static_cast<double>(params.destination.y),
                                 static_cast<double>(params.destination.width),
                                 static_cast<double>(params.destination.height));
        const QRectF sourceRect(params.source.x, params.source.y, params.source.width,
                                params.source.height);
        painter.drawImage(destination, source, sourceRect);
    }

    if (!overlay.empty()) {
        const QImage overlayImage =
            imageFrom(overlay, overlayWidth, overlayHeight, QImage::Format_RGBA8888_Premultiplied);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(QRectF(0.0, 0.0, targetWidth, targetHeight), overlayImage,
                          QRectF(0.0, 0.0, overlayWidth, overlayHeight));
    }
    painter.end();

    OracleImage result;
    result.width = static_cast<std::uint32_t>(targetWidth);
    result.height = static_cast<std::uint32_t>(targetHeight);
    result.pixels.resize(static_cast<std::size_t>(targetWidth) *
                         static_cast<std::size_t>(targetHeight));
    for (int y = 0; y < targetHeight; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
            const QColor c = target.pixelColor(x, y);
            result.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(targetWidth) +
                          static_cast<std::size_t>(x)] =
                Rgba8{static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
                      static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())};
        }
    }
    return result;
}

struct ImageDelta final {
    std::size_t mismatches = 0;
    int maxChannelDelta = 0;
    bool ok = true;
};

[[nodiscard]] inline ImageDelta compareImages(const std::vector<Rgba8>& gpu,
                                              const std::vector<Rgba8>& oracle,
                                              const int tolerance) {
    ImageDelta delta;
    if (gpu.size() != oracle.size()) {
        delta.ok = false;
        delta.mismatches = std::max(gpu.size(), oracle.size());
        return delta;
    }
    for (std::size_t index = 0; index < gpu.size(); ++index) {
        const std::uint8_t a[4] = {gpu[index].red, gpu[index].green, gpu[index].blue,
                                   gpu[index].alpha};
        const std::uint8_t b[4] = {oracle[index].red, oracle[index].green, oracle[index].blue,
                                   oracle[index].alpha};
        int worst = 0;
        for (int c = 0; c < 4; ++c) {
            worst = std::max(worst, std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])));
        }
        delta.maxChannelDelta = std::max(delta.maxChannelDelta, worst);
        if (worst > tolerance) {
            ++delta.mismatches;
            delta.ok = false;
        }
    }
    return delta;
}

} // namespace bloom::render::present_oracle
