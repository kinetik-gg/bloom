#pragma once

// CPU-side, immutable preparation of a GPU scene for the first non-media subset:
// Solid -> unparented translation-only Layer Output -> Normal Merge -> Composition Output.
//
// Preparation runs entirely on the CPU and produces an ordered list of GPU execution commands with
// the EXACT resolved operands and geometry the CPU evaluator would use. It never allocates a full
// RGBA CPU image and it never builds a per-pixel coverage mask: for a vector source it emits the
// immutable bounded `PathRasterCoverageGeometry` (integer scanline sample spans) that the native
// `GpuPathCoverage` producer rasterizes on the device. A solid command carries its resolved
// premultiplied pixel; a translation command carries the resolved local translation/opacity and
// source/output windows; a merge command carries the bottom-to-top foreground chain; the output
// command carries the composition window. The test tree replays these commands with the existing
// CPU primitives to prove pixel equality, but that replay is a test oracle, not part of
// preparation.
//
// Command indexes are EXECUTION ORDER ONLY. They are never pixel identity. A semantic key is built
// from the resolved parameters, the input commands' semantic keys, the geometry windows/pixel
// aspect, the operand semantics, and the pinned shader digests. It never contains node IDs, layer
// IDs, plan operation indexes, or the document revision, and it contains the frame time only when
// the actual resolved pixels depend on it (through a curve or a value-graph driver, both resolved
// by the real preflight).
//
// A request ROI is resolved exactly as the CPU evaluator resolves it: its process image descriptor
// data window is the requested ROI (validated by the same shared preflight), while every command
// keeps its native source/output window -- a media upload stays full resolution -- and only the
// terminal Composition Output is clipped to the ROI. An ROI edit therefore reuses all unchanged
// upstream content and re-dispatches one clipped command. A transform still samples the full input
// at the ROI edge, and an out-of-resolution ROI is refused by preflight.
//
// If any reachable operation is outside the subset, or a non-linear-rec709 working space reaches an
// operation without verified working-space semantics, preparation fails closed with Unsupported and
// no commands, so the caller can take the existing CPU path. Animated and driven parameters are
// supported because they are resolved by the real preflight, never assumed.

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {
class Rgba32fImage;
} // namespace bloom::render

namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache

namespace bloom::runtime {

class OperationCache;
class CpuCompositionEvaluator;
class GpuPreparedUploadCache;

namespace detail {
class VideoSourceContext;
} // namespace detail

// Execution-order handle. Never an identity input.
using GpuSceneCommandIndex = std::uint32_t;
inline constexpr GpuSceneCommandIndex kInvalidGpuSceneCommand = 0xFFFFFFFFU;

enum class PreparedGpuSceneDiagnosticCode : std::uint8_t {
    None,
    InvalidRequest,
    InvalidPlan,
    UnsupportedOperation,
    UnsupportedTransform,
    UnsupportedBlend,
    UnsupportedRequest,
    MediaUnavailable,
    PixelStorageBudgetExceeded,
    AllocationFailure,
    Cancelled,
    PreflightFailure,
    InternalInvariant,
};

// Per-build CPU work counters. They record decode/conversion/key work only: there is deliberately
// no GPU upload count because the native executor does not exist yet. A build owns exactly one of
// these; it is never shared or mutated concurrently, so an overlapping background build cannot race
// another. A failed build reports its partial counters on the diagnostic below, which is how the
// unsupported-no-decode screen is proven to have touched no media.
struct GpuSceneMediaStatistics final {
    std::uint64_t imageSources = 0;
    std::uint64_t videoSources = 0;
    std::uint64_t imageConversions = 0;
    std::uint64_t videoConversions = 0;
    // Native (GPU) colour-transform commands successfully prepared through the injected OCIO
    // preparer. A warm build serves the same program from the preparer's own content cache; the
    // upload cache hit is what suppresses the decode/convert entirely.
    std::uint64_t ocioCommandPreparations = 0;
    std::uint64_t uploadCacheHits = 0;
    std::uint64_t uploadCacheMisses = 0;
    std::uint64_t uploadKeyConstructions = 0;
    friend bool operator==(const GpuSceneMediaStatistics&,
                           const GpuSceneMediaStatistics&) = default;
};

struct PreparedGpuSceneDiagnostic final {
    PreparedGpuSceneDiagnosticCode code = PreparedGpuSceneDiagnosticCode::None;
    std::string message;
    // CPU work performed before the failure. Zero for every screen that precedes media resolution.
    GpuSceneMediaStatistics mediaStatistics;

