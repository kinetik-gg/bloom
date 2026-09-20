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
    // Decode-only identity: validated asset/sequence-member content digest + the requested alpha
    // association. It deliberately excludes the colour interpretation, input/working colour space,
    // configuration revision and the proxy, because none of them change the RAW decoded bytes. The
    // GPU media colour split keys its raw upload cache on this identity, so a changed working space
    // or display reuses the decoded upload while a changed frame is a miss.
    std::string decodeKey;
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

// Decode-only factoring for the GPU media colour split. The connected evaluateImageSource() path
// is unchanged; these two functions expose its halves so the GPU path can decode RAW (no OCIO
// processor, no implicit conversion, no proxy resample hidden in host code), upload the full source
// dimensions/metadata, and let the accepted GPU colour command run the input->working transform.

// Decode the selected source with no input colour processor and no resample. The returned image
// carries the native decoded pixels and the source's own descriptor. Selection, digest validation
// and diagnostics are exactly media::decodeImage()'s; the interpretation is forced to a Raw,
// processor-free decode so no OCIO pass runs.
[[nodiscard]] media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeRawImageSource(const ImageSourceSelection& selection, std::size_t budget,
                     const CancellationToken& cancel);

// Rebase a decoded image into the evaluator's composition descriptor: a (0,0)-origin data window
// sized ceil(sourceExtent * scale), the composition display window and pixel aspect. At unit scale
// this is the evaluator's exact rebase copy; at a fractional proxy scale it is the evaluator's
// exact nearest-neighbour copy. One implementation shared by evaluateImageSource() and the GPU
// split.
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
resampleDecodedImage(const render::Rgba32fImage& decoded,
                     render::Rgba32fImageDescriptor composition, double horizontalScale,
                     double verticalScale, std::size_t budget, const CancellationToken& cancel);
} // namespace bloom::runtime::detail
