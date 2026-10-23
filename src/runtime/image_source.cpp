#include "image_source.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"
#include <algorithm>
#include <bloom/media/cache/media_disk_cache_decode.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace bloom::runtime::detail {

ImageSourceSelection
selectImageSource(const CompiledImageSource& source, core::RationalTime time,
                  document::FrameRate rate, const std::filesystem::path& base,
                  const CancellationToken& cancel, const EvaluationColorIntent& colorIntent,
                  OperationCache* revisionCache, const MediaSourceRevisionPolicy revisionPolicy,
                  const std::size_t hostByteBudget, const bool allowRevisionInsertion) {
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
        const auto length = static_cast<std::uint64_t>(asset.manifest.last) -
                            static_cast<std::uint64_t>(asset.manifest.first) + 1;
        // Long-double subtraction avoids signed overflow even for hostile hand-built plans.
        const auto elapsed =
            static_cast<long double>(*frame) - static_cast<long double>(source.startFrame);
        // 2^63 is exactly representable even where long double is only double (MSVC),
        // unlike INT64_MAX which rounds up there. The upper bound is exclusive.
        if (elapsed < -9223372036854775808.0L || elapsed >= 9223372036854775808.0L) {
            selected.warning = "Sequence time exceeds the frame range";
            return selected;
        }
        const auto signedOffset = static_cast<std::int64_t>(elapsed);
        std::uint64_t offset = 0;
        if (signedOffset >= 0) {
            offset = static_cast<std::uint64_t>(signedOffset);
            if (source.loopMode == 1)
                offset %= length;
            else if (source.loopMode == 2 && length > 1) {
                const auto period = 2 * (length - 1);
                offset %= period;
                if (offset >= length)
                    offset = period - offset;
            } else
                offset = std::min(offset, length - 1);
        }
        const auto wanted =
            static_cast<std::int64_t>(static_cast<std::uint64_t>(asset.manifest.first) + offset);
        auto found = std::ranges::upper_bound(asset.manifest.members, wanted, {},
                                              &document::AssetSequenceMember::frame);
        if (found != asset.manifest.members.begin())
            --found;
        locator = &found->locator;
        selected.digest = found->contentDigest;
        memberFrame = found->frame;
        if (asset.manifest.hasGaps())
            selected.warning = "Sequence gaps hold the preceding frame";
    }
    OperationKey revisionKey;
    revisionKey.add(std::string{"immutable-image-source-v1"});
    const auto digestHex = selected.digest.toLowercaseHex();
    revisionKey.add(std::string(digestHex.begin(), digestHex.end()));
    revisionKey.add(locator->path);
    revisionKey.add(locator->relinkHint);
    const auto baseUtf8 = base.generic_u8string();
    revisionKey.add(std::string(reinterpret_cast<const char*>(baseUtf8.data()), baseUtf8.size()));
    const auto revisionGeneration =
        revisionCache ? std::optional{revisionCache->decodedMediaGeneration()} : std::nullopt;
    if (cancel.isCancellationRequested()) {
        selected.cancelled = true;
        return selected;
    }
    if (revisionCache) {
        if (const auto cached = revisionCache->find(revisionKey.bytes(), document::Revision{});
            cached && cached->sourceRevision &&
            cached->sourceRevision->retainedBytes() < hostByteBudget) {
            if (revisionPolicy == MediaSourceRevisionPolicy::Retained) {
                selected.sourceRevision = cached->sourceRevision;
                selected.retainedRevision = true;
            } else {
                // Revalidate: the retained bytes stand for the original only while a cheap identity
                // observation still matches the one that bracketed a full hash of this digest.
                selected.path = media::resolveImagePath(locator->path, locator->relinkHint, base);
                const auto observed = platform::observeSource(selected.path);
                if (observed.digest && *observed.digest == selected.digest &&
                    cached->sourceRevision->probe().contentDigest == selected.digest) {
                    selected.sourceRevision = cached->sourceRevision;
                    selected.retainedRevision = true;
                }
            }
        }
    }
    media::ImageResult<std::shared_ptr<const media::ImageSourceRevision>> capture;
    if (!selected.sourceRevision) {
        selected.path = media::resolveImagePath(locator->path, locator->relinkHint, base);
        capture = media::captureImageSourceRevision(
            selected.path, hostByteBudget, [&] { return cancel.isCancellationRequested(); },
            selected.digest);
        selected.sourceRevision = capture.value ? *capture.value : nullptr;
        selected.cancelled = capture.cancelled;
        if (selected.sourceRevision && revisionCache && allowRevisionInsertion)
            revisionCache->store(revisionKey.bytes(), document::Revision{},
                                 {.image = {},
                                  .values = {},
                                  .bounds = {},
                                  .sourceRevision = selected.sourceRevision},
                                 OperationCacheEntryKind::DecodedMedia, revisionGeneration);
    }
    const auto input = document::effectiveAssetInputColor(asset.interpretation, source.colorSpace,
                                                          source.inputColorSpaceId);
    selected.interpretation.colorSpace = static_cast<media::ImageColorSpace>(input.colorSpace);
    selected.interpretation.inputColorSpaceId = input.inputColorSpaceId;
    selected.interpretation.alphaAssociation = source.premultiply
                                                   ? media::ImageAlphaAssociation::Straight
                                                   : media::ImageAlphaAssociation::Premultiplied;
    selected.available = selected.sourceRevision != nullptr &&
                         selected.sourceRevision->probe().contentDigest == selected.digest;
    if (selected.available) {
        auto config = resolveInputColorConfig(colorIntent);
        if (!config) {
            selected.available = false;
            selected.warning = "The selected OCIO input configuration is unavailable";
        } else {
            const auto resolution = resolveImageInputColorSpace(
                *config, selected.sourceRevision->probe(), selected.interpretation.colorSpace,
                selected.interpretation.inputColorSpaceId);
            selected.inputColorSpaceAutomatic = resolution.automatic;
            selected.resolvedInputColorSpaceName = resolution.name;
            selected.inputColorSpaceWarning = resolution.warning;
            selected.workingColorSpaceId = std::string(config->processColorSpaceId());
            selected.configRevision = config->expectedRevision();
            if (!resolution.warning.empty() && resolution.id.empty())
                selected.warning = resolution.warning;
            if (!resolution.noConversion && resolution.id.empty())
                selected.available = false;
            if (selected.available) {
                selected.interpretation.inputColorSpaceId = resolution.id;
                std::string diagnostic;
                selected.inputProcessor =
                    prepareInputColorProcessor(*config, resolution, diagnostic);
                if (!resolution.noConversion && !selected.inputProcessor) {
                    selected.available = false;
                    selected.warning = diagnostic;
                }
            }
        }
    }
    if (!selected.available && selected.warning.empty())
        selected.warning = capture.diagnostic.empty() ? "Image changed; relink the asset in Assets"
                                                      : capture.diagnostic;
    OperationKey key;
    const auto digest = selected.digest.toLowercaseHex();
    const auto config = selected.configRevision.toLowercaseHex();
    key.add(std::string(digest.begin(), digest.end()));
    key.add(memberFrame);
    key.add(selected.interpretation.colorSpace);
    key.add(selected.interpretation.inputColorSpaceId);
    key.add(selected.workingColorSpaceId);
    const auto revision = selected.configRevision.toLowercaseHex();
    key.add(std::string(revision.begin(), revision.end()));
    key.add(selected.interpretation.alphaAssociation);
    key.add(std::string(config.begin(), config.end()));
    key.add(selected.available);
    key.add(locator->path);
    key.add(locator->relinkHint);
    selected.cacheKey = key.bytes();
    // Decode-only identity: the selected member's content digest plus the requested alpha
    // association. Colour interpretation, input/working colour space, config revision, proxy and
    // path/relink fields are deliberately absent, so a changed working space or display keeps the
    // same decoded upload while a changed frame or alpha association is a miss.
    OperationKey decodeKey;
    decodeKey.add(std::string{"gpu-raw-decode-v1"});
    decodeKey.add(std::string(digest.begin(), digest.end()));
    decodeKey.add(selected.interpretation.alphaAssociation);
    selected.decodeKey = decodeKey.digest();
    // Content-addressed and restart-stable (docs/architecture/media-io.md "Disk cache"): asset
    // digest + member frame + interpretation + Bloom Neutral config digest + decoder identity, so
    // it deliberately omits `path`/`relinkHint`/`available` -- a relink of the same content should
    // still hit the disk cache, and an unavailable selection never reaches evaluateImageSource().
    media::cache::ImageCacheKeyInputs diskInputs;
    diskInputs.contentDigest = selected.digest;
    diskInputs.memberFrame = memberFrame;
    diskInputs.colorSpace = selected.interpretation.colorSpace;
    diskInputs.inputColorSpaceId = selected.interpretation.inputColorSpaceId;
    diskInputs.workingColorSpaceId = selected.workingColorSpaceId;
    diskInputs.alphaAssociation = selected.interpretation.alphaAssociation;
    diskInputs.configDigest = selected.configRevision;
    selected.diskCacheKey =
        media::cache::buildImageCacheKey(diskInputs, media::cache::kImageDecoderIdentity);
    return selected;
}
media::ImageResult<render::Rgba32fImage> evaluateImageSource(
    const ImageSourceSelection& selected, render::Rgba32fImageDescriptor composition,
    double horizontalScale, double verticalScale, std::size_t budget, OperationCache* memoryCache,
    ImageSourceMemoryCacheAccess memoryAccess, const CancellationToken& cancel,
    media::cache::MediaDiskCache* diskCache, media::ImageDecodeStatistics* decodeStatistics) {
    if (decodeStatistics)
        *decodeStatistics = {};
    if (!selected.available)
        return {{}, selected.warning};
    std::shared_ptr<const render::Rgba32fImage> image;
    if (memoryAccess != ImageSourceMemoryCacheAccess::Disabled && memoryCache != nullptr) {
        auto cached = memoryCache->find(selected.cacheKey, document::Revision{});
        if (cached)
            image = cached->image;
    }
    if (!image) {
        // Memory -> disk -> decode, but only an ordinary ReadWrite request may touch the disk
        // cache. A ReadOnly interactive miss decodes directly (null disk cache) and stores nothing,
        // and an explicit Disabled bypass decodes without consulting either cache.
        auto* const decodeDiskCache =
            memoryAccess == ImageSourceMemoryCacheAccess::ReadWrite ? diskCache : nullptr;
        // `budget` is the already-validated caller request allowance (the evaluator and the media
        // split both pass a budget derived from `request.pixelStorageByteLimit`). It is honored as
        // given so a large real source can use the caller's real budget instead of being clamped to
        // the 256 MiB default; the decoder still enforces its own structural ceilings. No
        // artificial min() is applied here.
        auto decoded = media::cache::decodeThroughDiskCache(
            selected.path, selected.interpretation, selected.digest, selected.diskCacheKey,
            decodeDiskCache, /*writeAsync=*/true, [&] { return cancel.isCancellationRequested(); },
            {}, budget, selected.inputProcessor, selected.sourceRevision, decodeStatistics);
        if (!decoded.value.has_value())
            return {{}, decoded.diagnostic, decoded.cancelled};
        image = std::move(*decoded.value);
        if (memoryAccess == ImageSourceMemoryCacheAccess::ReadWrite && memoryCache != nullptr)
            memoryCache->store(selected.cacheKey, document::Revision{},
                               {.image = image, .values = {}, .bounds = {}},
                               OperationCacheEntryKind::DecodedMedia);
    }
    return resampleDecodedImage(*image, composition, horizontalScale, verticalScale, budget,
                                cancel);
}