    friend bool operator==(const PreparedGpuSceneDiagnostic&,
                           const PreparedGpuSceneDiagnostic&) = default;
};

struct GpuSceneSolidCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    // Tracing only; NOT part of semanticKey.
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    // The exact resolved premultiplied lin_rec709_scene pixel the CPU solid primitive produced.
    render::Rgba32f pixel = render::Rgba32f::transparent();
    render::ImageWindow dataWindow;
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::string semanticKey;
};

struct GpuSceneTranslationCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    render::ImageWindow sourceWindow;
    render::ImageWindow outputWindow;
    // The GPU translation is local to the OUTPUT window and already accounts for the output/source
    // window origin difference, so replay reproduces the CPU LayerTransform translation-only
    // sample.
    double translationX = 0.0;
    double translationY = 0.0;
    float opacity = 1.0F;
    std::string semanticKey;
};

struct GpuSceneMergeCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    // Foreground commands in bottom-to-top composite order (the CPU Merge folds its entries
    // reversed). Replay composites each over an accumulated destination that starts transparent.
    std::vector<GpuSceneCommandIndex> foregrounds;
    render::ImageWindow outputWindow;
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::string semanticKey;
};

struct GpuSceneCompositionOutputCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    // The composition output window (also the resident display window later).
    render::ImageWindow dataWindow;
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::string semanticKey;
};

// A solid layer whose CPU evaluation takes the vector-coverage path (a fractional device grid): the
// immutable bounded `PathRasterCoverageGeometry` the SAME CPU PathRaster produces -- integer
// scanline sample spans, never a CPU per-pixel mask -- plus the resolved premultiplied pixel and
// the layer opacity. The native `GpuPathCoverage` producer rasterizes that geometry on the device
// and `GpuSolid::beginCoveredResident` fills RGB through the resident mask and applies opacity; the
// fill stays resident and the full RGBA CPU image is never materialised here.
//
// Exactly one coverage representation is set:
//   * `geometry` is the Required native vector-coverage pixel work (a fractional Solid, a shape
//     fill/stroke, or a shaped text vector layer) and must be consumed by the GPU producer.
//   * `coverage` is the host FreeType 8-bit glyph bitmap of an integer-grid text leaf. That leaf is
//     the CPU text source's own font rasterization (host font preparation) translated by an exact
//     integer; it is not a PathRaster::coverageRow mask and is not the vector-coverage axis.
struct GpuSceneCoverageSolidCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    render::Rgba32f pixel = render::Rgba32f::transparent();
    float opacity = 1.0F;
    std::shared_ptr<const render::PathRasterCoverageGeometry> geometry;
    std::shared_ptr<const std::vector<std::uint8_t>> coverage;
    render::ImageWindow outputWindow;
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    // Coverage raster identity: exact chain matrix, proxy scales, rectangle size, window, PAR.
    std::string geometryKey;
    // Pixel identity: geometryKey + the resolved pixel and opacity + coverage-solid semantics.
    std::string semanticKey;

    // The one coverage representation this command carries, for charge/identity accounting.
    [[nodiscard]] const void* coverageIdentity() const noexcept {
        return geometry != nullptr ? static_cast<const void*>(geometry.get())
                                   : static_cast<const void*>(coverage.get());
    }
};

