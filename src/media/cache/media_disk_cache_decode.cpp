#include <bloom/media/cache/media_disk_cache_decode.hpp>

#include <bloom/core/sha256.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::media::cache {

std::string buildImageCacheKey(const ImageCacheKeyInputs& inputs,
                               const std::string_view decoderIdentity) {
    std::vector<std::byte> bytes;
    const auto appendBytes = [&](const std::span<const std::uint8_t> span) {
        for (const auto value : span)
            bytes.push_back(static_cast<std::byte>(value));
    };
    appendBytes(inputs.contentDigest.bytes());
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<std::byte>(
            (static_cast<std::uint64_t>(inputs.memberFrame) >> shift) & 0xFFU));
    bytes.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(inputs.colorSpace)));
    const auto appendText = [&](const std::string_view value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::byte>((value.size() >> shift) & 0xFFU));
        for (const auto character : value)
            bytes.push_back(static_cast<std::byte>(character));
    };
    appendText(inputs.inputColorSpaceId);
    appendText(inputs.workingColorSpaceId);
    bytes.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(inputs.alphaAssociation)));
    appendBytes(inputs.configDigest.bytes());
    for (const auto character : decoderIdentity)
        bytes.push_back(static_cast<std::byte>(character));
    const auto digest = core::Sha256Hasher::hash(bytes);
    if (!digest)
        return {};
    const auto hex = digest->toLowercaseHex();
    return {hex.begin(), hex.end()};
}

ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeThroughDiskCache(const std::filesystem::path& path, const ImageInterpretation& interpretation,
                       std::optional<core::Sha256Digest> expectedDigest,
                       const std::string& cacheKey, MediaDiskCache* diskCache,
                       const bool writeAsync, const CancelImageWork& cancel,
                       const ImageProgress& progress, const std::size_t pixelBudget,
                       std::shared_ptr<const color::CpuColorSpaceProcessor> processor) {
    const bool cacheUsable = diskCache != nullptr && diskCache->enabled() && !cacheKey.empty();
    if (cacheUsable) {
        if (auto hit = diskCache->find(cacheKey))
            return {std::move(hit), {}, false};
    }
    auto decoded = decodeImage(path, interpretation, std::move(processor), cancel, progress,
                               pixelBudget, expectedDigest);
    if (decoded.value.has_value() && cacheUsable) {
        if (writeAsync)
            diskCache->storeAsync(cacheKey, *decoded.value);
        else
            diskCache->store(cacheKey, *decoded.value);
    }
    return decoded;
}

} // namespace bloom::media::cache