media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeRawImageSource(const ImageSourceSelection& selected, const std::size_t budget,
                     const CancellationToken& cancel,
                     media::ImageDecodeStatistics* decodeStatistics) {
    if (decodeStatistics)
        *decodeStatistics = {};
    if (!selected.available)
        return {{}, selected.warning};
    media::ImageInterpretation rawInterpretation;
    rawInterpretation.colorSpace = media::ImageColorSpace::Raw;
    rawInterpretation.inputColorSpaceId = {};
    rawInterpretation.alphaAssociation = selected.interpretation.alphaAssociation;
    if (!selected.sourceRevision)
        return {{}, "Image selection has no immutable source revision"};
    return media::decodeImage(
        *selected.sourceRevision, rawInterpretation, {},
        [&cancel] { return cancel.isCancellationRequested(); }, {}, budget, decodeStatistics);
}

media::ImageResult<render::Rgba32fImage>
resampleDecodedImage(const render::Rgba32fImage& decoded,
                     const render::Rgba32fImageDescriptor composition, const double horizontalScale,
                     const double verticalScale, const std::size_t budget,
                     const CancellationToken& cancel) {
    if (decoded.descriptor() == nullptr)
        return {{}, "Decoded image has no descriptor"};
    const auto sourceWindow = decoded.descriptor()->dataWindow();
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
                decoded.pixels()[static_cast<std::size_t>(sourceY) * sourceWindow.extent().width() +
                                 sourceX];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return {{}, "Image source pixel validation failed"};
    return {std::move(*frozen.value()), {}};
}
} // namespace bloom::runtime::detail
