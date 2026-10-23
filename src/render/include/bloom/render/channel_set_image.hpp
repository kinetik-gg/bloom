#pragma once

// ADR 0026: the multi-channel transport. A ChannelSetImage holds N named Float32 planes that share
// one data window, display window and pixel aspect, plus the source part's string metadata. Values
// are never clamped, canonicalized or premultiplied: data channels (depth, normals, motion, IDs)
// and Cryptomatte hash bit patterns survive bit for bit. It is a separate type from the canonical
// premultiplied Rgba32fImage; nothing converts between the two implicitly.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::render {

enum class ChannelRole : std::uint8_t {
    ColorR,
    ColorG,
    ColorB,
    Alpha,
    Luminance,
    Depth,
    NormalX,
    NormalY,
    NormalZ,
    MotionU,
    MotionV,
    PositionX,
    PositionY,
    PositionZ,
    UvU,
    UvV,
    Id,
    Coverage,
    Data,
};
enum class ChannelSampleKind : std::uint8_t { Half, Float, Uint };
enum class ChannelValueEncoding : std::uint8_t { Colour, Data };

struct ChannelDescriptor final {
    // The full source channel name, e.g. "diffuse.R", "depth.Z", "CryptoObject00.r".
    std::string name;
    // Everything before the last '.', empty for root channels.
    std::string layer;
    ChannelRole role = ChannelRole::Data;
    ChannelSampleKind sampleKind = ChannelSampleKind::Float;
    ChannelValueEncoding encoding = ChannelValueEncoding::Data;
    friend bool operator==(const ChannelDescriptor&, const ChannelDescriptor&) = default;
};

// Role inference from a source channel name (media-io.md "Multi-channel images"). Colour roles are
// only the upper-case R/G/B/A/Y suffixes of a layer that is not a known data pass; every other
// channel is Data-encoded.
[[nodiscard]] ChannelDescriptor describeChannel(std::string_view fullName,
                                                ChannelSampleKind sampleKind);
[[nodiscard]] std::string_view channelLayerName(std::string_view fullName) noexcept;
[[nodiscard]] bool isColourRole(ChannelRole role) noexcept;

using ChannelMetadata = std::vector<std::pair<std::string, std::string>>;

class ChannelSetImage final {
  public:
    // Each plane holds dataWindow.width * dataWindow.height samples (Uint planes keep the 32-bit
    // pattern in the Float32 lane). The total plane bytes must fit `byteBudget`.
    [[nodiscard]] static ImageResult<ChannelSetImage>
    create(ImageWindow dataWindow, ImageWindow displayWindow, core::PixelAspectRatio pixelAspect,
           std::vector<ChannelDescriptor> channels, std::vector<std::vector<float>> planes,
           ChannelMetadata metadata, std::size_t byteBudget);

    [[nodiscard]] ImageWindow dataWindow() const noexcept { return dataWindow_; }
    [[nodiscard]] ImageWindow displayWindow() const noexcept { return displayWindow_; }
    [[nodiscard]] core::PixelAspectRatio pixelAspect() const noexcept { return pixelAspect_; }
    [[nodiscard]] std::span<const ChannelDescriptor> channels() const noexcept {
        return channels_;
    }
    [[nodiscard]] std::span<const float> plane(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const noexcept;
    [[nodiscard]] const ChannelMetadata& metadata() const noexcept { return metadata_; }
    // Plane bytes plus descriptor and metadata strings.
    [[nodiscard]] std::size_t byteCost() const noexcept;

  private:
    ChannelSetImage(ImageWindow dataWindow, ImageWindow displayWindow,
                    core::PixelAspectRatio pixelAspect, std::vector<ChannelDescriptor> channels,
                    std::vector<std::vector<float>> planes, ChannelMetadata metadata) noexcept;
    ImageWindow dataWindow_;
    ImageWindow displayWindow_;
    core::PixelAspectRatio pixelAspect_;
    std::vector<ChannelDescriptor> channels_;
    std::vector<std::vector<float>> planes_;
    ChannelMetadata metadata_;
};

} // namespace bloom::render
