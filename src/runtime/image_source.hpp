#pragma once
#include <bloom/media/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
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
    std::shared_ptr<const color::CpuColorSpaceProcessor> inputProcessor;
    std::string resolvedInputColorSpaceName;
    std::string inputColorSpaceWarning;
    std::string workingColorSpaceId;
    core::Sha256Digest configRevision;
    bool inputColorSpaceAutomatic = false;
    std::string cacheKey;
    // The disk cache's own content-addressed key (asset digest + member frame + interpretation +
    // input/working colour-space ids + config revision + decoder identity/version -- see
    // media-io.md "Disk cache").
    // Empty when the selection has no resolvable asset; the disk cache is never consulted then.
    std::string diskCacheKey;
    std::string warning;
    bool available = false;
    bool cancelled = false;
};
[[nodiscard]] ImageSourceSelection selectImageSource(
    const CompiledImageSource& source, core::RationalTime time, document::FrameRate rate,
    const std::filesystem::path& base, const CancellationToken& cancel,
    const EvaluationColorIntent& colorIntent = EvaluationColorIntent::LinearRec709Scene);
// CACHE-1: how evaluateImageSource() may use the evaluator's shared memory cache for an
// already-verified, immutable native decoded still-image. This governs only the source decode
// entry; derived operation memoization is gated separately by the evaluator's own `cache` pointer.
enum class ImageSourceMemoryCacheAccess : std::uint8_t {
    // Explicit evaluation bypass (`request.bypassOperationCache`): the still-image memory and disk
    // entries are neither read nor written, so the call is an uncached re-decode. This flag governs
    // derived operation memoization and the still-image source caches only; it does not touch the
    // video decoded cache or colour-processor caches.
    Disabled,
    // Interactive/overridden plan (`plan->bypassOperationCache()`): read a warmed native decoded
    // still-image entry, but never insert one. A gesture miss decodes directly and is not stored,
    // so gesture data cannot enter the memory cache under the source key.
    ReadOnly,
    // Ordinary request: memory -> disk -> decode, inserting the decoded entry on a miss.
    ReadWrite,
};
// `memoryCache` is the evaluator's shared memory cache and `memoryAccess` says whether this call
// may read it, read-and-write it, or ignore it entirely.
// `diskCache` is the memory-cache-miss fallback (docs/architecture/media-io.md "Disk cache":
// memory -> disk -> decode). Null disables it -- callers pass null for interactive/overridden
// requests, which must never populate or read the disk cache. It is consulted and written only
// under ReadWrite; a ReadOnly interactive miss decodes directly with no disk read or write. A
// decoded disk miss is written back off the calling thread via the disk cache's own background
// writer, so evaluation never waits on the write.
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
evaluateImageSource(const ImageSourceSelection& selection,
                    render::Rgba32fImageDescriptor composition, double horizontalScale,
                    double verticalScale, std::size_t budget, OperationCache* memoryCache,
                    ImageSourceMemoryCacheAccess memoryAccess, const CancellationToken& cancel,
                    media::cache::MediaDiskCache* diskCache = nullptr);
} // namespace bloom::runtime::detail