// An ImageSource or VideoSource leaf: the exact converted, immutable lin_rec709_scene source image
// the CPU evaluator would hand to the next stage, together with the descriptor it was published
// with. Decoding, colour conversion and hashing happen entirely in the CPU task that builds the
// scene; the command only carries the frozen result so a future native executor can upload it once
// per semantic source. Preparation never uploads anything itself.
struct GpuSceneUploadCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    // Tracing only; NOT part of semanticKey.
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    std::shared_ptr<const render::Rgba32fImage> image;
    render::Rgba32fImageDescriptor descriptor;
    // Source identity: validated asset/sequence/frame identity, interpretation and input/working
    // colour space, config/processor revision, and the proxy/composition descriptor the converted
    // image was produced for. It never contains a node ID, plan index, or frame time.
    std::string semanticKey;
};

// A complete composed affine placement of one resident input: the accepted AffineBilinearV1
// `beginAffineMatrix` form, which covers rotation, uniform/nonuniform/negative scale, anchor plus
// translation, and any precomposed parent matrix including shear. Source-local pixel-centre
// coordinates (0-based within the input's data window) map to absolute output coordinates through
// `matrix`; the output display window and pixel aspect are preserved from the input. The matrix is
// the exact Float64 composed matrix the CPU parent-aware oracle uses, so no node/plan/revision
// identity appears in the key.
struct GpuSceneAffineCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    // The input's own semantic key, carried explicitly so the executor can canonicalize the
    // effective key from fields (never trusting a producer-supplied combined key).
    std::string inputKey;
    // The input data window (source-local layout) and the output data window.
    render::ImageWindow sourceWindow;
    render::ImageWindow outputWindow;
    render::GpuAffineMatrix matrix;
    // Already rounded once to Float32 exactly as the CPU primitive does; finite in [0, 1].
    float opacity = 1.0F;
    // Output pixel aspect (inherited from the input).
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    // Diagnostics/back-compat only: the executor NEVER uses this for a cache lookup or insertion;
    // it recomputes the effective key from the fields above and the actual device selection.
    std::string semanticKey;
};

// A BlendV1 composite of two resident inputs under an explicit core::BlendMode. `source` is the
// foreground and `destination` the backdrop; the output data window is the destination data window,
// and the destination display window and pixel aspect are preserved. The key carries the actually
// used shader artifact digest (Float32 vs the capability-gated Float64 companion) so a device
// capability change never serves a wrongly keyed image, and it always carries the explicit mode so
// a non-Normal blend can never be silently keyed as Normal.
struct GpuSceneBlendCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    GpuSceneCommandIndex source = kInvalidGpuSceneCommand;
    GpuSceneCommandIndex destination = kInvalidGpuSceneCommand;
    // The two inputs' own semantic keys, carried explicitly so the executor can canonicalize the
    // effective key from fields instead of trusting a producer-supplied combined key.
    std::string sourceKey;
    std::string destinationKey;
    core::BlendMode mode = core::kDefaultBlendMode;
    // The source data window and the output data window (== destination data window).
    render::ImageWindow sourceWindow;
    render::ImageWindow outputWindow;
    // Destination display window and pixel aspect are preserved onto the output.
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    // The SHA-256 of the checked-in shader artifact the producer believes was dispatched. Advisory
    // only: the executor canonicalizes the effective f32/f64 pin from `mode` and the native
    // selection rule, so an arbitrary value here can never bless another shader's cached output.
    std::string artifactDigest;
    std::string semanticKey;
};

// One OCIO ProcessEffect transform over a resident RGBA32F input. `program` is the immutable
// runtime-prepared command (the extracted OCIO descriptor plus the compiled SPIR-V artifact and the
// canonical identity covering config/program/resources/uniforms/geometry/output encoding/artifact
// digest). The output has the input's geometry and stays resident RGBA32F; the executor never reads
// it back. The builder extracts/compiles this off the UI thread; the executor only drives it.
struct GpuSceneOcioEffectCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    // The input's own semantic key, carried explicitly so the executor canonicalizes the effective
    // key from fields rather than trusting a producer-supplied combined key.
    std::string inputKey;
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    // The output data window (== input data window) plus the preserved display window/pixel aspect.
    render::ImageWindow outputWindow;
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::string semanticKey;
};

