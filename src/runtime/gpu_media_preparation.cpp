#include "gpu_media_preparation.hpp"

#include "image_source.hpp"
#include "input_color_context.hpp"
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
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
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

// The source upload identity used by the connected path. It carries only what makes the CONVERTED
// pixels differ: the validated selection identity (asset/sequence/frame digest, interpretation,
// input/working colour space and configuration/processor revision), the proxy scales the conversion
// ran at, and the composition display window and pixel aspect its descriptor was built for. A layer
// transform, node id, plan index or frame time never enters it, so an unchanged source behind a
// changed transform is a hit.
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

[[nodiscard]] bool resolvedProxyIsUnit(const ResolvedEvaluation& resolved) {
    return resolved.horizontalScale == 1.0 && resolved.verticalScale == 1.0;
}

// The descriptor for the raw upload: the source's own data window extent at (0,0) with the
// composition display window and pixel aspect. Both the unit-scale and proxy cases keep this
// full-resolution data window.
[[nodiscard]] std::optional<render::Rgba32fImageDescriptor>
rawUploadDescriptor(const render::Rgba32fImage& source,
                    const render::Rgba32fImageDescriptor& composition) {
    if (source.descriptor() == nullptr) {
        return std::nullopt;
    }
    const auto sourceWindow = source.descriptor()->dataWindow();
    const auto window = render::ImageWindow::create(0, 0, sourceWindow.extent().width(),
                                                    sourceWindow.extent().height());
    if (!window) {
        return std::nullopt;
    }
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), composition.displayWindow(), composition.pixelAspect());
    if (!descriptor) {
        return std::nullopt;
    }
    return *descriptor.value();
}

// The proxy output descriptor: the media-image proxy window max(1, ceil(sourceExtent*scale)) at
// (0,0) with the composition display window and pixel aspect, exactly the CPU evaluateImageSource()
// oracle. The source extent is the raw upload's data-window extent (the FULL source dimensions).
[[nodiscard]] std::optional<render::Rgba32fImageDescriptor>
proxyOutputDescriptor(const render::Rgba32fImageDescriptor& source,
                      const ResolvedEvaluation& resolved) {
    const auto sourceWindow = source.dataWindow();
    const auto width = static_cast<std::uint64_t>(std::max(
        1.0,
        std::ceil(static_cast<double>(sourceWindow.extent().width()) * resolved.horizontalScale)));
    const auto height = static_cast<std::uint64_t>(std::max(
        1.0,
        std::ceil(static_cast<double>(sourceWindow.extent().height()) * resolved.verticalScale)));
    const auto window = render::ImageWindow::create(0, 0, width, height);
    if (!window) {
        return std::nullopt;
    }
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), resolved.imageDescriptor.displayWindow(),
        resolved.imageDescriptor.pixelAspect());
    if (!descriptor) {
        return std::nullopt;
    }
    return *descriptor.value();
}

// The decoded/converted upload the connected path and every identity colour leaf publish. Exactly
// the evaluator's bypass matrix: only the explicit request bypass disables the still image memory
// cache; an interactive (overridden) plan gets it read-only; the disk cache is never consulted or
// written for either bypass.
[[nodiscard]] MediaUploadOutcome evaluateConvertedImageUpload(
    const ImageSourceSelection& selection, const ResolvedEvaluation& resolved,
    const GpuSceneMediaContext& context, const std::uint64_t pixelBudget, const bool explicitBypass,
    const bool planBypass, const CancellationToken& cancellation,
    GpuSceneMediaStatistics& statistics) {
    MediaUploadOutcome outcome;
    outcome.semanticKey = uploadSemanticKey("image", selection.cacheKey, resolved);
    outcome.uploadSemanticKey = outcome.semanticKey;
    ++statistics.uploadKeyConstructions;
    if (!explicitBypass && context.preparedUploadCache != nullptr) {
        if (auto cached = context.preparedUploadCache->find(outcome.uploadSemanticKey)) {
            outcome.image = std::move(cached);
            outcome.cacheHit = true;
            ++statistics.uploadCacheHits;
            return outcome;
        }
        ++statistics.uploadCacheMisses;
    }
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
    if (!explicitBypass && !planBypass && context.preparedUploadCache != nullptr) {
        context.preparedUploadCache->store(outcome.uploadSemanticKey, outcome.image);
    }
    return outcome;
}

