#include "image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "input_color_context.hpp"
#include "operation_key.hpp"

#include <array>
#include <cmath>

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
                                                const EvaluationColorIntent& intent) {
    if (effect.bypass || std::holds_alternative<IdentityImageKernel>(effect.kernel))
        return {};
    const auto& cst = std::get<CstKernel>(effect.kernel);
    const std::string from(cst.fromId.empty() ? intent.workingColorSpaceId : cst.fromId);
    const std::string to(cst.toId.empty() ? intent.workingColorSpaceId : cst.toId);
    OperationKey key;
    key.add(std::string(intent.ocioConfigUri));
    const auto revision = intent.ocioConfigRevision.toLowercaseHex();
    key.add(std::string(revision.data(), revision.size()));
    key.add(std::string(intent.workingColorSpaceId));
    key.add(from);
    key.add(to);
    std::lock_guard lock(mutex_);
    if (const auto found = processors_.find(key.bytes()); found != processors_.end())
        return found->second;
    PreparedImageEffect prepared;
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
    if (processors_.size() >= 256)
        processors_.clear();
    processors_.emplace(key.bytes(), prepared);
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
        for (std::size_t x = 0; x < source.value()->size(); ++x) {
            if (x % 256 == 0 && cancellation.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            const auto pixel = (*source.value())[x];
            const auto alpha = pixel.alpha();
            if (alpha == 0) {
                (*target.value())[x] = pixel;
                continue;
            }
            std::array<std::array<float, 4>, 1> straight{
                {{pixel.red() / alpha, pixel.green() / alpha, pixel.blue() / alpha, alpha}}};
            if (!effect.processor->apply(straight)) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::ColorTransformFailed,
                                            "OCIO could not produce finite straight RGB");
                return result;
            }
            const auto value = render::Rgba32f::fromPremultiplied(
                straight[0][0] * alpha, straight[0][1] * alpha, straight[0][2] * alpha, alpha);
            if (!value) {
                result.diagnostic = refusal(EvaluationDiagnosticCode::InvalidPixel,
                                            "OCIO produced invalid premultiplied RGB");
                return result;
            }
            (*target.value())[x] = *value.value();
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
