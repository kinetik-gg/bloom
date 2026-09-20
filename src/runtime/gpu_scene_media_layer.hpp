#ifndef BLOOM_RUNTIME_GPU_SCENE_MEDIA_LAYER_HPP
#define BLOOM_RUNTIME_GPU_SCENE_MEDIA_LAYER_HPP

// Private to src/runtime. The media-specific parts of scene preparation: the reachable-graph screen
// that refuses an unsupported layer BEFORE any media decode, and the two ImageSource/VideoSource
// leaf builders that turn a converted upload into a GpuSceneUploadCommand plus bounds. Keeping them
// here leaves the builder orchestrator concerned only with graph shape, geometry, merge and output.

#include "gpu_scene_preparation_common.hpp"

#include "gpu_media_preparation.hpp"

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace bloom::render {
class Rgba32fImage;
} // namespace bloom::render

namespace bloom::runtime::detail {

// Charges one prepared media command's resident bytes; returns a failure when the request allowance
// would be exceeded. Shared with the solid coverage path's charger via the common header alias.
using GpuSceneMediaChargeBytes = std::function<std::optional<GpuSceneLeafFailure>(
    std::uint64_t width, std::uint64_t height, std::uint64_t bytesPerPixel)>;

// Screens every reachable layer for an unsupported blend or transform BEFORE any media is resolved
// or decoded. Returns a failure when the whole graph must fail closed, or nullopt to continue. An
// inactive layer publishes nothing and is not screened, exactly as in the CPU evaluator.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
screenUnsupportedLayers(const CompiledCompositionPlan& plan, const EvaluationRequest& request,
                        const ResolvedEvaluation& resolved, const CancellationToken& cancellation);

// A prepared media leaf: the frozen converted image, the descriptor it published, its semantic key,
// the resolved output window, and the leaf's bounds. The descriptor and window are held as
// optionals because neither `render::Rgba32fImageDescriptor` nor `render::ImageWindow` has a public
// default constructor, so this out-parameter aggregate must stay default-constructible for the
// orchestrator to declare it before filling it.
struct GpuSceneUploadLeafResult final {
    std::shared_ptr<const render::Rgba32fImage> image;
    std::optional<render::Rgba32fImageDescriptor> descriptor;
    // The final command key: the OCIO effect identity when `program` is set, otherwise the upload
    // identity. The upload command itself always uses `uploadSemanticKey`.
    std::string semanticKey;
    // The decode/upload identity (independent of working space/transform). Empty for a leaf built
    // before the colour split; callers fall back to `semanticKey` in that case.
    std::string uploadSemanticKey;
    // A real input->working OCIO ProcessEffect command to emit over the upload, or null for an
    // exact-identity transform.
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    std::optional<render::ImageWindow> outputWindow;
    EvaluatedOperationBounds bounds;
};

// Builds the upload result for one ImageSource leaf. On success fills `result` and returns nullopt.
// `pixelBudget` is the remaining scene allowance; `hScale`/`vScale` the proxy scales the bounds are
// measured in.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
buildImageUploadLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                     const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                     const GpuSceneMediaContext& context, std::uint64_t pixelBudget, double hScale,
                     double vScale, const GpuSceneMediaChargeBytes& chargeBytes,
                     const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                     GpuSceneUploadLeafResult& result);

// The VideoSource twin of buildImageUploadLeaf.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
buildVideoUploadLeaf(const CompiledVideoSource& source, const EvaluationRequest& request,
                     const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                     const GpuSceneMediaContext& context, std::uint64_t pixelBudget, double hScale,
                     double vScale, const GpuSceneMediaChargeBytes& chargeBytes,
                     const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                     GpuSceneUploadLeafResult& result);

// Emit a prepared media leaf as its scene commands: the upload always, plus a real
// input->working OCIO ProcessEffect command when the leaf carries a program. The OCIO output is
// charged (the raw upload and the effect output are resident concurrently). On success `command`
// and `semanticKey` are the final command index and key; `leaf` is consumed. Returns a failure only
// for a genuinely malformed leaf or an exceeded budget. Defined here so the builder orchestrator
// stays concerned with graph shape rather than media command construction.
template <typename Emit, typename Charge>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitMediaLeafCommands(const OperationIndex operationIndex, GpuSceneUploadLeafResult& leaf,
                      Charge&& charge, Emit&& emit, GpuSceneCommandIndex& command,
                      std::string& semanticKey) {
    if (!leaf.descriptor.has_value()) {
        return GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InternalInvariant,
                                   "the upload leaf produced no image descriptor"};
    }
    const std::string uploadKey =
        leaf.uploadSemanticKey.empty() ? leaf.semanticKey : leaf.uploadSemanticKey;
    GpuSceneUploadCommand upload{.sourceOperation = operationIndex,
                                 .image = std::move(leaf.image),
                                 .descriptor = *leaf.descriptor,
                                 .semanticKey = uploadKey};
    const GpuSceneCommandIndex uploadIndex = emit(std::move(upload));
    if (leaf.program == nullptr) {
        command = uploadIndex;
        semanticKey = uploadKey;
        return std::nullopt;
    }
    const auto extent = leaf.descriptor->dataWindow().extent();
    if (const auto error = charge(extent.width(), extent.height(), sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneOcioEffectCommand ocio{.index = kInvalidGpuSceneCommand,
                                   .sourceOperation = operationIndex,
                                   .input = uploadIndex,
                                   .inputKey = uploadKey,
                                   .program = std::move(leaf.program),
                                   .outputWindow = leaf.descriptor->dataWindow(),
                                   .displayWindow = leaf.descriptor->displayWindow(),
                                   .pixelAspect = leaf.descriptor->pixelAspect(),
                                   .semanticKey = {}};
    ocio.semanticKey = makeGpuSceneOcioEffectSemanticKey(ocio.inputKey, ocio.program->identity(),
                                                         ocio.outputWindow, ocio.pixelAspect);
    semanticKey = ocio.semanticKey;
    command = emit(std::move(ocio));
    return std::nullopt;
}

// The bounded GPU colour split leaves. They call prepareImageColorLeaf()/prepareVideoColorLeaf(),
// so a non-identity transform yields a RAW full-source upload plus a real input->working OCIO
// command; an exact-identity transform yields the unchanged connected upload with a null program.
[[nodiscard]] std::optional<GpuSceneLeafFailure>
buildImageColorLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                    const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                    const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                    std::uint64_t pixelBudget, double hScale, double vScale,
                    const GpuSceneMediaChargeBytes& chargeBytes,
                    const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                    GpuSceneUploadLeafResult& result);

[[nodiscard]] std::optional<GpuSceneLeafFailure>
buildVideoColorLeaf(const CompiledVideoSource& source, const EvaluationRequest& request,
                    const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                    const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                    std::uint64_t pixelBudget, double hScale, double vScale,
                    const GpuSceneMediaChargeBytes& chargeBytes,
                    const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics,
                    GpuSceneUploadLeafResult& result);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_MEDIA_LAYER_HPP