[[nodiscard]] MediaUploadOutcome evaluateConvertedVideoUpload(
    const VideoSourceSelection& selection, const ResolvedEvaluation& resolved,
    const GpuSceneMediaContext& context, const std::uint64_t pixelBudget, const bool explicitBypass,
    const bool planBypass, const CancellationToken& cancellation,
    GpuSceneMediaStatistics& statistics) {
    MediaUploadOutcome outcome;
    outcome.semanticKey = uploadSemanticKey("video", selection.cacheKey, resolved);
    outcome.uploadSemanticKey = outcome.semanticKey;
    ++statistics.uploadKeyConstructions;
    if (!explicitBypass && context.preparedUploadCache != nullptr) {
        if (auto cached = context.preparedUploadCache->find(outcome.uploadSemanticKey)) {
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
        context.preparedUploadCache->store(outcome.uploadSemanticKey, outcome.image);
    }
    return outcome;
}

// Whether the resolved input colour space is (exactly) the working colour space, so the CPU
// processor is identity and there is no colour pass to move to the GPU.
[[nodiscard]] bool colourTransformIsIdentity(const std::string& fromId, const std::string_view toId,
                                             const bool noConversion) {
    return noConversion || fromId.empty() || fromId == toId;
}

// Prepare the real input->working OCIO ProcessEffect for a non-identity media transform. Returns
// nullopt on success; on failure fills `outcome` (cancelled or a fail-closed diagnostic) and
// returns false.
[[nodiscard]] bool prepareMediaOcioProgram(const color::ResolvedBloomNeutralConfig& config,
                                           const std::string& fromId, const std::string_view toId,
                                           const render::ImageWindow outputWindow,
                                           const core::PixelAspectRatio pixelAspect,
                                           const GpuSceneOcioContext& ocioContext,
                                           const CancellationToken& cancellation,
                                           MediaUploadOutcome& outcome,
                                           GpuSceneMediaStatistics& statistics) {
    if (ocioContext.preparer == nullptr) {
        outcome.failure = "No GPU OCIO preparer is configured for media colour";
        return false;
    }
    const GpuOcioCommandGeometry geometry{
        .width = static_cast<std::uint32_t>(outputWindow.extent().width()),
        .height = static_cast<std::uint32_t>(outputWindow.extent().height())};
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = fromId;
    spec.toId = std::string{toId};
    auto prepared = ocioContext.preparer->prepare(
        config, spec, geometry, ocioContext.compileOptions,
        [&cancellation] { return cancellation.isCancellationRequested(); });
    if (!prepared) {
        if (prepared.error == GpuOcioPreparationError::CompileCancelled ||
            cancellation.isCancellationRequested()) {
            outcome.cancelled = true;
            return false;
        }
        if (prepared.error == GpuOcioPreparationError::IdentityTransform) {
            // OCIO itself considers the pair equivalent: the raw decode already carries working
            // space, so no effect command is emitted.
            return true;
        }
        outcome.failure = "GPU media colour preparation failed: " +
                          std::string{gpuOcioPreparationErrorName(prepared.error)};
        return false;
    }
    ++statistics.ocioCommandPreparations;
    outcome.program = prepared.command;
    outcome.semanticKey = makeGpuSceneOcioEffectSemanticKey(
        outcome.uploadSemanticKey, prepared.command->identity(), outputWindow, pixelAspect);
    return true;
}

} // namespace

