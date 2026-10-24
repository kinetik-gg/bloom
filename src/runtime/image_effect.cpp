#include "image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "discrete_image_effect.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"
#include "resize_fit_geometry.hpp"

#include <algorithm>
#include <array>
#include <bloom/media/image.hpp>
#include <chrono>
#include <cmath>
#include <thread>

namespace bloom::runtime {
std::shared_ptr<detail::ImageEffectContext> CpuCompositionEvaluator::imageEffectContext() const {
    std::lock_guard lock(assetContext_->mutex);
    if (!assetContext_->effects)
        assetContext_->effects = std::make_shared<detail::ImageEffectContext>();
    return assetContext_->effects;
}
} // namespace bloom::runtime

namespace bloom::runtime::detail {
namespace {
[[nodiscard]] EvaluationDiagnostic refusal(const EvaluationDiagnosticCode code,
                                           std::string reason) {
    return {code,
            DiagnosticSeverity::Warning,
            {},
            "Colour transform bypassed: " + reason,
            std::move(reason)};
}
[[nodiscard]] EvaluationDiagnostic
processorFailure(const color::OcioColorSpaceProcessorError error) {
    using Error = color::OcioColorSpaceProcessorError;
    switch (error) {
    case Error::MissingInputColorSpace:
        return refusal(EvaluationDiagnosticCode::ColorSpaceMissing,
                       "MissingInputColorSpace: the source id is absent from the selected config");
    case Error::MissingWorkingColorSpace:
        return refusal(
            EvaluationDiagnosticCode::ColorSpaceMissing,
            "MissingWorkingColorSpace: the destination id is absent from the selected config");
    case Error::InputColorSpaceIsData:
    case Error::WorkingColorSpaceInvalid:
        return refusal(EvaluationDiagnosticCode::ColorSpaceIsData,
                       "Data colour spaces cannot be used for image colour transforms");
    case Error::InvalidColorSpaceId:
        return refusal(EvaluationDiagnosticCode::ColorSpaceMissing, "InvalidColorSpaceId");
    case Error::UnsupportedFloatingPointEnvironment:
        return refusal(EvaluationDiagnosticCode::UnsupportedFloatingPointEnvironment,
                       "UnsupportedFloatingPointEnvironment");
    case Error::CpuProcessorUnavailable:
        return refusal(EvaluationDiagnosticCode::ColorTransformFailed, "CpuProcessorUnavailable");
    case Error::TransformBuildFailed:
    case Error::None:
        return refusal(EvaluationDiagnosticCode::ColorTransformFailed, "TransformBuildFailed");
    }
    return refusal(EvaluationDiagnosticCode::ColorTransformFailed, "TransformBuildFailed");
}
[[nodiscard]] EvaluationDiagnostic spatialFailure(const EvaluationDiagnosticCode code,
                                                  std::string reason) {
    return {code, DiagnosticSeverity::Error, {}, "Resize/Fit: " + reason, std::move(reason)};
}
} // namespace

PreparedImageEffect ImageEffectContext::prepare(const CompiledImageEffect& effect,
                                                const EvaluationColorIntent& intent,
                                                const std::filesystem::path& base,
                                                const CancellationToken& cancellation,
                                                const std::optional<double> sampledAmount,
                                                const std::optional<std::array<double, 4>> canvasMargins,
                                                const double horizontalScale,
                                                const double verticalScale,
                                                const std::optional<std::int64_t> sampledChannel,
                                                std::shared_ptr<const PreparedLayerMasks> layerMasks) {
    if (effect.bypass || std::holds_alternative<IdentityImageKernel>(effect.kernel))
        return {};
    PreparedImageEffect prepared;
    if (std::holds_alternative<LayerMaskKernel>(effect.kernel)) {
        if (!layerMasks) {
            prepared.diagnostic = refusal(EvaluationDiagnosticCode::InvalidParameter,
                                          "Layer mask parameters are not evaluable");
            return prepared;
        }
        prepared.cacheIdentity = layerMasks->identity;
        prepared.masks = std::move(layerMasks);
        return prepared;
    }
    if (const auto* canvas = std::get_if<CanvasImageKernel>(&effect.kernel)) {
        if (!canvasMargins) {
            prepared.diagnostic = refusal(EvaluationDiagnosticCode::InvalidParameter,
                                          "Canvas margins are not evaluable");
            return prepared;
        }
        prepared.canvas = CanvasMargins{canvas->effect, *canvasMargins};
        OperationKey key;
        key.add(canvas->effect);
        for (const auto margin : *canvasMargins)
            key.add(margin);
        prepared.cacheIdentity = key.bytes();
        return prepared;
    }
    if (const auto* combine = std::get_if<ImageCombineKernel>(&effect.kernel)) {
        // A driven selector samples the whole closed stored enumeration (channel and invert); a
        // constant one was split by the compiler into the kernel's channel and invert fields.
        const auto selection =
            combine->drivenChannel
                ? (sampledChannel ? render::matteSelectionFromStoredValue(*sampledChannel)
                                  : std::nullopt)
                : (static_cast<std::uint8_t>(combine->channel) <=
                           static_cast<std::uint8_t>(render::ImageCoverageChannel::Luma)
                       ? std::optional{render::MatteSelection{combine->channel, combine->invert}}
                       : std::nullopt);
        if (!effect.secondaryInput ||
            static_cast<std::uint8_t>(combine->mode) >
                static_cast<std::uint8_t>(ImageCombineMode::MultiplyAlpha) ||
            !selection ||
            (combine->mode == ImageCombineMode::SetAlpha) != !combine->factor ||
            (combine->factor && (!sampledAmount || !std::isfinite(*sampledAmount) ||
                                 *sampledAmount < 0.0 || *sampledAmount > 1.0))) {
            prepared.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                          "Image combine parameters are invalid");
            return prepared;
        }
        const float factor = combine->factor ? static_cast<float>(*sampledAmount) : 1.0F;
        prepared.combine = PreparedImageEffect::Combine{combine->mode, factor, *selection};
        OperationKey combineKey;
        combineKey.add(combine->mode);
        combineKey.add(static_cast<double>(factor));
        combineKey.add(selection->channel);
        // Appended only when set, so every pre-#361 combine key keeps its exact bytes.
        if (selection->invert)
            combineKey.add(std::string("inverted-matte"));
        prepared.cacheIdentity = combineKey.bytes();
        return prepared;
    }
    if (const auto* discrete = std::get_if<DiscreteImageKernel>(&effect.kernel)) {
        prepared.discrete = *discrete;
        OperationKey key;
        key.add(std::string("discrete-image-v1"));
        key.add(discrete->horizontal);
        key.add(discrete->vertical);
        key.add(static_cast<std::uint64_t>(discrete->quarterTurns));
        prepared.cacheIdentity = key.bytes();
        return prepared;
    }
    if (const auto* pointwise = std::get_if<PointwiseImageKernel>(&effect.kernel)) {
        prepared.pointwise = pointwiseAffine(*pointwise, sampledAmount);
        if (!prepared.pointwise) {
            prepared.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                          "Pointwise effect parameter cannot be represented");
            return prepared;
        }
        OperationKey pointwiseKey;
        pointwiseKey.add(pointwise->effect);
        pointwiseKey.add(static_cast<double>(prepared.pointwise->scale));
        pointwiseKey.add(static_cast<double>(prepared.pointwise->offset));
        prepared.cacheIdentity = pointwiseKey.bytes();
        return prepared;
    }
    if (const auto* kernel = std::get_if<ResizeFitKernel>(&effect.kernel)) {
        prepared.resizeFit = *kernel;
        return prepared;
    }
    if (const auto* blur = std::get_if<SeparableBlurKernel>(&effect.kernel)) {
        prepared.blur =
            prepareSeparableBlur(*blur, sampledAmount, horizontalScale, verticalScale);
        if (!prepared.blur) {
            prepared.diagnostic = refusal(EvaluationDiagnosticCode::InvalidParameter,
                                          "Blur radius or scale is out of domain");
            return prepared;
        }
        OperationKey blurKey;
        blurKey.add(blur->kind);
        blurKey.add(blur->edge);
        for (const auto weight : prepared.blur->horizontal)
            blurKey.add(double(weight));
        blurKey.add(std::string("vertical"));
        for (const auto weight : prepared.blur->vertical)
            blurKey.add(double(weight));
        prepared.cacheIdentity = blurKey.bytes();
        return prepared;
    }
    OperationKey key;
    key.add(std::string(intent.ocioConfigUri));
    const auto revision = intent.ocioConfigRevision.toLowercaseHex();
    key.add(std::string(revision.data(), revision.size()));
    key.add(std::string(intent.workingColorSpaceId));
    const auto* cst = std::get_if<CstKernel>(&effect.kernel);
    const auto* file = std::get_if<FileTransformKernel>(&effect.kernel);
    const std::string from(cst && !cst->fromId.empty() ? cst->fromId : intent.workingColorSpaceId);
    const std::string to(
        cst ? (cst->toId.empty() ? intent.workingColorSpaceId : cst->toId)
            : (file->processSpaceId.empty() ? intent.workingColorSpaceId : file->processSpaceId));
    key.add(from);
    key.add(to);
    color::LutFile resource;
    const auto cancelled = [&cancellation] { return cancellation.isCancellationRequested(); };
    if (file) {
        if (!file->asset || file->asset->kind != document::AssetKind::Lut ||
            file->asset->id != file->lutAssetId) {
            prepared.diagnostic = refusal(EvaluationDiagnosticCode::LutTransformFailed,
                                          "MissingFile: select a LUT asset");
            prepared.cacheIdentity = "missing-lut";
            return prepared;
        }
        resource =
            color::readLutFile(media::resolveImagePath(file->asset->locator.path,
                                                       file->asset->locator.relinkHint, base),
                               cancelled);
        const auto digest = resource.digest.toLowercaseHex();
        key.add(std::string(digest.data(), digest.size()));
        key.add(resource.format);
        key.add(file->interpolation);
        key.add(file->direction);
        if (resource.error == color::LutError::None &&
            resource.digest != file->asset->contentDigest)
            resource.error = color::LutError::ChangedFile;
        prepared.cacheIdentity = key.bytes() + std::string(color::lutErrorName(resource.error));
        if (resource.error != color::LutError::None) {
            prepared.cancelled = resource.error == color::LutError::HelperCancelled;
            if (!prepared.cancelled)
                prepared.diagnostic = refusal(EvaluationDiagnosticCode::LutTransformFailed,
                                              std::string(color::lutErrorName(resource.error)));
            return prepared;
        }
    }
    // Never hold the cache mutex during configuration preparation, process startup or I/O.
    {
        std::lock_guard lock(mutex_);
        if (const auto found = processors_.find(key.bytes());
            found != processors_.end() &&
            (!found->second.fileProcessor || found->second.fileProcessor->isAvailable()))
            return found->second;
    }
    auto config = resolveInputColorConfig(intent);
    if (!config) {
        prepared.diagnostic =
            refusal(EvaluationDiagnosticCode::OcioConfigUnavailable,
                    "The exact OCIO config revision or working space is unavailable");
    } else {
        auto result = color::CpuColorSpaceProcessor::prepare(*config, from, to);
        if (result)
            prepared.processor = std::move(result).takeProcessor();
        else
            prepared.diagnostic = processorFailure(result.error());
    }
    if (file && !prepared.diagnostic) {
        auto after = color::CpuColorSpaceProcessor::prepare(*config, to, from);
        if (!after) {
            prepared.diagnostic = processorFailure(after.error());
        } else {
            prepared.afterProcessor = std::move(after).takeProcessor();
            auto lut = color::CpuFileTransformProcessor::prepare(
                resource, static_cast<color::LutInterpolation>(file->interpolation),
                static_cast<color::LutDirection>(file->direction), cancelled);
            prepared.fileProcessor = std::move(lut.processor);
            prepared.cancelled = lut.error == color::LutError::HelperCancelled;
            if (lut.error != color::LutError::None && !prepared.cancelled)
                prepared.diagnostic = refusal(EvaluationDiagnosticCode::LutTransformFailed,
                                              std::string(color::lutErrorName(lut.error)));
            // A proven identity LUT is an exact identity even across a nonlinear process space.
            if (prepared.fileProcessor && prepared.fileProcessor->isIdentity()) {
                prepared.processor.reset();
                prepared.afterProcessor.reset();
            }
        }
    }
    if (prepared.diagnostic)
        prepared.cacheIdentity += ":prepare-refused:" + prepared.diagnostic->detail;
    if (!prepared.cancelled && !prepared.diagnostic) {
        std::lock_guard lock(mutex_);
        if (processors_.size() >= (file ? 4U : 256U))
            processors_.clear();
        processors_.insert_or_assign(key.bytes(), prepared);
    }
    return prepared;
}

