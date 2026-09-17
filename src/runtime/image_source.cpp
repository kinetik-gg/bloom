#include "image_source.hpp"
#include "operation_key.hpp"
#include <algorithm>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/media/cache/media_disk_cache_decode.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace bloom::runtime::detail {

ImageSourceSelection selectImageSource(const CompiledImageSource& source, core::RationalTime time,
                                       document::FrameRate rate, const std::filesystem::path& base,
                                       const CancellationToken& cancel) {
    ImageSourceSelection selected;
    if (!source.asset) {
        selected.warning = "Image asset is missing; choose an asset or relink in Assets";
        selected.cacheKey = "missing-asset";
        return selected;
    }
    const auto& asset = *source.asset;
    const document::AssetLocator* locator = &asset.locator;
    selected.digest = asset.contentDigest;
    std::int64_t memberFrame = 0;
    if (asset.kind == document::AssetKind::Sequence) {
        const auto frame = valueGraphFrameIndex(time, rate);
        if (!frame || asset.manifest.members.empty()) {
            selected.warning = "Sequence time is outside the supported range";
            return selected;
        }
        const auto length = asset.manifest.last - asset.manifest.first + 1;
        // Long-double subtraction avoids signed overflow even for hostile hand-built plans.
        const auto elapsed =
            static_cast<long double>(*frame) - static_cast<long double>(source.startFrame);
        if (elapsed < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
            elapsed > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
            selected.warning = "Sequence time exceeds the frame range";
            return selected;
        }
        auto offset = static_cast<std::int64_t>(elapsed);
        if (offset < 0)
            offset = 0;
        else if (source.loopMode == 1)
            offset %= length;
        else if (source.loopMode == 2 && length > 1) {
            const auto period = 2 * (length - 1);
            offset %= period;
            if (offset >= length)
                offset = period - offset;
        } else
            offset = std::min(offset, length - 1);
        const auto wanted = asset.manifest.first + offset;
        auto found = std::ranges::upper_bound(asset.manifest.members, wanted, {},
                                              &document::AssetSequenceMember::frame);
        if (found != asset.manifest.members.begin())
            --found;
        locator = &found->locator;
        selected.digest = found->contentDigest;
        memberFrame = found->frame;
        if (!asset.manifest.gaps.empty())
            selected.warning = "Sequence gaps hold the preceding frame";
    }
    selected.path = media::resolveImagePath(locator->path, locator->relinkHint, base);
    selected.interpretation.colorSpace = static_cast<media::ImageColorSpace>(
        source.colorSpace == 0 ? static_cast<std::int64_t>(asset.interpretation.colorSpace)
                               : source.colorSpace);
    selected.interpretation.alphaAssociation = source.premultiply
                                                   ? media::ImageAlphaAssociation::Straight
                                                   : media::ImageAlphaAssociation::Premultiplied;
    const auto probe =
        media::probeImage(selected.path, [&] { return cancel.isCancellationRequested(); });
    selected.cancelled = probe.cancelled;
    selected.available = probe.value.has_value() && probe.value->contentDigest == selected.digest;
    if (!selected.available)
        selected.warning = probe.value.has_value() ? "Image changed; relink the asset in Assets"
                                                   : probe.diagnostic;
    OperationKey key;
    const auto digest = selected.digest.toLowercaseHex();
    const auto config = color::kBloomNeutralV1ConfigDigest.toLowercaseHex();
    key.add(std::string(digest.begin(), digest.end()));
    key.add(memberFrame);
    key.add(selected.interpretation.colorSpace);
    key.add(selected.interpretation.alphaAssociation);
    key.add(std::string(config.begin(), config.end()));
    key.add(selected.available);
    key.add(locator->path);
    key.add(locator->relinkHint);
    selected.cacheKey = key.bytes();
    // Content-addressed and restart-stable (docs/architecture/media-io.md "Disk cache"): asset
    // digest + member frame + interpretation + Bloom Neutral config digest + decoder identity, so
    // it deliberately omits `path`/`relinkHint`/`available` -- a relink of the same content should
    // still hit the disk cache, and an unavailable selection never reaches evaluateImageSource().
    media::cache::ImageCacheKeyInputs diskInputs;
    diskInputs.contentDigest = selected.digest;
    diskInputs.memberFrame = memberFrame;
    diskInputs.colorSpace = selected.interpretation.colorSpace;
    diskInputs.alphaAssociation = selected.interpretation.alphaAssociation;
    diskInputs.configDigest = color::kBloomNeutralV1ConfigDigest;
    selected.diskCacheKey =
        media::cache::buildImageCacheKey(diskInputs, media::cache::kImageDecoderIdentity);
    return selected;
}
media::ImageResult<render::Rgba32fImage>
evaluateImageSource(const ImageSourceSelection& selected,
                    render::Rgba32fImageDescriptor composition, double horizontalScale,
                    double verticalScale, std::size_t budget, OperationCache* cache,
                    const CancellationToken& cancel, media::cache::MediaDiskCache* diskCache) {
    if (!selected.available)
        return {{}, selected.warning};
    auto cached =
        cache != nullptr ? cache->find(selected.cacheKey, document::Revision{}) : std::nullopt;
    std::shared_ptr<const render::Rgba32fImage> image = cached ? cached->image : nullptr;
    if (!image) {
        // Memory -> disk -> decode. A disk hit skips decodeImage() entirely; a disk miss decodes
        // and hands the write to the disk cache's own background thread (writeAsync = true) so
        // this call -- running on an evaluation thread -- never waits on it.
        auto decoded = media::cache::decodeThroughDiskCache(
            selected.path, selected.interpretation, selected.digest, selected.diskCacheKey,
            diskCache, /*writeAsync=*/true, [&] { return cancel.isCancellationRequested(); }, {},
            std::min(budget, media::kMaxImageStorageBytes));
        if (!decoded.value.has_value())
            return {{}, decoded.diagnostic, decoded.cancelled};
        image = std::move(*decoded.value);
        if (cache != nullptr)
            cache->store(selected.cacheKey, document::Revision{},
                         {.image = image, .values = {}, .bounds = {}},
                         OperationCacheEntryKind::DecodedMedia);
    }
    const auto sourceWindow = image->descriptor()->dataWindow();
    const auto width = static_cast<std::uint64_t>(
        std::max(1.0, std::ceil(sourceWindow.extent().width() * horizontalScale)));
    const auto height = static_cast<std::uint64_t>(
        std::max(1.0, std::ceil(sourceWindow.extent().height() * verticalScale)));
    const auto window = render::ImageWindow::create(0, 0, width, height);
    if (!window)
        return {{}, "Image proxy dimensions are invalid"};
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), composition.displayWindow(), composition.pixelAspect());
    if (!descriptor)
        return {{}, "Image source descriptor is invalid"};
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return {{}, "Image source exceeds the pixel budget"};
    for (std::uint64_t y = 0; y < height; ++y) {
        if (cancel.isCancellationRequested())
            return {{}, {}, true};
        auto row = builder.value()->row(static_cast<std::int64_t>(y));
        const auto sourceY =
            std::min(sourceWindow.extent().height() - 1,
                     static_cast<std::uint32_t>(static_cast<double>(y) / verticalScale));
        for (std::uint64_t x = 0; x < width; ++x) {
            const auto sourceX =
                std::min(sourceWindow.extent().width() - 1,
                         static_cast<std::uint32_t>(static_cast<double>(x) / horizontalScale));
            (*row.value())[x] =
                image->pixels()[static_cast<std::size_t>(sourceY) * sourceWindow.extent().width() +
                                sourceX];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return {{}, "Image source pixel validation failed"};
    return {std::move(*frozen.value()), {}};
}
} // namespace bloom::runtime::detail