MediaUploadOutcome
prepareImageUpload(const CompiledImageSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                   const bool explicitBypass, const bool planBypass,
                   const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    ++statistics.imageSources;
    const auto selection =
        selectImageSource(source, request.time, plan.format().frameRate(),
                          context.assetBaseDirectory, cancellation, request.colorIntent);
    if (selection.cancelled) {
        MediaUploadOutcome outcome;
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.available) {
        MediaUploadOutcome outcome;
        outcome.failure =
            selection.warning.empty() ? "Image source is unavailable" : selection.warning;
        return outcome;
    }
    return evaluateConvertedImageUpload(selection, resolved, context, pixelBudget, explicitBypass,
                                        planBypass, cancellation, statistics);
}

MediaUploadOutcome
prepareVideoUpload(const CompiledVideoSource& source, const EvaluationRequest& request,
                   const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                   const GpuSceneMediaContext& context, const std::uint64_t pixelBudget,
                   const bool explicitBypass, const bool planBypass,
                   const CancellationToken& cancellation, GpuSceneMediaStatistics& statistics) {
    ++statistics.videoSources;
    MediaUploadOutcome outcome;
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
    return evaluateConvertedVideoUpload(selection, resolved, context, pixelBudget, explicitBypass,
                                        planBypass, cancellation, statistics);
}