ImageEffectResult applyImageEffect(const PreparedImageEffect& effect,
                                   const render::Rgba32fImage& input,
                                   const render::Rgba32fImage* secondary,
                                   const std::size_t pixelBudget,
                                   const CancellationToken& cancellation,
                                   const OperationIndex operation,
                                   const EvaluationProgressCallback& progress,
                                   const double horizontalScale, const double verticalScale,
                                   const std::optional<const render::Rgba32fImage*> mask) {
    if (effect.discrete)
        return applyDiscreteImageEffect(*effect.discrete, input, pixelBudget, cancellation,
                                        operation, progress, horizontalScale, verticalScale);
    if (effect.blur)
        return applySeparableBlur(*effect.blur, input, pixelBudget, cancellation, operation,
                                  progress);
    ImageEffectResult result;
    if (effect.canvas) {
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            return result;
        }
        const auto source = input.descriptor()->dataWindow();
        const auto window = canvasOutputWindow(source, *effect.canvas, horizontalScale,
                                               verticalScale);
        const auto display = canvasOutputWindow(input.descriptor()->displayWindow(),
                                                *effect.canvas, horizontalScale, verticalScale);
        if (!window || !display) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidParameter,
                                        "Canvas margins leave an invalid data window");
            return result;
        }
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *window, *display, input.descriptor()->pixelAspect());
        if (!descriptor) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidParameter,
                                        "Canvas output descriptor is invalid");
            return result;
        }
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), pixelBudget);
        if (!builder) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                                        "Canvas output exceeds the pixel storage budget");
            return result;
        }
        const auto view = input.view();
        if (!view) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                        "Canvas input is invalid");
            return result;
        }
        const auto transparent = render::Rgba32f::transparent();
        reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0,
                                  window->extent().height()});
        for (auto y = window->originY(); y < window->maxYExclusive(); ++y) {
            if (cancellation.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            auto target = builder.value()->row(y);
            if (!target) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::InternalInvariant,
                                            "Canvas output row is unavailable");
                return result;
            }
            for (std::size_t i = 0; i < target.value()->size(); ++i) {
                if ((i & 4095U) == 0 && cancellation.isCancellationRequested()) {
                    result.cancelled = true;
                    return result;
                }
                const auto x = window->originX() + static_cast<std::int64_t>(i);
                if (source.contains(x, y)) {
                    const auto pixel = view.value()->read(x, y);
                    if (!pixel) {
                        result.diagnostic = refusal(EvaluationDiagnosticCode::InternalInvariant,
                                                    "Canvas input pixel is unavailable");
                        return result;
                    }
                    (*target.value())[i] = *pixel.value();
                } else {
                    (*target.value())[i] = transparent;
                }
            }
            reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                      static_cast<std::uint64_t>(y - window->originY() + 1),
                                      window->extent().height()});
        }
        auto frozen = std::move(*builder.value()).freeze();
        if (!frozen) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                        "Canvas output validation failed");
            return result;
        }
        result.image = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
        return result;
    }
    if (effect.resizeFit) {
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            return result;
        }
        const auto geometry = resolveResizeFitGeometry(*effect.resizeFit, *input.descriptor(),
                                                       horizontalScale, verticalScale);
        if (!geometry) {
            result.diagnostic = spatialFailure(EvaluationDiagnosticCode::InvalidParameter,
                                               "Resize/Fit output geometry is not representable");
            return result;
        }
        auto target = render::Rgba32fImageBuilder::create(geometry->descriptor, pixelBudget);
        if (!target) {
            result.diagnostic =
                spatialFailure(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                               "Resize/Fit output exceeds the pixel storage budget");
            return result;
        }
        const auto source = input.view();
        if (!source) {
            result.diagnostic = spatialFailure(EvaluationDiagnosticCode::InvalidPixel,
                                               "Resize/Fit input image is invalid");
            return result;
        }
        const auto window = geometry->descriptor.dataWindow();
        const ParentedLayerTransform transform{geometry->matrix, input.descriptor()->dataWindow(),
                                               1.0, 1.0, 1.0F};
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
            if (cancellation.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            const auto row = target.value()->row(y);
            if (!row || transform.row(*source.value(), window, y, *row.value())) {
                result.diagnostic = spatialFailure(EvaluationDiagnosticCode::InvalidPixel,
                                                   "Resize/Fit produced an invalid row");
                return result;
            }
            reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                      static_cast<std::uint64_t>(y + 1), window.extent().height()});
        }
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            return result;
        }
        auto frozen = std::move(*target.value()).freeze();
        if (!frozen) {
            result.diagnostic = spatialFailure(EvaluationDiagnosticCode::InvalidPixel,
                                               "Resize/Fit output validation failed");
            return result;
        }
        result.image = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
        return result;
    }
    auto builder = render::Rgba32fImageBuilder::create(*input.descriptor(), pixelBudget);
    if (!builder) {
        result.diagnostic = refusal(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                                    "The colour transform image exceeds the pixel storage budget");
        return result;
    }
    const auto view = input.view();
    if (!view) {
        result.diagnostic =
            refusal(EvaluationDiagnosticCode::InvalidPixel, "Colour transform input is invalid");
        return result;
    }
    const auto window = input.descriptor()->dataWindow();
    if (effect.masks) {
        // ADR 0028: the input times the accumulated mask coverage at each pixel centre, in the
        // input's own (content) pixel space. The data window is unchanged.
        const auto total = window.extent().height();
        reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
            if (cancellation.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            const auto sourceRow = view.value()->row(y);
            auto targetRow = builder.value()->row(y);
            if (!sourceRow || !targetRow) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::InternalInvariant,
                                            "Layer mask row is unavailable");
                return result;
            }
            const float centreY = static_cast<float>(y) + 0.5F;
            for (std::size_t column = 0; column < sourceRow.value()->size(); ++column) {
                const auto x = window.originX() + static_cast<std::int64_t>(column);
                const float coverage =
                    layerMaskCoverage(*effect.masks, static_cast<float>(x) + 0.5F, centreY);
                const auto base = (*sourceRow.value())[column];
                const auto pixel = render::Rgba32f::fromPremultiplied(
                    base.red() * coverage, base.green() * coverage, base.blue() * coverage,
                    base.alpha() * coverage);
                if (!pixel) {
                    result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                                "Layer mask produced an invalid pixel");
                    return result;
                }
                (*targetRow.value())[column] = *pixel.value();
            }
            reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                      static_cast<std::uint64_t>(y - window.originY() + 1), total});
        }
        auto frozen = std::move(*builder.value()).freeze();
        if (!frozen) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                        "Layer mask image validation failed");
            return result;
        }
        result.image = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
        return result;
    }
    if (effect.combine) {
        // #361: an absent second image (a matte Layer outside its range, an unconnected or empty
        // matte) is a transparent matte: every tap is transparent black, exactly as a tap outside
        // a present matte's data window. The resident GPU route declares an empty source extent
        // for the same result.
        if (secondary != nullptr && secondary->descriptor() == nullptr) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                        "Image combine second input is unavailable");
            return result;
        }
        std::optional<render::Rgba32fImageView> secondView;
        std::optional<render::ImageWindow> secondWindow;
        if (secondary != nullptr) {
            auto matteView = secondary->view();
            if (!matteView) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                            "Image combine second input is invalid");
                return result;
            }
            secondView.emplace(*matteView.value());
            secondWindow = secondary->descriptor()->dataWindow();
        }
        // ADR 0028: Image Mix's optional Mask, read from its alpha; outside its data window the
        // mask is zero.
        std::optional<render::Rgba32fImageView> maskView;
        std::optional<render::ImageWindow> maskWindow;
        if (mask && *mask != nullptr) {
            if ((*mask)->descriptor() == nullptr) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                            "Image Mix mask is unavailable");
                return result;
            }
            auto fetched = (*mask)->view();
            if (!fetched) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                            "Image Mix mask is invalid");
                return result;
            }
            maskView.emplace(*fetched.value());
            maskWindow = (*mask)->descriptor()->dataWindow();
        }
        const auto total = window.extent().height();
        reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
            if (cancellation.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            const auto sourceRow = view.value()->row(y);
            auto targetRow = builder.value()->row(y);
            if (!sourceRow || !targetRow) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::InternalInvariant,
                                            "Image combine base row is unavailable");
                return result;
            }
            std::optional<std::span<const render::Rgba32f>> secondRow;
            if (secondWindow && y >= secondWindow->originY() &&
                y < secondWindow->maxYExclusive()) {
                const auto fetched = secondView->row(y);
                if (!fetched) {
                    result.diagnostic = refusal(EvaluationDiagnosticCode::InternalInvariant,
                                                "Image combine second row is unavailable");
                    return result;
                }
                secondRow = *fetched.value();
            }
            std::optional<std::span<const render::Rgba32f>> maskRow;
            if (maskWindow && y >= maskWindow->originY() && y < maskWindow->maxYExclusive()) {
                const auto fetched = maskView->row(y);
                if (!fetched) {
                    result.diagnostic = refusal(EvaluationDiagnosticCode::InternalInvariant,
                                                "Image Mix mask row is unavailable");
                    return result;
                }
                maskRow = *fetched.value();
            }
            for (std::size_t column = 0; column < sourceRow.value()->size(); ++column) {
                if ((column & 4095U) == 0 && cancellation.isCancellationRequested()) {
                    result.cancelled = true;
                    return result;
                }
                const auto x = window.originX() + static_cast<std::int64_t>(column);
                const auto base = (*sourceRow.value())[column];
                const auto matte =
                    secondRow && x >= secondWindow->originX() && x < secondWindow->maxXExclusive()
                        ? (*secondRow)[static_cast<std::size_t>(x - secondWindow->originX())]
                        : render::Rgba32f::transparent();
                render::ImageResult<render::Rgba32f> pixel =
                    render::ImageResult<render::Rgba32f>::success(base);
                if (effect.combine->mode == ImageCombineMode::SetAlpha) {
                    pixel = render::setImageAlpha(
                        base, render::matteCoverage(matte, effect.combine->matte));
                } else {
                    // ADR 0028: the masked factor is one Float32 product, as on the GPU.
                    const float maskCoverage =
                        !mask ? 1.0F
                        : maskRow && x >= maskWindow->originX() && x < maskWindow->maxXExclusive()
                            ? (*maskRow)[static_cast<std::size_t>(x - maskWindow->originX())]
                                  .alpha()
                            : 0.0F;
                    const double weight = effect.combine->mode == ImageCombineMode::Mix
                                              ? static_cast<double>(
                                                    !mask ? effect.combine->factor
                                                          : effect.combine->factor * maskCoverage)
                                              : static_cast<double>(render::matteCoverage(
                                                    matte, effect.combine->matte)) *
                                                    static_cast<double>(effect.combine->factor);
                    const double baseWeight = effect.combine->mode == ImageCombineMode::Mix
                                                  ? 1.0 - weight : weight;
                    const double matteWeight = effect.combine->mode == ImageCombineMode::Mix
                                                   ? weight : 0.0;
                    pixel = render::Rgba32f::fromPremultiplied(
                        static_cast<float>(baseWeight * static_cast<double>(base.red()) +
                                           matteWeight * static_cast<double>(matte.red())),
                        static_cast<float>(baseWeight * static_cast<double>(base.green()) +
                                           matteWeight * static_cast<double>(matte.green())),
                        static_cast<float>(baseWeight * static_cast<double>(base.blue()) +
                                           matteWeight * static_cast<double>(matte.blue())),
                        static_cast<float>(baseWeight * static_cast<double>(base.alpha()) +
                                           matteWeight * static_cast<double>(matte.alpha())));
                }
                if (!pixel) {
                    result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                                "Image combine produced invalid premultiplied RGB");
                    return result;
                }
                (*targetRow.value())[column] = *pixel.value();
            }
        }
        auto frozen = std::move(*builder.value()).freeze();
        if (!frozen) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                        "Image combine image validation failed");
            return result;
        }
        result.image = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
        reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
        return result;
    }
    const auto total = window.extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            return result;
        }
        const auto source = view.value()->row(y);
        auto target = builder.value()->row(y);
        if (!source || !target) {
            result.diagnostic = refusal(EvaluationDiagnosticCode::InternalInvariant,
                                        "Colour transform row is unavailable");
            return result;
        }
        constexpr std::size_t chunkSize = 4096;
        std::array<std::array<float, 4>, chunkSize> buffer{};
        for (std::size_t x = 0; x < source.value()->size(); x += chunkSize) {
            if (cancellation.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            const auto count = std::min(chunkSize, source.value()->size() - x);
            if (effect.pointwise) {
                // Affine RGB commutes with association. Avoid overflowing an otherwise valid
                // premultiplied pixel merely to recover an unrepresentable straight value.
                // Keep the binary32 multiply and add separate, matching the GPU matrix path.
                for (std::size_t i = 0; i < count; ++i) {
                    const auto pixel = (*source.value())[x + i];
                    const auto alpha = pixel.alpha();
                    if (alpha == 0.0F) {
                        (*target.value())[x + i] = pixel;
                        continue;
                    }
                    const float offset = alpha * effect.pointwise->offset;
                    const auto value = render::Rgba32f::fromPremultiplied(
                        pixel.red() * effect.pointwise->scale + offset,
                        pixel.green() * effect.pointwise->scale + offset,
                        pixel.blue() * effect.pointwise->scale + offset, alpha);
                    if (!value) {
                        result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                                    "Pointwise effect produced invalid RGB");
                        return result;
                    }
                    (*target.value())[x + i] = *value.value();
                }
                continue;
            }
            auto straight = std::span(buffer).first(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto pixel = (*source.value())[x + i];
                const auto a = pixel.alpha();
                straight[i] = a == 0 ? std::array<float, 4>{}
                                     : std::array<float, 4>{pixel.red() / a, pixel.green() / a,
                                                            pixel.blue() / a, a};
            }
            if (effect.processor && !effect.processor->apply(straight)) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::ColorTransformFailed,
                                            "OCIO could not produce finite straight RGB");
                return result;
            }
            if (effect.fileProcessor) {
                const auto error = effect.fileProcessor->apply(
                    straight, [&cancellation] { return cancellation.isCancellationRequested(); });
                if (error != color::LutError::None) {
                    result.cancelled = error == color::LutError::HelperCancelled;
                    if (!result.cancelled)
                        result.diagnostic = refusal(EvaluationDiagnosticCode::LutTransformFailed,
                                                    std::string(color::lutErrorName(error)));
                    return result;
                }
            }
            if (effect.afterProcessor && !effect.afterProcessor->apply(straight)) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::ColorTransformFailed,
                                            "OCIO could not return finite working RGB");
                return result;
            }
            for (std::size_t i = 0; i < count; ++i) {
                const auto alpha = (*source.value())[x + i].alpha();
                if (alpha == 0) {
                    (*target.value())[x + i] = (*source.value())[x + i];
                    continue;
                }
                const auto value = render::Rgba32f::fromPremultiplied(
                    straight[i][0] * alpha, straight[i][1] * alpha, straight[i][2] * alpha, alpha);
                if (!value) {
                    result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                                "OCIO produced invalid premultiplied RGB");
                    return result;
                }
                (*target.value())[x + i] = *value.value();
            }
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                    "Colour transform image validation failed");
        return result;
    }
    result.image = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    return result;
}
} // namespace bloom::runtime::detail
