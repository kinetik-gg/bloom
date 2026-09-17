#pragma once

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/media/image.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// The single decode-through-caches entry point shared by the runtime evaluator
// (src/runtime/image_source.cpp) and the UI Asset Controller (src/ui/asset_controller.cpp), so
// "the same store" in docs/architecture/media-io.md is literally one code path rather than two
// call sites that happen to agree.
namespace bloom::media::cache {

// The single decoder-identity string both call sites fold into their disk-cache key (see
// buildImageCacheKey() below). Owned here, not by either caller, so the two can never drift apart
// and silently stop sharing entries. Bump it whenever the decode or Bloom-Neutral-conversion
// pipeline decodeImage() runs changes in a way that could change decoded pixels for the same
// source bytes and interpretation -- an entry keyed on the old identity is never read back
// (docs/architecture/media-io.md "Disk cache": "cache key includes the decoder's identity so a
// decoder upgrade invalidates entries").
inline constexpr std::string_view kImageDecoderIdentity = "bloom-image-decode-2";

// The inputs that make a decoded image's on-disk identity: everything selectImageSource() /
// selectThumbnail() already resolve per docs/architecture/media-io.md's cache-key contract (asset
// content digest, member/frame, interpretation, and the Bloom Neutral config digest), EXCLUDING
// decoder identity/version, which buildImageCacheKey() folds in separately so callers cannot
// forget it.
struct ImageCacheKeyInputs final {
    core::Sha256Digest contentDigest;
    std::int64_t memberFrame = 0;
    ImageColorSpace colorSpace = ImageColorSpace::Auto;
    ImageAlphaAssociation alphaAssociation = ImageAlphaAssociation::Straight;
    core::Sha256Digest configDigest;

    friend bool operator==(const ImageCacheKeyInputs&, const ImageCacheKeyInputs&) = default;
};

// A fixed-length lowercase SHA-256 hex digest over `inputs` and `decoderIdentity` (pass
// kImageDecoderIdentity above unless a caller has its own reason to diverge).
[[nodiscard]] std::string buildImageCacheKey(const ImageCacheKeyInputs& inputs,
                                             std::string_view decoderIdentity);

// memory caches stay the caller's own (OperationCache for the evaluator, the small proxy image map
// for Asset Controller); this is the disk stage between them and decodeImage(): a disk hit skips
// decodeImage() entirely, and a disk miss decodes then (synchronously or via the cache's own
// background writer thread, per `writeAsync`) stores the result.
//
// `diskCache` may be null or disabled: behaves exactly like calling media::decodeImage() directly.
[[nodiscard]] ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeThroughDiskCache(const std::filesystem::path& path, ImageInterpretation interpretation,
                       std::optional<core::Sha256Digest> expectedDigest,
                       const std::string& cacheKey, MediaDiskCache* diskCache, bool writeAsync,
                       const CancelImageWork& cancel = {}, const ImageProgress& progress = {},
                       std::size_t pixelBudget = kMaxImageStorageBytes);

} // namespace bloom::media::cache
