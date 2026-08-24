#pragma once

#include <bloom/core/pixel_aspect_ratio.hpp>

#include <cstdint>
#include <numeric>
#include <optional>

namespace bloom::document {

class FrameRate final {
  public:
    [[nodiscard]] static constexpr FrameRate framesPerSecond24() noexcept {
        return FrameRate(24, 1);
    }

    [[nodiscard]] static constexpr std::optional<FrameRate>
    create(const std::uint32_t numerator, const std::uint32_t denominator) noexcept {
        if (numerator == 0 || denominator == 0) {
            return std::nullopt;
        }
        const auto divisor = std::gcd(numerator, denominator);
        return FrameRate(numerator / divisor, denominator / divisor);
    }

    [[nodiscard]] constexpr std::uint32_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] constexpr std::uint32_t denominator() const noexcept { return denominator_; }

    friend constexpr bool operator==(const FrameRate&, const FrameRate&) noexcept = default;

  private:
    constexpr FrameRate(const std::uint32_t numerator, const std::uint32_t denominator) noexcept
        : numerator_(numerator), denominator_(denominator) {}

    std::uint32_t numerator_ = 24;
    std::uint32_t denominator_ = 1;
};

class CompositionFormat final {
  public:
    static constexpr std::uint32_t kDefaultWidth = 1920;
    static constexpr std::uint32_t kDefaultHeight = 1080;
    static constexpr std::uint32_t kMaximumDimension = 1U << 20U;
    static constexpr std::uint64_t kMaximumPixelCount = std::uint64_t{1} << 32U;

    constexpr CompositionFormat() noexcept = default;

    [[nodiscard]] static constexpr std::optional<CompositionFormat>
    create(const std::uint32_t width, const std::uint32_t height,
           const core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square(),
           const FrameRate frameRate = FrameRate::framesPerSecond24()) noexcept {
        if (!isValidExtent(width, height)) {
            return std::nullopt;
        }
        return CompositionFormat(width, height, pixelAspect, frameRate);
    }

    [[nodiscard]] constexpr std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] constexpr std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] constexpr core::PixelAspectRatio pixelAspect() const noexcept {
        return pixelAspect_;
    }
    [[nodiscard]] constexpr FrameRate frameRate() const noexcept { return frameRate_; }

    friend constexpr bool operator==(const CompositionFormat&,
                                     const CompositionFormat&) noexcept = default;

  private:
    [[nodiscard]] static constexpr bool isValidExtent(const std::uint32_t width,
                                                      const std::uint32_t height) noexcept {
        return width > 0 && height > 0 && width <= kMaximumDimension &&
               height <= kMaximumDimension &&
               static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <=
                   kMaximumPixelCount;
    }

    constexpr CompositionFormat(const std::uint32_t width, const std::uint32_t height,
                                const core::PixelAspectRatio pixelAspect,
                                const FrameRate frameRate) noexcept
        : width_(width), height_(height), pixelAspect_(pixelAspect), frameRate_(frameRate) {}

    std::uint32_t width_ = kDefaultWidth;
    std::uint32_t height_ = kDefaultHeight;
    core::PixelAspectRatio pixelAspect_ = core::PixelAspectRatio::square();
    FrameRate frameRate_ = FrameRate::framesPerSecond24();
};

} // namespace bloom::document