// One PointResampleV1 nearest-neighbour gather of a resident input into a proxy output. It is the
// exact CPU media-image proxy mapping (src/runtime/image_source.cpp evaluateImageSource): the
// output data window is (0, 0, max(1, ceil(sourceWidth*horizontalScale)), max(1, ceil(sourceHeight*
// verticalScale))), the output display window and pixel aspect are preserved from the input, and
// the selected source index is min(extent - 1, (uint32)(x / scale)) evaluated in binary64. This is
// the accepted native primitive that lets a proxied non-identity media source keep its
// full-resolution raw upload and run the input->working OCIO transform on the GPU at the proxy
// geometry.
struct GpuScenePointResampleCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    // The input's own semantic key, carried explicitly so the executor canonicalizes the effective
    // key from fields rather than trusting a producer-supplied combined key.
    std::string inputKey;
    // The input data window (source-local layout) and the derived proxy output data window.
    render::ImageWindow sourceWindow;
    render::ImageWindow outputWindow;
    // The output display window. The proxy resample publishes a NEW image whose display window is
    // the composition/proxy display, which is NOT necessarily the full-resolution upload's display
    // window (a source frame larger than the composition proxy would otherwise inherit an
    // oversized display). Carried explicitly so the executor validates and reports the exact
    // geometry.
    render::ImageWindow displayWindow;
    // The resolved proxy scales, in (0, 1].
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    // Output pixel aspect (inherited from the input).
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    // Diagnostics/back-compat only: the executor NEVER uses this for a cache lookup or insertion;
    // it recomputes the effective key from the fields above.
    std::string semanticKey;
};

using GpuSceneCommand =
    std::variant<GpuSceneSolidCommand, GpuSceneTranslationCommand, GpuSceneCoverageSolidCommand,
                 GpuSceneUploadCommand, GpuSceneMergeCommand, GpuSceneCompositionOutputCommand,
                 GpuSceneAffineCommand, GpuSceneBlendCommand, GpuSceneOcioEffectCommand,
                 GpuScenePointResampleCommand>;

// Canonical, construction-time semantic key builders. Producer code (the scene builder / graph
// worker) must call these rather than hand-format a key, so two independent producers agree and a
// missing field is impossible. Both fold the input command keys, the resolved geometry window, the
// output pixel aspect, the operation kind tag, and the ACTUAL shader artifact digest. The blend
// helper takes `mode` explicitly; there is deliberately no default argument, so a caller cannot
// silently key every blend as Normal.
namespace gpu_scene_key_detail {

inline void appendSemanticDouble(std::string& out, double value) {
    // Locale-independent and round-trip exact. std::to_chars never consults the C++ locale, so a
    // comma-decimal locale cannot split a key; NaN/Inf are rejected by the caller before keying.
    if (value == 0.0) {
        value = 0.0;
    }
    char buffer[40];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec == std::errc{}) {
        out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
    }
}

inline void appendSemanticWindow(std::string& out, const render::ImageWindow& window) {
    out.push_back('|');
    appendSemanticDouble(out, static_cast<double>(window.originX()));
    out.push_back(',');
    appendSemanticDouble(out, static_cast<double>(window.originY()));
    out.push_back(',');
    out.append(std::to_string(window.extent().width()));
    out.push_back('x');
    out.append(std::to_string(window.extent().height()));
}

inline void appendSemanticPixelAspect(std::string& out, const core::PixelAspectRatio& pixelAspect) {
    out.append("|par=");
    out.append(std::to_string(pixelAspect.numerator()));
    out.push_back('/');
    out.append(std::to_string(pixelAspect.denominator()));
}

inline void appendSemanticFloatBits(std::string& out, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    out.push_back('|');
    out.append(std::to_string(bits));
}

} // namespace gpu_scene_key_detail

