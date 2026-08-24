#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace bloom::render {

enum class ImageErrorCode : std::uint8_t {
    InvalidExtent,
    InvalidWindow,
    ArithmeticOverflow,
    PixelStorageBudgetExceeded,
    AllocationFailure,
    InvalidPixel,
    InvalidStorageSize,
    CoordinateOutOfBounds,
    InvalidState,
    InvalidParameter,
    UnsupportedFloatingPointEnvironment,
    IncompatibleImageDescriptor,
    NonFiniteResult,
};

struct ImageError final {
    ImageErrorCode code;
    std::optional<std::size_t> requestedPixelStorageBytes;
    std::optional<std::size_t> pixelStorageByteLimit;
    std::optional<std::size_t> actualPixelStorageBytes;
    std::optional<std::size_t> expectedPixelStorageBytes;

    [[nodiscard]] static constexpr ImageError codeOnly(const ImageErrorCode code) noexcept {
        return ImageError{code, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    }
    [[nodiscard]] static constexpr ImageError
    pixelStorageBudgetExceeded(const std::size_t requestedBytes,
                               const std::size_t byteLimit) noexcept {
        return ImageError{ImageErrorCode::PixelStorageBudgetExceeded, requestedBytes, byteLimit,
                          std::nullopt, std::nullopt};
    }
    [[nodiscard]] static constexpr ImageError
    allocationFailure(const std::size_t requestedBytes) noexcept {
        return ImageError{ImageErrorCode::AllocationFailure, requestedBytes, std::nullopt,
                          std::nullopt, std::nullopt};
    }
    [[nodiscard]] static constexpr ImageError
    storageSizeMismatch(const std::size_t actualBytes, const std::size_t expectedBytes) noexcept {
        return ImageError{ImageErrorCode::InvalidStorageSize, std::nullopt, std::nullopt,
                          actualBytes, expectedBytes};
    }

    friend constexpr bool operator==(const ImageError&, const ImageError&) noexcept = default;
};

template <typename T> class [[nodiscard]] ImageResult final {
  public:
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "ImageResult values must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "ImageResult values must be nothrow destructible");

    [[nodiscard]] static ImageResult success(T value) noexcept {
        return ImageResult(std::move(value));
    }

    [[nodiscard]] static ImageResult failure(const ImageError error) noexcept {
        return ImageResult(error);
    }

    [[nodiscard]] bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] T* value() noexcept { return std::get_if<T>(&storage_); }
    [[nodiscard]] const T* value() const noexcept { return std::get_if<T>(&storage_); }
    [[nodiscard]] ImageError* error() noexcept { return std::get_if<ImageError>(&storage_); }
    [[nodiscard]] const ImageError* error() const noexcept {
        return std::get_if<ImageError>(&storage_);
    }

  private:
    explicit ImageResult(T&& value) noexcept : storage_(std::in_place_index<0>, std::move(value)) {}
    explicit ImageResult(const ImageError error) noexcept
        : storage_(std::in_place_index<1>, error) {}

    std::variant<T, ImageError> storage_;
};

// No value means success. A value carries the reason an in-place operation was rejected.
using ImageStatus = std::optional<ImageError>;

class ImageExtent final {
  public:
    [[nodiscard]] static ImageResult<ImageExtent> create(std::uint64_t width,
                                                         std::uint64_t height) noexcept;

    [[nodiscard]] constexpr std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] constexpr std::uint32_t height() const noexcept { return height_; }

    friend constexpr auto operator<=>(const ImageExtent&, const ImageExtent&) noexcept = default;

  private:
    constexpr ImageExtent(const std::uint32_t width, const std::uint32_t height) noexcept
        : width_(width), height_(height) {}

    std::uint32_t width_;
    std::uint32_t height_;
};

class ImageWindow final {
  public:
    [[nodiscard]] static ImageResult<ImageWindow> create(std::int64_t originX, std::int64_t originY,
                                                         std::uint64_t width,
                                                         std::uint64_t height) noexcept;

