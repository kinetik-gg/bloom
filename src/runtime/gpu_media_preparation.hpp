#ifndef BLOOM_RUNTIME_GPU_MEDIA_PREPARATION_HPP
#define BLOOM_RUNTIME_GPU_MEDIA_PREPARATION_HPP

// Private to src/runtime. Resolves and converts the ImageSource/VideoSource leaves of a compiled
// plan into the immutable upload images the scene builder publishes as GpuSceneUploadCommand. It
// reuses the REAL detail::selectImageSource()/evaluateImageSource() and
// detail::selectVideoSource()/media::video::videoToSceneLinear() the CPU evaluator calls, so
// selection, decode, colour conversion and hashing have exactly one implementation. The builder
// owns no UI dependency and makes no native call; everything here is CPU work on the task thread.

#include "cpu_composition_evaluator_support.hpp"

#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bloom::render {
class Rgba32fImage;
} // namespace bloom::render

namespace bloom::runtime::detail {

// The outcome of preparing one media leaf. A non-empty `failure` is a fail-closed result: the whole
// scene preparation is refused so the caller can take the existing CPU path, rather than silently
// turning a missing or malformed source into a different warning.
struct MediaUploadOutcome final {
    std::shared_ptr<const render::Rgba32fImage> image;
    std::string semanticKey;
    bool cancelled = false;
    bool cacheHit = false;
    bool converted = false;
    std::string failure;
};

[[nodiscard]] MediaUploadOutcome
prepareImageUpload(const CompiledImageSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, std::uint64_t pixelBudget,
                   bool explicitBypass, bool planBypass, const CancellationToken& cancellation,
                   GpuSceneMediaStatistics& statistics);

[[nodiscard]] MediaUploadOutcome
prepareVideoUpload(const CompiledVideoSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, std::uint64_t pixelBudget,
                   bool explicitBypass, bool planBypass, const CancellationToken& cancellation,
                   GpuSceneMediaStatistics& statistics);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_MEDIA_PREPARATION_HPP