MediaUploadOutcome
prepareImageColorLeaf(const CompiledImageSource& source, const EvaluationRequest& request,
                      const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                      const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                      const std::uint64_t pixelBudget, const bool explicitBypass,
                      const bool planBypass, const CancellationToken& cancellation,
                      GpuSceneMediaStatistics& statistics) {
    const auto selection =
        selectImageSource(source, request.time, plan.format().frameRate(),
                          context.assetBaseDirectory, cancellation, request.colorIntent);
    if (selection.cancelled) {
        ++statistics.imageSources;
        MediaUploadOutcome outcome;
        outcome.cancelled = true;
        return outcome;
    }
    if (!selection.available) {
        ++statistics.imageSources;
        MediaUploadOutcome outcome;
        outcome.failure =
            selection.warning.empty() ? "Image source is unavailable" : selection.warning;
        return outcome;
    }
    const std::string fromId = selection.interpretation.inputColorSpaceId;
    const std::string_view toId = selection.workingColorSpaceId;
    const bool noConversion = selection.inputProcessor == nullptr;
    if (colourTransformIsIdentity(fromId, toId, noConversion)) {
        ++statistics.imageSources;
        return evaluateConvertedImageUpload(selection, resolved, context, pixelBudget,
                                            explicitBypass, planBypass, cancellation, statistics);
    }

    ++statistics.imageSources;
    auto config = resolveInputColorConfig(request.colorIntent);
    if (!config) {
        MediaUploadOutcome outcome;
        outcome.failure = "The selected OCIO input configuration is unavailable";
        return outcome;
    }
    // A real input->working transform. The raw upload stays at the FULL source dimensions and
    // carries the source's native metadata. Under a fractional proxy the accepted PointResampleV1
    // command gathers the full-resolution upload to the proxy output window on the GPU, and the
    // OCIO transform runs over that proxy geometry. The CPU oracle applies exact nearest resampling
    // before the per-pixel colour transform; an exact nearest sample commutes with a per-pixel
    // transform, so nearest-then-OCIO is pixel-identical. No host per-pixel resampling happens.
    MediaUploadOutcome outcome;
    outcome.uploadSemanticKey = uploadSemanticKey("image-raw", selection.decodeKey, resolved);
    outcome.semanticKey = outcome.uploadSemanticKey;
    ++statistics.uploadKeyConstructions;
    if (!explicitBypass && context.preparedUploadCache != nullptr) {
        if (auto cached = context.preparedUploadCache->find(outcome.uploadSemanticKey)) {
            outcome.image = std::move(cached);
            outcome.cacheHit = true;
            ++statistics.uploadCacheHits;
        } else {
            ++statistics.uploadCacheMisses;
        }
    }
    if (outcome.image == nullptr) {
        // RAW decode: no processor, no implicit conversion, no resample. File decode stays host
        // preparation; the OCIO colour pass is the GPU command prepared below.
        auto decoded = decodeRawImageSource(selection, byteBudget(pixelBudget), cancellation);
        ++statistics.imageConversions;
        if (decoded.cancelled) {
            outcome.cancelled = true;
            return outcome;
        }
        if (!decoded.value.has_value()) {
            outcome.failure = decoded.diagnostic.empty() ? "Image source could not be decoded"
                                                         : decoded.diagnostic;
            return outcome;
        }
        // The raw upload keeps the FULL source dimensions with the composition display window/pixel
        // aspect. At unit scale this rebase is exactly the CPU's copy (no pixel resampling); at a
        // fractional proxy it still copies every source pixel unchanged and the PointResampleV1
        // command gathers the proxy on the GPU. Parsing the decode once at full resolution keeps
        // the decode/upload identity independent of the proxy.
        const auto uploadDescriptor =
            rawUploadDescriptor(**decoded.value, resolved.imageDescriptor);
        if (!uploadDescriptor.has_value()) {
            outcome.failure = "Raw image upload descriptor is invalid";
            return outcome;
        }
        auto rebased = resampleDecodedImage(**decoded.value, *uploadDescriptor, 1.0, 1.0,
                                            byteBudget(pixelBudget), cancellation);
        if (rebased.cancelled) {
            outcome.cancelled = true;
            return outcome;
        }
        if (!rebased.value.has_value()) {
            outcome.failure =
                rebased.diagnostic.empty() ? "Image raw rebase failed" : rebased.diagnostic;
            return outcome;
        }
        outcome.image = std::make_shared<const render::Rgba32fImage>(std::move(*rebased.value));
        outcome.converted = true;
        if (!explicitBypass && !planBypass && context.preparedUploadCache != nullptr) {
            context.preparedUploadCache->store(outcome.uploadSemanticKey, outcome.image);
        }
    }
    if (outcome.image->descriptor() == nullptr) {
        outcome.failure = "Raw image upload has no descriptor";
        outcome.image.reset();
        return outcome;
    }
    const auto descriptor = *outcome.image->descriptor();
    if (!resolvedProxyIsUnit(resolved)) {
        const auto proxyDescriptor = proxyOutputDescriptor(descriptor, resolved);
        if (!proxyDescriptor.has_value()) {
            outcome.failure = "Image proxy output descriptor is invalid";
            outcome.image.reset();
            return outcome;
        }
        outcome.resample =
            MediaResamplePlan{*proxyDescriptor, resolved.horizontalScale, resolved.verticalScale};
    }
    const auto ocioWindow = outcome.resample.has_value() ? outcome.resample->output.dataWindow()
                                                         : descriptor.dataWindow();
    const auto ocioAspect = outcome.resample.has_value() ? outcome.resample->output.pixelAspect()
                                                         : descriptor.pixelAspect();
    if (!prepareMediaOcioProgram(*config, fromId, toId, ocioWindow, ocioAspect, ocioContext,
                                 cancellation, outcome, statistics)) {
        if (!outcome.cancelled) {
            outcome.image.reset();
        }
        return outcome;
    }
    return outcome;
}

