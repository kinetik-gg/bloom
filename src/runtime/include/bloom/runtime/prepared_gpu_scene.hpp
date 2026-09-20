#pragma once

// CPU-side, immutable preparation of a GPU scene for the first non-media subset:
// Solid -> unparented translation-only Layer Output -> Normal Merge -> Composition Output.
//
// Preparation runs entirely on the CPU and produces an ordered list of GPU execution commands with
// the EXACT resolved operands and geometry the CPU evaluator would use. It never allocates a full
// RGBA CPU image; for a fractional-translation solid it DOES allocate the exact R8 coverage raster
// the CPU PathRaster produces (bounded by the request allowance), and for nothing else. a solid
// command carries its resolved premultiplied pixel; a translation command carries the resolved
// local translation/opacity and source/output windows; a merge command carries the bottom-to-top
// foreground chain; the output command carries the composition window. The test tree replays these
// commands with the existing CPU primitives to prove pixel equality, but that replay is a test
// oracle, not part of preparation.
//
// Command indexes are EXECUTION ORDER ONLY. They are never pixel identity. A semantic key is built
// from the resolved parameters, the input commands' semantic keys, the geometry windows/pixel
// aspect, the operand semantics, and the pinned shader digests. It never contains node IDs, layer
// IDs, plan operation indexes, or the document revision, and it contains the frame time only when
// the actual resolved pixels depend on it (through a curve or a value-graph driver, both resolved
// by the real preflight).
//
// If any reachable operation is outside the subset -- text/shape/media/effects, a parented,
// rotated, or scaled layer, a non-Normal blend, ROI, or a non-linear-rec709 color intent --
// preparation fails closed with Unsupported and no commands, so the caller can take the existing
// CPU path. Animated and driven parameters are supported because they are resolved by the real
// preflight, never assumed.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstdint>
#include <filesystem>
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
// exact immutable R8 coverage the SAME CPU PathRaster produces, plus the resolved premultiplied
// pixel and the layer opacity. A native op is expected to fill RGB from the pixel through the
// coverage, apply opacity, and stay resident; the full RGBA CPU image is never materialised here.
struct GpuSceneCoverageSolidCommand final {
    GpuSceneCommandIndex index = kInvalidGpuSceneCommand;
    OperationIndex sourceOperation = OperationIndex::fromRaw(0);
    render::Rgba32f pixel = render::Rgba32f::transparent();
    float opacity = 1.0F;
    std::shared_ptr<const std::vector<std::uint8_t>> coverage;
    render::ImageWindow outputWindow;
    render::ImageWindow displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    // Coverage raster identity: exact chain matrix, proxy scales, rectangle size, window, PAR.
    std::string geometryKey;
    // Pixel identity: geometryKey + the resolved pixel and opacity + coverage-solid semantics.
    std::string semanticKey;
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

using GpuSceneCommand =
    std::variant<GpuSceneSolidCommand, GpuSceneTranslationCommand, GpuSceneCoverageSolidCommand,
                 GpuSceneUploadCommand, GpuSceneMergeCommand, GpuSceneCompositionOutputCommand>;

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
    explicit CpuGpuSceneBuilder(std::shared_ptr<GpuSceneCoverageCache> coverageCache = nullptr,
                                GpuSceneMediaContext mediaContext = {})
        : coverageCache_(std::move(coverageCache)), mediaContext_(std::move(mediaContext)) {}

    [[nodiscard]] PreparedGpuSceneBuildResult
    build(const std::shared_ptr<const CompiledCompositionPlan>& plan,
          const EvaluationRequest& request, const CancellationToken& cancellation = {}) const;

  private:
    [[nodiscard]] PreparedGpuSceneBuildResult
    buildImpl(const std::shared_ptr<const CompiledCompositionPlan>& plan,
              const EvaluationRequest& request, const CancellationToken& cancellation) const;

    std::shared_ptr<GpuSceneCoverageCache> coverageCache_;
    GpuSceneMediaContext mediaContext_;
};

} // namespace bloom::runtime
