#pragma once

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::render {

// This handoff identifies the temporary built-in reference display transform. It is intentionally
// not an OCIO-qualified production display result.
class ReferenceDisplayBufferDescriptor final {
  public:
    [[nodiscard]] static ImageResult<ReferenceDisplayBufferDescriptor>
    create(ImageWindow displayWindow, core::PixelAspectRatio pixelAspect) noexcept;

    [[nodiscard]] constexpr ImageWindow displayWindow() const noexcept { return displayWindow_; }
    [[nodiscard]] constexpr core::PixelAspectRatio pixelAspect() const noexcept {
        return pixelAspect_;
    }
    [[nodiscard]] constexpr const PackedImageLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] static constexpr PixelPacking pixelPacking() noexcept {
        return PixelPacking::PackedRgba8;
    }
    [[nodiscard]] static constexpr AlphaAssociation alphaAssociation() noexcept {
        return AlphaAssociation::Straight;
    }
    [[nodiscard]] static constexpr std::string_view referenceDisplayPipelineId() noexcept {
        return "bloom.reference.lin_rec709_scene-to-srgb.v1";
    }
    [[nodiscard]] static constexpr bool isOcioQualified() noexcept { return false; }

    friend constexpr bool operator==(const ReferenceDisplayBufferDescriptor&,
                                     const ReferenceDisplayBufferDescriptor&) noexcept = default;

  private:
    constexpr ReferenceDisplayBufferDescriptor(const ImageWindow displayWindow,
                                               const core::PixelAspectRatio pixelAspect,
                                               const PackedImageLayout layout) noexcept
        : displayWindow_(displayWindow), pixelAspect_(pixelAspect), layout_(layout) {}

    ImageWindow displayWindow_;
    core::PixelAspectRatio pixelAspect_;
    PackedImageLayout layout_;
};

// Views borrow pixel storage. Destroying, moving, or assigning the owner invalidates them.
class ReferenceDisplayBufferView final {
  public:
    constexpr ReferenceDisplayBufferView() noexcept = default;

    [[nodiscard]] constexpr bool isValid() const noexcept { return descriptor_.has_value(); }
    [[nodiscard]] constexpr std::optional<ReferenceDisplayBufferDescriptor>
    descriptor() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] constexpr std::span<const Rgba8> pixels() const noexcept { return pixels_; }

  private:
    friend class PreparedReferenceDisplayBuffer;
    friend class ReferenceDisplayBufferBuilder;

    constexpr ReferenceDisplayBufferView(const ReferenceDisplayBufferDescriptor descriptor,
                                         const std::span<const Rgba8> pixels) noexcept
        : descriptor_(descriptor), pixels_(pixels) {}

    std::optional<ReferenceDisplayBufferDescriptor> descriptor_;
    std::span<const Rgba8> pixels_;
};

class PreparedReferenceDisplayBuffer final {
  public:
    PreparedReferenceDisplayBuffer(const PreparedReferenceDisplayBuffer&) = delete;
    PreparedReferenceDisplayBuffer& operator=(const PreparedReferenceDisplayBuffer&) = delete;
    PreparedReferenceDisplayBuffer(PreparedReferenceDisplayBuffer&& other) noexcept;
    PreparedReferenceDisplayBuffer& operator=(PreparedReferenceDisplayBuffer&& other) noexcept;
    ~PreparedReferenceDisplayBuffer() = default;

    [[nodiscard]] static ImageResult<PreparedReferenceDisplayBuffer>
    create(ReferenceDisplayBufferDescriptor descriptor, std::span<const Rgba8> pixels,
           std::size_t pixelStorageByteLimit) noexcept;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const ReferenceDisplayBufferDescriptor* descriptor() const& noexcept;
    [[nodiscard]] const ReferenceDisplayBufferDescriptor* descriptor() const&& = delete;
    [[nodiscard]] std::span<const Rgba8> pixels() const& noexcept { return pixels_; }
    [[nodiscard]] std::span<const Rgba8> pixels() const&& = delete;
    [[nodiscard]] ImageResult<ReferenceDisplayBufferView> view() const& noexcept;
    [[nodiscard]] ImageResult<ReferenceDisplayBufferView> view() const&& = delete;

  private:
    friend class ReferenceDisplayBufferBuilder;

    PreparedReferenceDisplayBuffer(ReferenceDisplayBufferDescriptor descriptor,
                                   std::vector<Rgba8> pixels) noexcept;
    void reset() noexcept;

    std::optional<ReferenceDisplayBufferDescriptor> descriptor_;
    std::vector<Rgba8> pixels_;
};

// Mutation is confined to worker-side construction. Freeze consumes the builder and publishes the
// immutable packed display handoff without copying its pixel payload.
class ReferenceDisplayBufferBuilder final {
  public:
    ReferenceDisplayBufferBuilder(const ReferenceDisplayBufferBuilder&) = delete;
    ReferenceDisplayBufferBuilder& operator=(const ReferenceDisplayBufferBuilder&) = delete;
    ReferenceDisplayBufferBuilder(ReferenceDisplayBufferBuilder&& other) noexcept;
    ReferenceDisplayBufferBuilder& operator=(ReferenceDisplayBufferBuilder&& other) noexcept;
    ~ReferenceDisplayBufferBuilder() = default;

    [[nodiscard]] static ImageResult<ReferenceDisplayBufferBuilder>
    create(ReferenceDisplayBufferDescriptor descriptor, std::size_t pixelStorageByteLimit) noexcept;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] const ReferenceDisplayBufferDescriptor* descriptor() const& noexcept;
    [[nodiscard]] const ReferenceDisplayBufferDescriptor* descriptor() const&& = delete;
    [[nodiscard]] std::span<const Rgba8> pixels() const& noexcept { return pixels_; }
    [[nodiscard]] std::span<const Rgba8> pixels() const&& = delete;
    [[nodiscard]] ImageResult<ReferenceDisplayBufferView> view() const& noexcept;
    [[nodiscard]] ImageResult<ReferenceDisplayBufferView> view() const&& = delete;
    [[nodiscard]] ImageResult<std::span<Rgba8>> row(std::int64_t y) & noexcept;
    [[nodiscard]] ImageResult<std::span<Rgba8>> row(std::int64_t y) && = delete;
    [[nodiscard]] ImageResult<PreparedReferenceDisplayBuffer> freeze() && noexcept;

  private:
    ReferenceDisplayBufferBuilder(ReferenceDisplayBufferDescriptor descriptor,
                                  std::vector<Rgba8> pixels) noexcept;
    void reset() noexcept;

    std::optional<ReferenceDisplayBufferDescriptor> descriptor_;
    std::vector<Rgba8> pixels_;
};

} // namespace bloom::render
