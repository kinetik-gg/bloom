#include "image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"

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
} // namespace

PreparedImageEffect ImageEffectContext::prepare(const CompiledImageEffect& effect,
                                                const EvaluationColorIntent& intent,
                                                const std::filesystem::path& base,
                                                const CancellationToken& cancellation) {
    if (effect.bypass || std::holds_alternative<IdentityImageKernel>(effect.kernel))
        return {};
    PreparedImageEffect prepared;
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
                                   const EvaluationProgressCallback& progress) {
    ImageEffectResult result;
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
