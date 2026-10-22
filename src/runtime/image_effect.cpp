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
                                                const std::optional<double> pointwiseAmount,
                                                const double horizontalScale,
                                                const double verticalScale) {
    if (effect.bypass || std::holds_alternative<IdentityImageKernel>(effect.kernel))
        return {};
    PreparedImageEffect prepared;
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
        prepared.pointwise = pointwiseAffine(*pointwise, pointwiseAmount);
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
            prepareSeparableBlur(*blur, pointwiseAmount, horizontalScale, verticalScale);
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
                                   const render::Rgba32fImage& input, const std::size_t pixelBudget,
                                   const CancellationToken& cancellation,
                                   const OperationIndex operation,
                                   const EvaluationProgressCallback& progress,
                                   const double horizontalScale, const double verticalScale) {
    if (effect.discrete)
        return applyDiscreteImageEffect(*effect.discrete, input, pixelBudget, cancellation,
                                        operation, progress);
    if (effect.blur)
        return applySeparableBlur(*effect.blur, input, pixelBudget, cancellation, operation,
                                  progress);
    ImageEffectResult result;
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
            if (effect.pointwise) {
                for (auto& pixel : straight) {
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        pixel[channel] = pixel[channel] * effect.pointwise->scale +
                                         effect.pointwise->offset;
                    }
                }
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