[[nodiscard]] inline std::string
makeGpuSceneAffineSemanticKey(const std::string& inputKey, const render::ImageWindow& sourceWindow,
                              const render::GpuAffineMatrix& matrix, const float opacity,
                              const render::ImageWindow& outputWindow,
                              const core::PixelAspectRatio& pixelAspect,
                              const std::string& artifactDigest) {
    std::string key = "affine-bilinear-v1|in=";
    key.append(inputKey);
    gpu_scene_key_detail::appendSemanticWindow(key, sourceWindow);
    key.append("|m=");
    gpu_scene_key_detail::appendSemanticDouble(key, matrix.a);
    key.push_back(',');
    gpu_scene_key_detail::appendSemanticDouble(key, matrix.b);
    key.push_back(',');
    gpu_scene_key_detail::appendSemanticDouble(key, matrix.tx);
    key.push_back(',');
    gpu_scene_key_detail::appendSemanticDouble(key, matrix.c);
    key.push_back(',');
    gpu_scene_key_detail::appendSemanticDouble(key, matrix.d);
    key.push_back(',');
    gpu_scene_key_detail::appendSemanticDouble(key, matrix.ty);
    gpu_scene_key_detail::appendSemanticFloatBits(key, opacity);
    gpu_scene_key_detail::appendSemanticWindow(key, outputWindow);
    gpu_scene_key_detail::appendSemanticPixelAspect(key, pixelAspect);
    key.append("|artifact=");
    key.append(artifactDigest);
    return key;
}

[[nodiscard]] inline std::string makeGpuSceneBlendSemanticKey(
    const std::string& sourceKey, const std::string& destinationKey, const core::BlendMode mode,
    const render::ImageWindow& sourceWindow, const render::ImageWindow& outputWindow,
    const core::PixelAspectRatio& pixelAspect, const std::string& artifactDigest) {
    std::string key = "blend-v1|src=";
    key.append(sourceKey);
    key.append("|dst=");
    key.append(destinationKey);
    key.append("|mode=");
    key.append(std::to_string(static_cast<unsigned>(mode)));
    gpu_scene_key_detail::appendSemanticWindow(key, sourceWindow);
    gpu_scene_key_detail::appendSemanticWindow(key, outputWindow);
    gpu_scene_key_detail::appendSemanticPixelAspect(key, pixelAspect);
    key.append("|artifact=");
    key.append(artifactDigest);
    return key;
}

// Effective scene key for one PointResampleV1 command. The input command key, the source data
// window, the output data window, the output pixel aspect, both proxy scales, and the single
// PointResampleV1 artifact token are folded in, so two resamples over different geometry or scales
// never share an output.
[[nodiscard]] inline std::string makeGpuScenePointResampleSemanticKey(
    const std::string& inputKey, const render::ImageWindow& sourceWindow,
    const render::ImageWindow& outputWindow, const render::ImageWindow& displayWindow,
    const double horizontalScale, const double verticalScale,
    const core::PixelAspectRatio& pixelAspect, const std::string& artifactDigest) {
    std::string key = "point-resample-v1|in=";
    key.append(inputKey);
    gpu_scene_key_detail::appendSemanticWindow(key, sourceWindow);
    gpu_scene_key_detail::appendSemanticWindow(key, outputWindow);
    gpu_scene_key_detail::appendSemanticWindow(key, displayWindow);
    key.append("|sx=");
    gpu_scene_key_detail::appendSemanticDouble(key, horizontalScale);
    key.append("|sy=");
    gpu_scene_key_detail::appendSemanticDouble(key, verticalScale);
    gpu_scene_key_detail::appendSemanticPixelAspect(key, pixelAspect);
    key.append("|artifact=");
    key.append(artifactDigest);
    return key;
}

// Effective scene key for one OCIO ProcessEffect command. The program's canonical identity already
// covers config revision, extracted program/resources, uniforms, geometry, and output encoding plus
// the compiled artifact digest; the input command key and the output window/pixel aspect are folded
// in so two commands with the same program over different upstream geometry never share an output.
[[nodiscard]] inline std::string makeGpuSceneOcioEffectSemanticKey(
    const std::string& inputKey, const core::Sha256Digest& programIdentity,
    const render::ImageWindow& outputWindow, const core::PixelAspectRatio& pixelAspect) {
    std::string key = "ocio-effect-v1|in=";
    key.append(inputKey);
    key.append("|program=");
    const auto hex = programIdentity.toLowercaseHex();
    key.append(hex.data(), hex.size());
    gpu_scene_key_detail::appendSemanticWindow(key, outputWindow);
    gpu_scene_key_detail::appendSemanticPixelAspect(key, pixelAspect);
    return key;
}

