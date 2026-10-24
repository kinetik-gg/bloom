#include <bloom/render/channel_set_image.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace bloom::render {
namespace {

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

[[nodiscard]] bool layerIs(const std::string& layer,
                           std::initializer_list<std::string_view> names) {
    return std::ranges::any_of(names, [&](const std::string_view name) { return layer == name; });
}

} // namespace

std::string_view channelLayerName(const std::string_view fullName) noexcept {
    const auto dot = fullName.rfind('.');
    return dot == std::string_view::npos ? std::string_view{} : fullName.substr(0, dot);
}

bool isColourRole(const ChannelRole role) noexcept {
    return role == ChannelRole::ColorR || role == ChannelRole::ColorG ||
           role == ChannelRole::ColorB || role == ChannelRole::Luminance;
}

ChannelDescriptor describeChannel(const std::string_view fullName,
                                  const ChannelSampleKind sampleKind) {
    ChannelDescriptor descriptor;
    descriptor.name = std::string(fullName);
    descriptor.layer = std::string(channelLayerName(fullName));
    descriptor.sampleKind = sampleKind;
    const auto suffix = fullName.substr(descriptor.layer.empty() ? 0 : descriptor.layer.size() + 1);
    const auto layer = lower(descriptor.layer);
    const auto leaf = lower(layer.substr(layer.rfind('.') == std::string::npos
                                             ? 0
                                             : layer.rfind('.') + 1));
    const auto axis = lower(suffix);
    const auto pick = [&](const ChannelRole x, const ChannelRole y, const ChannelRole z) {
        if (axis == "x" || axis == "r" || axis == "u")
            return x;
        if (axis == "y" || axis == "g" || axis == "v")
            return y;
        if (axis == "z" || axis == "b" || axis == "w")
            return z;
        return ChannelRole::Data;
    };
    if (leaf.starts_with("crypto")) {
        // Cryptomatte rank pairs: (r, g) and (b, a) are (ID hash, coverage).
        descriptor.role = (axis == "r" || axis == "b") ? ChannelRole::Id : ChannelRole::Coverage;
    } else if (layerIs(leaf, {"depth", "z", "zdepth"}) || (layer.empty() && suffix == "Z")) {
        descriptor.role = ChannelRole::Depth;
    } else if (layerIs(leaf, {"n", "normal", "normals"})) {
        descriptor.role = pick(ChannelRole::NormalX, ChannelRole::NormalY, ChannelRole::NormalZ);
    } else if (layerIs(leaf, {"p", "position", "pworld", "pref"})) {
        descriptor.role =
            pick(ChannelRole::PositionX, ChannelRole::PositionY, ChannelRole::PositionZ);
    } else if (layerIs(leaf, {"motion", "forward", "backward", "mv", "vector", "velocity"})) {
        descriptor.role = (axis == "u" || axis == "x" || axis == "r") ? ChannelRole::MotionU
                          : (axis == "v" || axis == "y" || axis == "g") ? ChannelRole::MotionV
                                                                         : ChannelRole::Data;
    } else if (layerIs(leaf, {"uv", "st"})) {
        descriptor.role = (axis == "u" || axis == "s" || axis == "x" || axis == "r")
                              ? ChannelRole::UvU
                          : (axis == "v" || axis == "t" || axis == "y" || axis == "g")
                              ? ChannelRole::UvV
                              : ChannelRole::Data;
    } else if (suffix == "R") {
        descriptor.role = ChannelRole::ColorR;
    } else if (suffix == "G") {
        descriptor.role = ChannelRole::ColorG;
    } else if (suffix == "B") {
        descriptor.role = ChannelRole::ColorB;
    } else if (suffix == "A") {
        descriptor.role = ChannelRole::Alpha;
    } else if (suffix == "Y") {
        descriptor.role = ChannelRole::Luminance;
    }
    // Integer samples are never colour or alpha.
    if (sampleKind == ChannelSampleKind::Uint &&
        (isColourRole(descriptor.role) || descriptor.role == ChannelRole::Alpha))
        descriptor.role = ChannelRole::Data;
    descriptor.encoding = isColourRole(descriptor.role) ? ChannelValueEncoding::Colour
                                                         : ChannelValueEncoding::Data;
    return descriptor;
}

ImageResult<ChannelSetImage>
ChannelSetImage::create(const ImageWindow dataWindow, const ImageWindow displayWindow,
                        const core::PixelAspectRatio pixelAspect,
                        std::vector<ChannelDescriptor> channels,
                        std::vector<std::vector<float>> planes, ChannelMetadata metadata,
                        const std::size_t byteBudget, std::string colourSpace) {
    if (channels.empty() || channels.size() != planes.size())
        return ImageResult<ChannelSetImage>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidParameter));
    const auto samples = static_cast<std::uint64_t>(dataWindow.extent().width()) *
                         static_cast<std::uint64_t>(dataWindow.extent().height());
    std::uint64_t bytes = 0;
    for (std::size_t index = 0; index < planes.size(); ++index) {
        if (planes[index].size() != samples)
            return ImageResult<ChannelSetImage>::failure(
                ImageError::codeOnly(ImageErrorCode::InvalidStorageSize));
        for (std::size_t other = 0; other < index; ++other)
            if (channels[other].name == channels[index].name)
                return ImageResult<ChannelSetImage>::failure(
                    ImageError::codeOnly(ImageErrorCode::InvalidParameter));
        if (samples > (std::numeric_limits<std::uint64_t>::max() - bytes) / sizeof(float))
            return ImageResult<ChannelSetImage>::failure(
                ImageError::codeOnly(ImageErrorCode::ArithmeticOverflow));
        bytes += samples * sizeof(float);
    }
    if (bytes > byteBudget)
        return ImageResult<ChannelSetImage>::failure(
            ImageError::pixelStorageBudgetExceeded(bytes, byteBudget));
    return ImageResult<ChannelSetImage>::success(
        ChannelSetImage(dataWindow, displayWindow, pixelAspect, std::move(channels),
                        std::move(planes), std::move(metadata), std::move(colourSpace)));
}

ChannelSetImage::ChannelSetImage(const ImageWindow dataWindow, const ImageWindow displayWindow,
                                 const core::PixelAspectRatio pixelAspect,
                                 std::vector<ChannelDescriptor> channels,
                                 std::vector<std::vector<float>> planes,
                                 ChannelMetadata metadata, std::string colourSpace) noexcept
    : dataWindow_(dataWindow), displayWindow_(displayWindow), pixelAspect_(pixelAspect),
      channels_(std::move(channels)), planes_(std::move(planes)), metadata_(std::move(metadata)),
      colourSpace_(std::move(colourSpace)) {}

std::span<const float> ChannelSetImage::plane(const std::size_t index) const noexcept {
    return index < planes_.size() ? std::span<const float>(planes_[index])
                                  : std::span<const float>{};
}

std::optional<std::size_t> ChannelSetImage::find(const std::string_view name) const noexcept {
    for (std::size_t index = 0; index < channels_.size(); ++index)
        if (channels_[index].name == name)
            return index;
    return std::nullopt;
}

std::size_t ChannelSetImage::byteCost() const noexcept {
    std::size_t total = sizeof(ChannelSetImage);
    for (const auto& plane : planes_)
        total += plane.capacity() * sizeof(float);
    for (const auto& channel : channels_)
        total += sizeof(ChannelDescriptor) + channel.name.capacity() + channel.layer.capacity();
    for (const auto& [key, value] : metadata_)
        total += key.capacity() + value.capacity();
    return total + colourSpace_.capacity();
}

} // namespace bloom::render
