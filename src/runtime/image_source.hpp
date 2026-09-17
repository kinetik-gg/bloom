#pragma once
#include <bloom/media/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <filesystem>

namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache

namespace bloom::runtime::detail {
struct ImageSourceSelection {
    std::filesystem::path path;
    core::Sha256Digest digest;
    media::ImageInterpretation interpretation;
    std::string cacheKey;
    // The disk cache's own content-addressed key (asset digest + member frame + interpretation +
    // Bloom Neutral config digest + decoder identity/version -- see media-io.md "Disk cache").
    // Empty when the selection has no resolvable asset; the disk cache is never consulted then.
    std::string diskCacheKey;
    std::string warning;
    bool available = false;
    bool cancelled = false;
};
[[nodiscard]] ImageSourceSelection selectImageSource(const CompiledImageSource& source,
                                                     core::RationalTime time,
                                                     document::FrameRate rate,
                                                     const std::filesystem::path& base,
                                                     const CancellationToken& cancel);
// `cache` is the evaluator's shared memory cache; null when the request bypasses it (an
// interactive/overridden request, or the request explicitly asked to bypass it) -- the memory
// cache is then neither consulted nor written.
// `diskCache` is the memory-cache-miss fallback (docs/architecture/media-io.md "Disk cache":
// memory -> disk -> decode). Null disables it -- callers pass null for interactive/overridden
// requests, which must never populate or read either cache (the same condition that already
// bypasses `cache` governs this). A decoded disk miss is written back off the calling thread via
// the disk cache's own background writer, so evaluation never waits on the write.
[[nodiscard]] media::ImageResult<render::Rgba32fImage> evaluateImageSource(
    const ImageSourceSelection& selection, render::Rgba32fImageDescriptor composition,
    double horizontalScale, double verticalScale, std::size_t budget, OperationCache* cache,
    const CancellationToken& cancel, media::cache::MediaDiskCache* diskCache = nullptr);
} // namespace bloom::runtime::detail