// Shared, bounded CPU-side store of already converted and frozen source uploads. It keys ONLY on
// the source semantic key -- validated source identity, interpretation/colour configuration, and
// the proxy/composition descriptor -- so an unchanged source is never decoded or converted twice
// while a changed source, frame, colour interpretation or proxy is a miss. The evaluator's own
// media caches retain the DECODED native image, not the converted/premultiplied one, which is why
// this narrow store exists. A null store means every build converts its own source.
class GpuPreparedUploadCache;

// The evaluator-owned media context the builder needs to resolve and convert
// ImageSource/VideoSource leaves. Construction from an evaluator shares its operation cache, video
// decode context, asset base directory and disk cache; the prepared-upload cache is owned here. A
// default context has no caches and an empty base directory, so a scene containing media fails
// closed rather than decoding with different semantics.
//
// Counters are deliberately NOT shared here: a build keeps its own local GpuSceneMediaStatistics
// and publishes it on the result (or the failure diagnostic). That keeps overlapping preparation
// jobs race-free without a shared mutable aggregate; a caller that wants a running total must sum
// the per-result values itself or observe the evaluator's own real media caches.
struct GpuSceneMediaContext final {
    std::shared_ptr<OperationCache> operationCache;
    std::shared_ptr<detail::VideoSourceContext> videoContext;
    std::filesystem::path assetBaseDirectory;
    std::shared_ptr<media::cache::MediaDiskCache> mediaDiskCache;
    std::shared_ptr<GpuPreparedUploadCache> preparedUploadCache;

    [[nodiscard]] static GpuSceneMediaContext
    fromEvaluator(const CpuCompositionEvaluator& evaluator);
};

class PreparedGpuScene final {
  public:
    PreparedGpuScene(const PreparedGpuScene&) = delete;
    PreparedGpuScene& operator=(const PreparedGpuScene&) = delete;
    PreparedGpuScene(PreparedGpuScene&&) noexcept = default;
    PreparedGpuScene& operator=(PreparedGpuScene&&) = delete;
    ~PreparedGpuScene() = default;

    [[nodiscard]] const std::vector<GpuSceneCommand>& commands() const noexcept {
        return commands_;
    }
    // Operation index -> command index, or kInvalidGpuSceneCommand for an unreachable operation.
    [[nodiscard]] const std::vector<GpuSceneCommandIndex>& commandForOperation() const noexcept {
        return commandForOperation_;
    }
    [[nodiscard]] GpuSceneCommandIndex outputCommand() const noexcept { return outputCommand_; }
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const& noexcept {
        return processIdentity_;
    }
    [[nodiscard]] const ProcessFrameIdentity& processIdentity() const&& = delete;
    [[nodiscard]] const std::vector<EvaluatedOperationBounds>& bounds() const noexcept {
        return bounds_;
    }
    [[nodiscard]] const render::Rgba32fImageDescriptor& outputDescriptor() const& noexcept {
        return outputDescriptor_;
    }
    [[nodiscard]] const render::Rgba32fImageDescriptor& outputDescriptor() const&& = delete;
    [[nodiscard]] const GpuSceneMediaStatistics& mediaStatistics() const noexcept {
        return mediaStatistics_;
    }

  private:
    friend class CpuGpuSceneBuilder;
    // Proof-only fixture seam: constructs deliberately controlled command lists for native executor
    // tests. Declared here (never defined in production) so no mutable production creation API is
    // exposed; the definition lives only in the test translation unit.
    friend struct GpuSceneFixtureBuilder;

    PreparedGpuScene(std::vector<GpuSceneCommand> commands,
                     std::vector<GpuSceneCommandIndex> commandForOperation,
                     GpuSceneCommandIndex outputCommand, ProcessFrameIdentity processIdentity,
                     std::vector<EvaluatedOperationBounds> bounds,
                     render::Rgba32fImageDescriptor outputDescriptor,
                     GpuSceneMediaStatistics mediaStatistics) noexcept
        : commands_(std::move(commands)), commandForOperation_(std::move(commandForOperation)),
          outputCommand_(outputCommand), processIdentity_(std::move(processIdentity)),
          bounds_(std::move(bounds)), outputDescriptor_(outputDescriptor),
          mediaStatistics_(mediaStatistics) {}

