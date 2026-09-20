#ifndef BLOOM_RUNTIME_GPU_MEDIA_PREPARATION_HPP
#define BLOOM_RUNTIME_GPU_MEDIA_PREPARATION_HPP

// Private to src/runtime. Resolves the ImageSource/VideoSource leaves of a compiled plan into the
// immutable commands the scene builder publishes.
//
// Two families live here:
//
//   * prepareImageUpload()/prepareVideoUpload() are the original connected path. They reuse the
//     REAL detail::selectImageSource()/evaluateImageSource() and
//     detail::selectVideoSource()/media::video::videoToSceneLinear() the CPU evaluator calls, so
//     selection, decode, colour conversion and hashing have exactly one implementation. The
//     resulting upload carries the CONVERTED lin_rec709_scene pixels.
//
//   * prepareImageColorLeaf()/prepareVideoColorLeaf() are the bounded GPU colour split. Decode and
//     (for a real transform) the input->working OCIO conversion are separated: the upload carries
//     the RAW decoded pixels under a decode-only identity, and a real PreparedGpuOcioCommand
//     resolves the input colour space to the working space. The caller emits an upload command and,
//     when a program is present, a GpuSceneOcioEffectCommand over it. The decode/upload identity is
//     independent of the working space, so editing a layer or a display never re-decodes.
//
// Both families select through the production functions, so there is no competing media path.

#include "cpu_composition_evaluator_support.hpp"

#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
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
//
// `uploadSemanticKey` is the decode/upload identity: what makes the DECODED bytes differ. The
// prepared-upload cache and the emitted GpuSceneUploadCommand use it. `program` is null exactly
// when the resolved input->working transform is exact identity, in which case `semanticKey` equals
// `uploadSemanticKey`; otherwise `semanticKey` is the OCIO effect identity over the upload key and
// the program. `image` always carries the command's resident pixels (converted for the connected
// path, raw for the split path).
struct MediaUploadOutcome final {
    std::shared_ptr<const render::Rgba32fImage> image;
    std::string semanticKey;
    std::string uploadSemanticKey;
    std::shared_ptr<const PreparedGpuOcioCommand> program;
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

// The bounded media-source GPU colour path for a still image. When the resolved input colour space
// equals the working colour space (or the source has no conversion), this is the exact connected
// path and `program` is null. Otherwise it decodes the source RAW -- no OCIO colour pass and no
// proxy resample hidden in host preparation -- applies the CPU's exact nearest proxy resample to
// the raw bytes, uploads the result under the decode-only identity, and prepares the real
// input->working OCIO ProcessEffect command through `ocioContext.preparer`.
//
// A null `ocioContext.preparer` for a non-identity transform fails closed (Unsupported) so the
// caller keeps the CPU reference path; it is never substituted with identity.
[[nodiscard]] MediaUploadOutcome
prepareImageColorLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                      const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                      const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                      std::uint64_t pixelBudget, bool explicitBypass, bool planBypass,
                      const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics);

// The VideoSource twin of prepareImageColorLeaf. Video's YUV->RGB transfer/primaries conversion is
// codec-side plane conversion and stays host preparation; the resolved config OCIO processor is the
// colour pass. Until the codec-side split lands (see work-result.md) a non-identity video transform
// fails closed rather than hiding the OCIO pass in host code and calling it GPU.
[[nodiscard]] MediaUploadOutcome
prepareVideoColorLeaf(const CompiledVideoSource& source, const EvaluationRequest& request,
                      const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                      const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                      std::uint64_t pixelBudget, bool explicitBypass, bool planBypass,
                      const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_MEDIA_PREPARATION_HPP
