#include "asset_data_thumbnail.hpp"

#include <bloom/media/image.hpp>
#include <bloom/render/channel_set_image.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bloom::ui::detail {

QImage dataChannelThumbnail(const std::filesystem::path& path, const core::Sha256Digest& digest,
                            const std::string& part, const std::function<bool()>& cancel) {
    const auto revision =
        media::captureImageSourceRevision(path, media::kMaxImageStorageBytes, cancel, digest);
    if (!revision.value || !*revision.value)
        return {};
    const auto* info = (*revision.value)->probe().findPart(part);
    if (info == nullptr || info->channels.empty())
        return {};
    std::vector<std::string> names;
    for (const auto& channel : info->channels)
        names.push_back(channel.name);
    const auto types = runtime::cryptomatteTypes(names);
    media::ImageChannelSelection request;
    request.part = part;
    request.channels = types.empty() ? std::vector<std::string>{names.front()}
                                     : std::vector<std::string>{types.front().id,
                                                                types.front().coverage};
    const auto decoded = media::decodeImageChannels(**revision.value, request, cancel, {},
                                                    media::kMaxImageStorageBytes);
    if (!decoded.value || !*decoded.value)
        return {};
    const auto& set = **decoded.value;
    const auto window = set.dataWindow();
    const auto width = window.extent().width();
    const auto height = window.extent().height();
    if (width == 0 || height == 0)
        return {};
    const auto scale = std::min(1.0, 64.0 / static_cast<double>(std::max(width, height)));
    const auto thumbWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
    const auto thumbHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
    QImage image(thumbWidth, thumbHeight, QImage::Format_RGBA8888);
    const auto first = set.plane(0);
    const auto second = types.empty() ? std::span<const float>{} : set.plane(1);
    const auto byte = [](const float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    for (int y = 0; y < thumbHeight; ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < thumbWidth; ++x) {
            const auto sx = static_cast<std::size_t>(x) * width / static_cast<std::size_t>(thumbWidth);
            const auto sy =
                static_cast<std::size_t>(y) * height / static_cast<std::size_t>(thumbHeight);
            const auto index = sy * width + sx;
            std::array<float, 3> rgb{};
            if (!types.empty()) {
                if (index < second.size() && second[index] > 0.0F)
                    rgb = runtime::cryptomatteIdColour(std::bit_cast<std::uint32_t>(first[index]));
            } else if (index < first.size() && std::isfinite(first[index])) {
                rgb.fill(first[index]);
            }
            row[x * 4 + 0] = byte(rgb[0]);
            row[x * 4 + 1] = byte(rgb[1]);
            row[x * 4 + 2] = byte(rgb[2]);
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

} // namespace bloom::ui::detail