    std::vector<GpuSceneCommand> commands_;
    std::vector<GpuSceneCommandIndex> commandForOperation_;
    GpuSceneCommandIndex outputCommand_ = kInvalidGpuSceneCommand;
    ProcessFrameIdentity processIdentity_;
    std::vector<EvaluatedOperationBounds> bounds_;
    render::Rgba32fImageDescriptor outputDescriptor_;
    GpuSceneMediaStatistics mediaStatistics_;
};

struct PreparedGpuSceneBuildResult final {
    // Null exactly when preparation failed closed (Unsupported or a diagnosed failure).
    std::shared_ptr<const PreparedGpuScene> scene;
    PreparedGpuSceneDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return scene != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

// Off-UI GPU OCIO preparation injection for media-source colour conversion and image-effect
// commands. The builder resolves the exact configured colour-space transform through the accepted
// OCIO GPU builder, generates the wrapper, and compiles it off the UI thread through the injected
// preparer; it never compiles shader text itself and never consults PATH, an environment variable,
// or a workspace path. A null preparer makes a reachable non-identity transform fail closed
// (Unsupported) so a caller that does not prepare transforms keeps the existing CPU path rather
// than silently substituting identity.
struct GpuSceneOcioContext final {
    std::shared_ptr<GpuOcioProgramPreparer> preparer;
    GpuOcioCompileOptions compileOptions;
};

// Stateless CPU scene preparation. The caller owns the plan and request; nothing is retained beyond
// the returned scene.
class GpuSceneCoverageCache;

class CpuGpuSceneBuilder final {
  public:
    // An optional coverage cache avoids re-rasterizing an unchanged geometry subtree. Null means no
    // cache: every fractional solid rasterizes once per build.
    //
    // The optional media context supplies the evaluator-owned operation/video/disk caches, the
    // asset base directory and the prepared-upload cache used by ImageSource/VideoSource leaves.
    // The default empty context keeps the non-media constructor callers working: a media scene then
    // fails closed with MediaUnavailable instead of decoding with different semantics.
    //
    // The optional OCIO context supplies the off-UI preparer and the qualified compiler tool paths
    // used for a media-source or image-effect colour transform. Its default (no preparer, no tools)
    // keeps every existing caller working and fails a non-identity transform closed rather than
    // mis-rendering it.
    explicit CpuGpuSceneBuilder(std::shared_ptr<GpuSceneCoverageCache> coverageCache = nullptr,
                                GpuSceneMediaContext mediaContext = {},
                                GpuSceneOcioContext ocioContext = {})
        : coverageCache_(std::move(coverageCache)), mediaContext_(std::move(mediaContext)),
          ocioContext_(std::move(ocioContext)) {}

    [[nodiscard]] PreparedGpuSceneBuildResult
    build(const std::shared_ptr<const CompiledCompositionPlan>& plan,
          const EvaluationRequest& request, const CancellationToken& cancellation = {}) const;

  private:
    // Proof-only fixture seam: a private, default-empty checkpoint callback, declared as a friend
    // here and defined only in the test translation unit (mirroring GpuSceneFixtureBuilder), so
    // production exposes no creation API for it and an empty callback leaves behavior unchanged.
    friend struct GpuSceneBuilderTestAccess;

    [[nodiscard]] PreparedGpuSceneBuildResult
    buildImpl(const std::shared_ptr<const CompiledCompositionPlan>& plan,
              const EvaluationRequest& request, const CancellationToken& cancellation) const;

    std::shared_ptr<GpuSceneCoverageCache> coverageCache_;
    GpuSceneMediaContext mediaContext_;
    GpuSceneOcioContext ocioContext_;
    std::function<void()> checkpoint_;
};

} // namespace bloom::runtime
