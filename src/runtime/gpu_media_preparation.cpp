#include "gpu_media_preparation.hpp"

#include "image_source.hpp"
#include "operation_key.hpp"
#include "video_source.hpp"

#include "cpu_composition_evaluator_support.hpp"

#include <bloom/media/image.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/media/video/colour.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <variant>

namespace bloom::runtime {

GpuSceneMediaContext GpuSceneMediaContext::fromEvaluator(const CpuCompositionEvaluator& evaluator) {
    GpuSceneMediaContext context;
    context.operationCache = evaluator.operationCache();
    context.videoContext = evaluator.videoContext();
    context.assetBaseDirectory = evaluator.assetBaseDirectory();
    context.mediaDiskCache = evaluator.mediaDiskCache();
    context.preparedUploadCache = std::make_shared<GpuPreparedUploadCache>();
    return context;
}

} // namespace bloom::runtime

namespace bloom::runtime::detail {
namespace {

void addWindow(OperationKey& key, const render::ImageWindow window) {
    key.add(window.originX());
    key.add(window.originY());
    key.add(window.extent().width());
    key.add(window.extent().height());
}

void addPixelAspect(OperationKey& key, const core::PixelAspectRatio ratio) {
    key.add(ratio.numerator());
    key.add(ratio.denominator());
}

// The source upload identity. It carries only what makes the CONVERTED pixels differ: the validated
// selection identity (asset/sequence/frame digest, interpretation, input/working colour space and
// configuration/processor revision), the proxy scales the conversion ran at, and the composition
// display window and pixel aspect its descriptor was built for. A layer transform, node id, plan
// index or frame time never enters it, so an unchanged source behind a changed transform is a hit.
[[nodiscard]] std::string uploadSemanticKey(const std::string_view kind,
                                            const std::string& selectionKey,
                                            const ResolvedEvaluation& resolved) {
    OperationKey key;
    key.add(std::string{"gpu-upload-v1"});
    key.add(std::string{kind});
    key.add(selectionKey);
    key.add(resolved.horizontalScale);
    key.add(resolved.verticalScale);
    addWindow(key, resolved.imageDescriptor.displayWindow());
    addPixelAspect(key, resolved.imageDescriptor.pixelAspect());
    return key.digest();
}

[[nodiscard]] std::size_t byteBudget(const std::uint64_t pixels) {
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(pixels, std::numeric_limits<std::size_t>::max()));
}

} // namespace

MediaUploadOutcome
prepareImageUpload(const CompiledImageSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                   const bool explicitBypass, const bool planBypass,
                   const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    MediaUploadOutcome outcome;
    ++statistics.imageSources;
    const auto selection =
        selectImageSource(source, request.time, plan.format().frameRate(),
                          context.assetBaseDirectory, cancellation, request.colorIntent);
    if (selection.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.available) {
        outcome.failure =
            selection.warning.empty() ? "Image source is unavailable" : selection.warning;
        return outcome;
    }
    outcome.semanticKey = uploadSemanticKey("image", selection.cacheKey, resolved);
    ++statistics.uploadKeyConstructions;

    if (!explicitBypass && context.preparedUploadCache != nullptr) {
        if (auto cached = context.preparedUploadCache->find(outcome.semanticKey)) {
            outcome.image = std::move(cached);
            outcome.cacheHit = true;
            ++statistics.uploadCacheHits;
            return outcome;
        }
        ++statistics.uploadCacheMisses;
    }

    // Exactly the evaluator's bypass matrix: only the explicit request bypass disables the still
    // image memory cache; an interactive (overridden) plan gets it read-only. The disk cache is
    // never consulted or written for either bypass.
    auto* const memoryCache = explicitBypass ? nullptr : context.operationCache.get();
    const auto memoryAccess = explicitBypass
                                  ? ImageSourceMemoryCacheAccess::Disabled
                                  : (planBypass ? ImageSourceMemoryCacheAccess::ReadOnly
                                                : ImageSourceMemoryCacheAccess::ReadWrite);
    auto* const diskCache = (explicitBypass || planBypass) ? nullptr : context.mediaDiskCache.get();
    auto image = evaluateImageSource(selection, resolved.imageDescriptor, resolved.horizontalScale,
                                     resolved.verticalScale, byteBudget(pixelBudget), memoryCache,
                                     memoryAccess, cancellation, diskCache);
    ++statistics.imageConversions;
    if (image.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!image.value.has_value()) {
        outcome.failure =
            image.diagnostic.empty() ? "Image source could not be evaluated" : image.diagnostic;
        return outcome;
    }
    outcome.image = std::make_shared<const render::Rgba32fImage>(std::move(*image.value));
    outcome.converted = true;
    // A gesture miss is never inserted, exactly as the evaluator never inserts a decoded source
    // entry on an interactive miss; an explicit bypass touches no cache at all.
    if (!explicitBypass && !planBypass && context.preparedUploadCache != nullptr) {
        context.preparedUploadCache->store(outcome.semanticKey, outcome.image);
    }
    return outcome;
}

MediaUploadOutcome
prepareVideoUpload(const CompiledVideoSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                   const bool explicitBypass, const bool planBypass,
                   const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    MediaUploadOutcome outcome;
    ++statistics.videoSources;
    if (context.videoContext == nullptr) {
        outcome.failure = "Video decode context is unavailable";
        return outcome;
    }
    const auto selection = selectVideoSource(source, request.time, plan.format().frameRate(),
                                             context.assetBaseDirectory, *context.videoContext,
                                             true, cancellation, request.colorIntent);
    if (selection.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.frame) {
        outcome.failure =
            selection.warning.empty() ? "Video source is unavailable" : selection.warning;
        return outcome;
    }
    outcome.semanticKey = uploadSemanticKey("video", selection.cacheKey, resolved);
    ++statistics.uploadKeyConstructions;

    if (!explicitBypass && context.preparedUploadCache != nullptr) {
        if (auto cached = context.preparedUploadCache->find(outcome.semanticKey)) {
            outcome.image = std::move(cached);
            outcome.cacheHit = true;
            ++statistics.uploadCacheHits;
            return outcome;
        }
        ++statistics.uploadCacheMisses;
    }

    auto converted = media::video::videoToSceneLinear(
        *selection.frame, selection.interpretation, selection.inputColorSpaceId,
        selection.inputProcessor, resolved.imageDescriptor, resolved.horizontalScale,
        resolved.verticalScale, byteBudget(pixelBudget),
        [&cancellation] { return cancellation.isCancellationRequested(); });
    ++statistics.videoConversions;
    if (const auto* error = std::get_if<media::provider::Unavailable>(&converted)) {
        outcome.cancelled = error->reason == media::provider::Error::Cancelled;
        if (!outcome.cancelled) {
            outcome.failure =
                error->detail.empty() ? "Video frame could not be converted" : error->detail;
        }
        return outcome;
    }
    auto& pixels = std::get<render::Rgba32fImage>(converted);
    outcome.image = std::make_shared<const render::Rgba32fImage>(std::move(pixels));
    outcome.converted = true;
    if (!explicitBypass && !planBypass && context.preparedUploadCache != nullptr) {
        context.preparedUploadCache->store(outcome.semanticKey, outcome.image);
    }
    return outcome;
}

} // namespace bloom::runtime::detail