    [[nodiscard]] constexpr std::int64_t originX() const noexcept { return originX_; }
    [[nodiscard]] constexpr std::int64_t originY() const noexcept { return originY_; }
    [[nodiscard]] constexpr ImageExtent extent() const noexcept { return extent_; }
    [[nodiscard]] constexpr std::int64_t maxXExclusive() const noexcept {
        return originX_ + static_cast<std::int64_t>(extent_.width());
    }
    [[nodiscard]] constexpr std::int64_t maxYExclusive() const noexcept {
        return originY_ + static_cast<std::int64_t>(extent_.height());
    }
    [[nodiscard]] constexpr bool contains(const std::int64_t x,
                                          const std::int64_t y) const noexcept {
        return x >= originX_ && x < maxXExclusive() && y >= originY_ && y < maxYExclusive();
    }

    friend constexpr auto operator<=>(const ImageWindow&, const ImageWindow&) noexcept = default;

  private:
    constexpr ImageWindow(const std::int64_t originX, const std::int64_t originY,
                          const ImageExtent extent) noexcept
        : originX_(originX), originY_(originY), extent_(extent) {}

    std::int64_t originX_;
    std::int64_t originY_;
    ImageExtent extent_;
};

struct PackedImageLayout final {
    std::size_t pixelCount;
    std::size_t rowStrideBytes;
    std::size_t pixelStorageBytes;

    friend constexpr auto operator<=>(const PackedImageLayout&,
                                      const PackedImageLayout&) noexcept = default;
};

enum class AlphaAssociation : std::uint8_t {
    Straight,
    Premultiplied,
};

enum class PixelPacking : std::uint8_t {
    PackedRgba32f,
    PackedRgba8,
};

enum class ColorEncoding : std::uint8_t {
    LinearRec709Scene,
};

inline constexpr std::string_view kLinearRec709SceneEncodingId = "lin_rec709_scene";

[[nodiscard]] constexpr std::string_view colorEncodingId(const ColorEncoding encoding) noexcept {
    switch (encoding) {
    case ColorEncoding::LinearRec709Scene:
        return kLinearRec709SceneEncodingId;
    }
    return {};
}

// A validated RGBA value. RGB is finite and unbounded, alpha is finite and confined to [0, 1],
// and the components are premultiplied. Color encoding belongs to the containing image descriptor.
class Rgba32f final {
  public:
    [[nodiscard]] static ImageResult<Rgba32f> fromPremultiplied(float red, float green, float blue,
                                                                float alpha) noexcept;
    [[nodiscard]] static constexpr Rgba32f transparent() noexcept {
        return Rgba32f(0.0F, 0.0F, 0.0F, 0.0F);
    }

    [[nodiscard]] constexpr float red() const noexcept { return components_[0]; }
    [[nodiscard]] constexpr float green() const noexcept { return components_[1]; }
    [[nodiscard]] constexpr float blue() const noexcept { return components_[2]; }
    [[nodiscard]] constexpr float alpha() const noexcept { return components_[3]; }
    [[nodiscard]] constexpr std::span<const float, 4> components() const noexcept {
        return std::span<const float, 4>(components_);
    }

    friend constexpr bool operator==(const Rgba32f&, const Rgba32f&) noexcept = default;

  private:
    constexpr Rgba32f(const float red, const float green, const float blue,
                      const float alpha) noexcept
        : components_{red, green, blue, alpha} {}

    std::array<float, 4> components_;
};

struct Rgba8 final {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha;

    friend constexpr bool operator==(const Rgba8&, const Rgba8&) noexcept = default;
};

static_assert(sizeof(float) == 4, "RGBA32F requires 32-bit float components");
static_assert(std::numeric_limits<float>::is_iec559, "RGBA32F requires IEEE 754 float components");
static_assert(sizeof(Rgba32f) == 4 * sizeof(float), "RGBA32F pixels must be tightly packed");
static_assert(std::is_standard_layout_v<Rgba32f> && std::is_trivially_copyable_v<Rgba32f>,
              "RGBA32F pixels must have a stable value layout");
static_assert(sizeof(Rgba8) == 4, "RGBA8 pixels must be tightly packed");
static_assert(std::is_standard_layout_v<Rgba8> && std::is_trivially_copyable_v<Rgba8>,
              "RGBA8 pixels must have a stable value layout");

[[nodiscard]] ImageResult<PackedImageLayout> checkedRgba32fLayout(ImageExtent extent) noexcept;
[[nodiscard]] ImageResult<PackedImageLayout> checkedRgba8Layout(ImageExtent extent) noexcept;

} // namespace bloom::render
