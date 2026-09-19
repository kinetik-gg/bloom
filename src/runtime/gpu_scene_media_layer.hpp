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
    std::string semanticKey;
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

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_MEDIA_LAYER_HPP
