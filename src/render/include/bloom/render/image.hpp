#pragma once

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bloom::render {

class Rgba32fImageDescriptor final {
  public:
    [[nodiscard]] static ImageResult<Rgba32fImageDescriptor>
    create(ImageWindow dataWindow, ImageWindow displayWindow,
           core::PixelAspectRatio pixelAspect) noexcept;

    [[nodiscard]] constexpr ImageWindow dataWindow() const noexcept { return dataWindow_; }
    [[nodiscard]] constexpr ImageWindow displayWindow() const noexcept { return displayWindow_; }
    [[nodiscard]] constexpr core::PixelAspectRatio pixelAspect() const noexcept {
        return pixelAspect_;
    }
    [[nodiscard]] constexpr const PackedImageLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] static constexpr AlphaAssociation alphaAssociation() noexcept {
        return AlphaAssociation::Premultiplied;
    }
    [[nodiscard]] static constexpr PixelPacking pixelPacking() noexcept {
        return PixelPacking::PackedRgba32f;
    }
    [[nodiscard]] static constexpr ColorEncoding colorEncoding() noexcept {
        return ColorEncoding::ReferenceLinearSrgb;
    }

    friend constexpr bool operator==(const Rgba32fImageDescriptor&,
                                     const Rgba32fImageDescriptor&) noexcept = default;

  private:
    constexpr Rgba32fImageDescriptor(const ImageWindow dataWindow, const ImageWindow displayWindow,
                                     const core::PixelAspectRatio pixelAspect,
                                     const PackedImageLayout layout) noexcept
        : dataWindow_(dataWindow), displayWindow_(displayWindow), pixelAspect_(pixelAspect),
          layout_(layout) {}

    ImageWindow dataWindow_;
    ImageWindow displayWindow_;
    core::PixelAspectRatio pixelAspect_;
    PackedImageLayout layout_;
};

// Views borrow pixel storage. Destroying, moving, assigning, or freezing the owner invalidates
// them.
class Rgba32fImageView final {
  public:
    constexpr Rgba32fImageView() noexcept = default;

    [[nodiscard]] constexpr bool isValid() const noexcept { return descriptor_.has_value(); }
    [[nodiscard]] constexpr std::optional<Rgba32fImageDescriptor> descriptor() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] constexpr std::span<const Rgba32f> pixels() const noexcept { return pixels_; }
    [[nodiscard]] ImageResult<std::span<const Rgba32f>> row(std::int64_t y) const noexcept;
    [[nodiscard]] ImageResult<Rgba32f> read(std::int64_t x, std::int64_t y) const noexcept;

  private:
    friend class Rgba32fImage;
    friend class Rgba32fImageBuilder;

    constexpr Rgba32fImageView(const Rgba32fImageDescriptor descriptor,
                               const std::span<const Rgba32f> pixels) noexcept
        : descriptor_(descriptor), pixels_(pixels) {}

    std::optional<Rgba32fImageDescriptor> descriptor_;
    std::span<const Rgba32f> pixels_;
};

// Immutable owning CPU image suitable for publication across task and render boundaries.
class Rgba32fImage final {
  public:
    Rgba32fImage(const Rgba32fImage&) = delete;
    Rgba32fImage& operator=(const Rgba32fImage&) = delete;
    Rgba32fImage(Rgba32fImage&& other) noexcept;
    Rgba32fImage& operator=(Rgba32fImage&& other) noexcept;
    ~Rgba32fImage() = default;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const Rgba32fImageDescriptor* descriptor() const& noexcept;
    [[nodiscard]] const Rgba32fImageDescriptor* descriptor() const&& = delete;
    [[nodiscard]] std::span<const Rgba32f> pixels() const& noexcept { return pixels_; }
    [[nodiscard]] std::span<const Rgba32f> pixels() const&& = delete;
    [[nodiscard]] ImageResult<Rgba32fImageView> view() const& noexcept;
    [[nodiscard]] ImageResult<Rgba32fImageView> view() const&& = delete;
    [[nodiscard]] ImageResult<Rgba32f> read(std::int64_t x, std::int64_t y) const noexcept;

  private:
    friend class Rgba32fImageBuilder;

    Rgba32fImage(Rgba32fImageDescriptor descriptor, std::vector<Rgba32f> pixels) noexcept;
    void reset() noexcept;

    std::optional<Rgba32fImageDescriptor> descriptor_;
    std::vector<Rgba32f> pixels_;
};

// Mutation is deliberately confined to construction. Freeze consumes the builder and publishes an
// immutable owner, leaving the builder in a coherent invalid state.
class Rgba32fImageBuilder final {
  public:
    Rgba32fImageBuilder(const Rgba32fImageBuilder&) = delete;
    Rgba32fImageBuilder& operator=(const Rgba32fImageBuilder&) = delete;
    Rgba32fImageBuilder(Rgba32fImageBuilder&& other) noexcept;
    Rgba32fImageBuilder& operator=(Rgba32fImageBuilder&& other) noexcept;
    ~Rgba32fImageBuilder() = default;

    [[nodiscard]] static ImageResult<Rgba32fImageBuilder>
    create(Rgba32fImageDescriptor descriptor, std::size_t pixelStorageByteLimit,
           Rgba32f clearValue = Rgba32f::transparent()) noexcept;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const Rgba32fImageDescriptor* descriptor() const& noexcept;
    [[nodiscard]] const Rgba32fImageDescriptor* descriptor() const&& = delete;
    [[nodiscard]] std::span<const Rgba32f> pixels() const& noexcept { return pixels_; }
    [[nodiscard]] std::span<const Rgba32f> pixels() const&& = delete;
    [[nodiscard]] ImageResult<Rgba32fImageView> view() const& noexcept;
    [[nodiscard]] ImageResult<Rgba32fImageView> view() const&& = delete;
    [[nodiscard]] ImageResult<std::span<Rgba32f>> row(std::int64_t y) & noexcept;
    [[nodiscard]] ImageResult<std::span<Rgba32f>> row(std::int64_t y) && = delete;
    [[nodiscard]] ImageResult<Rgba32f> read(std::int64_t x, std::int64_t y) const noexcept;
    [[nodiscard]] ImageStatus write(std::int64_t x, std::int64_t y, Rgba32f value) noexcept;
    [[nodiscard]] ImageStatus clear(Rgba32f value) noexcept;
    [[nodiscard]] ImageResult<Rgba32fImage> freeze() && noexcept;

  private:
    Rgba32fImageBuilder(Rgba32fImageDescriptor descriptor, std::vector<Rgba32f> pixels) noexcept;
    void reset() noexcept;

    std::optional<Rgba32fImageDescriptor> descriptor_;
    std::vector<Rgba32f> pixels_;
};

} // namespace bloom::render