MediaUploadOutcome
prepareVideoColorLeaf(const CompiledVideoSource& source, const EvaluationRequest& request,
                      const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                      const GpuSceneMediaContext& context, const GpuSceneOcioContext& ocioContext,
                      const std::uint64_t pixelBudget, const bool explicitBypass,
                      const bool planBypass, const CancellationToken& cancellation,
                      GpuSceneMediaStatistics& statistics) {
    ++statistics.videoSources;
    MediaUploadOutcome outcome;
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
    const std::string& fromId = selection.inputColorSpaceId;
    const std::string_view toId = selection.workingColorSpaceId;
    const bool noConversion = selection.inputProcessor == nullptr;
    if (colourTransformIsIdentity(fromId, toId, noConversion)) {
        // The YUV->RGB transfer/primaries conversion plus an identity processor is exactly the
        // connected path; no OCIO colour pass is hidden in host code.
        return evaluateConvertedVideoUpload(selection, resolved, context, pixelBudget,
                                            explicitBypass, planBypass, cancellation, statistics);
    }
    // A real input->working transform. The host does only the codec-side YUV matrix/range and the
    // config-managed transfer-8 path (no curve) into the source's pre-OCIO state; the OCIO pass is
    // the GPU command prepared below. The raw frame stays at source resolution; under a fractional
    // proxy the PointResampleV1 command gathers it on the GPU before the OCIO transform.
    auto config = resolveInputColorConfig(request.colorIntent);
    if (!config) {
        outcome.failure = "The selected OCIO input configuration is unavailable";
        return outcome;
    }
    outcome.uploadSemanticKey = uploadSemanticKey("video-raw", selection.decodeKey, resolved);
    outcome.semanticKey = outcome.uploadSemanticKey;
    ++statistics.uploadKeyConstructions;
    if (!explicitBypass && context.preparedUploadCache != nullptr) {
        if (auto cached = context.preparedUploadCache->find(outcome.uploadSemanticKey)) {
            outcome.image = std::move(cached);
            outcome.cacheHit = true;
            ++statistics.uploadCacheHits;
        } else {
            ++statistics.uploadCacheMisses;
        }
    }
    if (outcome.image == nullptr) {
        // Codec-side preparation only: no OCIO processor runs here. The output is premultiplied
        // pre-OCIO RGB at the full frame resolution, exactly the CPU's pre-OCIO state.
        auto prepared = media::video::videoToInputColorSpace(
            *selection.frame, selection.interpretation, selection.inputColorSpaceId,
            resolved.imageDescriptor, 1.0, 1.0, byteBudget(pixelBudget),
            [&cancellation] { return cancellation.isCancellationRequested(); });
        ++statistics.videoConversions;
        if (const auto* error = std::get_if<media::provider::Unavailable>(&prepared)) {
            outcome.cancelled = error->reason == media::provider::Error::Cancelled;
            if (!outcome.cancelled) {
                outcome.failure =
                    error->detail.empty() ? "Video frame could not be prepared" : error->detail;
            }
            return outcome;
        }
        auto& pixels = std::get<render::Rgba32fImage>(prepared);
        outcome.image = std::make_shared<const render::Rgba32fImage>(std::move(pixels));
        outcome.converted = true;
        if (!explicitBypass && !planBypass && context.preparedUploadCache != nullptr) {
            context.preparedUploadCache->store(outcome.uploadSemanticKey, outcome.image);
        }
    }
    if (outcome.image->descriptor() == nullptr) {
        outcome.failure = "Raw video upload has no descriptor";
        outcome.image.reset();
        return outcome;
    }
    const auto descriptor = *outcome.image->descriptor();
    if (!resolvedProxyIsUnit(resolved)) {
        const auto proxyDescriptor = proxyOutputDescriptor(descriptor, resolved);
        if (!proxyDescriptor.has_value()) {
            outcome.failure = "Video proxy output descriptor is invalid";
            outcome.image.reset();
            return outcome;
        }
        outcome.resample =
            MediaResamplePlan{*proxyDescriptor, resolved.horizontalScale, resolved.verticalScale};
    }
    const auto ocioWindow = outcome.resample.has_value() ? outcome.resample->output.dataWindow()
                                                         : descriptor.dataWindow();
    const auto ocioAspect = outcome.resample.has_value() ? outcome.resample->output.pixelAspect()
                                                         : descriptor.pixelAspect();
    if (!prepareMediaOcioProgram(*config, fromId, toId, ocioWindow, ocioAspect, ocioContext,
                                 cancellation, outcome, statistics)) {
        if (!outcome.cancelled) {
            outcome.image.reset();
        }
        return outcome;
    }
    return outcome;
}

} // namespace bloom::runtime::detail
