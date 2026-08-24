#include "cpu_composition_evaluator_support.hpp"

#include <bloom/render/cpu_image_primitives.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace detail {

[[nodiscard]] EvaluationDiagnostic diagnostic(const EvaluationDiagnosticCode code,
                                              std::string summary, std::string detail,
                                              EvaluationSubject subject) {
    return {.code = code,
            .severity = DiagnosticSeverity::Error,
            .subject = std::move(subject),
            .summary = std::move(summary),
            .detail = std::move(detail)};
}

[[nodiscard]] EvaluationSubject subjectFor(const OperationIndex index,
                                           const CompiledOperation& operation) {
    EvaluationSubject subject{
        .operation = index, .nodeId = std::nullopt, .layerId = std::nullopt, .field = {}};
    std::visit(
        Overloaded{
            [&subject](const CompiledSolid& solid) { subject.nodeId = solid.sourceNodeId; },
            [&subject](const CompiledLayerOutput& layer) {
                subject.nodeId = layer.sourceNodeId;
                subject.layerId = layer.layerId;
            },
            [&subject](const CompiledLayerStack& stack) { subject.nodeId = stack.sourceNodeId; },
            [&subject](const CompiledCompositionOutput& output) {
                subject.nodeId = output.sourceNodeId;
            },
        },
        operation);
    return subject;
}

void reportProgress(const EvaluationProgressCallback& callback,
                    const EvaluationProgress& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring is best effort. A presentation-side allocation failure must not change pixels.
    }
}

[[nodiscard]] std::optional<core::PixelAspectRatio>
proxyPixelAspect(const document::CompositionFormat& format,
                 const render::ImageExtent proxyExtent) noexcept {
    std::array<std::uint64_t, 3> numerator{format.pixelAspect().numerator(), format.width(),
                                           proxyExtent.height()};
    std::array<std::uint64_t, 3> denominator{format.pixelAspect().denominator(), format.height(),
                                             proxyExtent.width()};
    for (auto& numeratorFactor : numerator) {
        for (auto& denominatorFactor : denominator) {
            const auto divisor = std::gcd(numeratorFactor, denominatorFactor);
            numeratorFactor /= divisor;
            denominatorFactor /= divisor;
        }
    }

    const auto reducedNumerator = checkedProduct(numerator);
    const auto reducedDenominator = checkedProduct(denominator);
    if (!reducedNumerator.has_value() || !reducedDenominator.has_value()) {
        return std::nullopt;
    }
    return core::PixelAspectRatio::create(*reducedNumerator, *reducedDenominator);
}

[[nodiscard]] EvaluationDiagnostic imageDiagnostic(const render::ImageError& error,
                                                   EvaluationSubject subject, std::string summary) {
    EvaluationDiagnosticCode code = EvaluationDiagnosticCode::InternalInvariant;
    switch (error.code) {
    case render::ImageErrorCode::ArithmeticOverflow:
        code = EvaluationDiagnosticCode::ArithmeticOverflow;
        break;
    case render::ImageErrorCode::PixelStorageBudgetExceeded:
        code = EvaluationDiagnosticCode::PixelStorageBudgetExceeded;
        break;
    case render::ImageErrorCode::AllocationFailure:
        code = EvaluationDiagnosticCode::AllocationFailure;
        break;
    case render::ImageErrorCode::InvalidPixel:
    case render::ImageErrorCode::NonFiniteResult:
        code = EvaluationDiagnosticCode::InvalidPixel;
        break;
    case render::ImageErrorCode::InvalidParameter:
        code = EvaluationDiagnosticCode::InvalidParameter;
        break;
    case render::ImageErrorCode::UnsupportedFloatingPointEnvironment:
        code = EvaluationDiagnosticCode::UnsupportedFloatingPointEnvironment;
        break;
    case render::ImageErrorCode::IncompatibleImageDescriptor:
        code = EvaluationDiagnosticCode::IncompatibleImageDescriptor;
        break;
    case render::ImageErrorCode::InvalidExtent:
    case render::ImageErrorCode::InvalidWindow:
    case render::ImageErrorCode::InvalidStorageSize:
    case render::ImageErrorCode::CoordinateOutOfBounds:
    case render::ImageErrorCode::InvalidState:
        code = EvaluationDiagnosticCode::InternalInvariant;
        break;
    }

    std::string detail;
    if (error.requestedPixelStorageBytes.has_value()) {
        detail += "requestedBytes=" + std::to_string(*error.requestedPixelStorageBytes);
    }
    if (error.pixelStorageByteLimit.has_value()) {
        if (!detail.empty()) {
            detail += ' ';
        }
        detail += "byteLimit=" + std::to_string(*error.pixelStorageByteLimit);
    }
    return diagnostic(code, std::move(summary), std::move(detail), std::move(subject));
}

[[nodiscard]] bool isImageProducing(const CompiledOperation& operation) noexcept {
    return !std::holds_alternative<CompiledCompositionOutput>(operation);
}

[[nodiscard]] bool hasExpectedInputKinds(const CompiledCompositionPlan& plan,
                                         const std::size_t index, EvaluationDiagnostic& failure) {
    const auto& operation = plan.operations[index];
    return std::visit(
        Overloaded{
            [](const CompiledSolid&) { return true; },
            [&plan, index, &failure](const CompiledLayerOutput& layer) {
                if (layer.input.value() >= index ||
                    !std::holds_alternative<CompiledSolid>(plan.operations[layer.input.value()])) {
                    failure = diagnostic(
                        EvaluationDiagnosticCode::InvalidPlan,
                        "Layer Output has an invalid image input",
                        "The input must name an earlier Solid operation.",
                        subjectFor(OperationIndex::fromRaw(index), plan.operations[index]));
                    return false;
                }
                return true;
            },
            [&plan, index, &failure](const CompiledLayerStack& stack) {
                for (const auto& entry : stack.entries) {
                    if (entry.input.value() >= index) {
                        failure = diagnostic(
                            EvaluationDiagnosticCode::InvalidPlan,
                            "Layer Stack has a non-topological input", {},
                            subjectFor(OperationIndex::fromRaw(index), plan.operations[index]));
                        return false;
                    }
                    const auto* layer =
                        std::get_if<CompiledLayerOutput>(&plan.operations[entry.input.value()]);
                    if (layer == nullptr || layer->layerId != entry.layerId) {
                        failure = diagnostic(
                            EvaluationDiagnosticCode::InvalidPlan,
                            "Layer Stack entry does not match its layer output", {},
                            subjectFor(OperationIndex::fromRaw(index), plan.operations[index]));
                        return false;
                    }
                }
                return true;
            },
            [&plan, index, &failure](const CompiledCompositionOutput& output) {
                if (output.input.value() >= index || !std::holds_alternative<CompiledLayerStack>(
                                                         plan.operations[output.input.value()])) {
                    failure = diagnostic(
                        EvaluationDiagnosticCode::InvalidPlan,
                        "Composition Output has an invalid stack input", {},
                        subjectFor(OperationIndex::fromRaw(index), plan.operations[index]));
                    return false;
                }
                return true;
            },
        },
        operation);
}

[[nodiscard]] PreflightOutcome preflight(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                         const EvaluationRequest& request,
                                         const CancellationToken& cancellation,
                                         const EvaluationProgressCallback& progress) {
    if (plan == nullptr) {
        return PreflightOutcome::failure(diagnostic(EvaluationDiagnosticCode::InvalidRequest,
                                                    "Evaluation has no compiled plan"));
    }
    if (request.quality != EvaluationQuality::Reference ||
        request.colorIntent != EvaluationColorIntent::ReferenceLinearSrgb) {
        return PreflightOutcome::failure(
            diagnostic(EvaluationDiagnosticCode::InvalidRequest,
                       "Evaluation request uses an unsupported intent"));
    }
    if (plan->operations.empty() || request.output.value() >= plan->operations.size() ||
        request.output != plan->output || request.output.value() + 1 != plan->operations.size() ||
        !std::holds_alternative<CompiledCompositionOutput>(
            plan->operations[request.output.value()])) {
        return PreflightOutcome::failure(
            diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                       "Evaluation output is not the plan's terminal Composition Output"));
    }

    render::ImageExtent extent =
        *render::ImageExtent::create(plan->format.width(), plan->format.height()).value();
    core::PixelAspectRatio pixelAspect = plan->format.pixelAspect();
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    if (const auto* proxy = std::get_if<ProxyResolution>(&request.resolution)) {
        extent = proxy->extent;
        const auto derivedPixelAspect = proxyPixelAspect(plan->format, extent);
        if (!derivedPixelAspect.has_value()) {
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::InvalidProxyPixelAspect,
                "Proxy pixel aspect cannot be represented exactly",
                "Choose a proxy extent whose reduced pixel aspect fits Bloom's ratio type."));
        }
        pixelAspect = *derivedPixelAspect;
        horizontalScale = static_cast<double>(extent.width()) / plan->format.width();
        verticalScale = static_cast<double>(extent.height()) / plan->format.height();
    }

    const auto windowResult = render::ImageWindow::create(0, 0, extent.width(), extent.height());
    if (!windowResult) {
        return PreflightOutcome::failure(
            imageDiagnostic(*windowResult.error(), {}, "Evaluation window is invalid"));
    }
    const auto window = *windowResult.value();
    const auto descriptorResult =
        render::Rgba32fImageDescriptor::create(window, window, pixelAspect);
    if (!descriptorResult) {
        return PreflightOutcome::failure(
            imageDiagnostic(*descriptorResult.error(), {}, "Process image descriptor is invalid"));
    }
    const auto displayDescriptorResult =
        render::ReferenceDisplayBufferDescriptor::create(window, pixelAspect);
    if (!displayDescriptorResult) {
        return PreflightOutcome::failure(imageDiagnostic(*displayDescriptorResult.error(), {},
                                                         "Display image descriptor is invalid"));
    }

    const auto operationCount = plan->operations.size();
    std::vector<bool> reachable(operationCount, false);
    std::vector<std::size_t> pending{request.output.value()};
    while (!pending.empty()) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        const auto index = pending.back();
        pending.pop_back();
        if (index >= operationCount) {
            return PreflightOutcome::failure(
                diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                           "Compiled plan references an invalid operation"));
        }
        if (reachable[index]) {
            continue;
        }
        reachable[index] = true;
        forEachInput(plan->operations[index],
                     [&pending](const OperationIndex input) { pending.push_back(input.value()); });
    }

    std::vector<std::size_t> consumers(operationCount, 0);
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        reportProgress(progress, {.stage = EvaluationProgressStage::Preflight,
                                  .operation = OperationIndex::fromRaw(index),
                                  .completed = index,
                                  .total = operationCount});
        if (!reachable[index]) {
            return PreflightOutcome::failure(
                diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                           "Compiled plan contains an unreachable operation", {},
                           subjectFor(OperationIndex::fromRaw(index), plan->operations[index])));
        }
        EvaluationDiagnostic invalidOperation;
        if (!hasExpectedInputKinds(*plan, index, invalidOperation)) {
            return PreflightOutcome::failure(std::move(invalidOperation));
        }
        bool overflow = false;
        forEachInput(plan->operations[index], [&consumers, &overflow](const OperationIndex input) {
            auto& count = consumers[input.value()];
            if (count == std::numeric_limits<std::size_t>::max()) {
                overflow = true;
            } else {
                ++count;
            }
        });
        if (overflow) {
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::ArithmeticOverflow, "Operation consumer count overflowed",
                {}, subjectFor(OperationIndex::fromRaw(index), plan->operations[index])));
        }
        reportProgress(progress, {.stage = EvaluationProgressStage::Preflight,
                                  .operation = OperationIndex::fromRaw(index),
                                  .completed = index + 1,
                                  .total = operationCount});
    }

    const auto imageBytes = descriptorResult.value()->layout().pixelStorageBytes;
    const auto displayBytes = displayDescriptorResult.value()->layout().pixelStorageBytes;
    auto remaining = consumers;
    std::size_t residentBytes = 0;
    std::size_t peakBytes = 0;
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (cancellation.isCancellationRequested()) {
            return PreflightOutcome::cancellation();
        }
        if (isImageProducing(plan->operations[index])) {
            if (!checkedAdd(residentBytes, imageBytes, residentBytes)) {
                return PreflightOutcome::failure(
                    diagnostic(EvaluationDiagnosticCode::ArithmeticOverflow,
                               "Evaluation working set overflowed"));
            }
            peakBytes = std::max(peakBytes, residentBytes);
        }
        if (index == request.output.value()) {
            continue;
        }
        forEachInput(plan->operations[index],
                     [&remaining, &residentBytes, imageBytes](const OperationIndex input) {
                         auto& count = remaining[input.value()];
                         --count;
                         if (count == 0) {
                             residentBytes -= imageBytes;
                         }
                     });
    }
    if (!checkedAdd(residentBytes, displayBytes, residentBytes)) {
        return PreflightOutcome::failure(diagnostic(EvaluationDiagnosticCode::ArithmeticOverflow,
                                                    "Prepared-frame working set overflowed"));
    }
    peakBytes = std::max(peakBytes, residentBytes);
    if (peakBytes > request.pixelStorageByteLimit) {
        return PreflightOutcome::failure(
            diagnostic(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                       "Evaluation exceeds its pixel-storage budget",
                       "requiredPeakBytes=" + std::to_string(peakBytes) +
                           " byteLimit=" + std::to_string(request.pixelStorageByteLimit)));
    }

    return PreflightOutcome::success(
        ResolvedEvaluation{.imageDescriptor = *descriptorResult.value(),
                           .displayDescriptor = *displayDescriptorResult.value(),
                           .horizontalScale = horizontalScale,
                           .verticalScale = verticalScale,
                           .imageBytes = imageBytes,
                           .displayBytes = displayBytes,
                           .remainingConsumers = std::move(consumers)});
}

[[nodiscard]] EvaluationResult unexpectedAllocationFailure() {
    return EvaluationResult::failed(
        diagnostic(EvaluationDiagnosticCode::AllocationFailure,
                   "Evaluation control storage could not be allocated"));
}

} // namespace detail

using detail::diagnostic;
using detail::forEachInput;
using detail::imageDiagnostic;
using detail::Overloaded;
using detail::preflight;
using detail::reportProgress;
using detail::subjectFor;
using detail::unexpectedAllocationFailure;

EvaluationResult CpuCompositionEvaluator::evaluate(
    std::shared_ptr<const CompiledCompositionPlan> plan, const EvaluationRequest& request,
    const CancellationToken& cancellation, EvaluationProgressCallback progress) const {
    try {
        auto checked = preflight(plan, request, cancellation, progress);
        if (checked.cancelled || cancellation.isCancellationRequested()) {
            return EvaluationResult::cancelled();
        }
        if (!checked.resolved.has_value()) {
            return EvaluationResult::failed(std::move(*checked.diagnostic));
        }
        auto resolved = std::move(*checked.resolved);
        std::vector<std::optional<render::Rgba32fImage>> slots(plan->operations.size());
        std::optional<render::Rgba32fImage> processImage;

        for (std::size_t index = 0; index < plan->operations.size(); ++index) {
            if (cancellation.isCancellationRequested()) {
                return EvaluationResult::cancelled();
            }
            const auto operationIndex = OperationIndex::fromRaw(index);
            const auto operationSubject = subjectFor(operationIndex, plan->operations[index]);
            std::optional<render::Rgba32fImage> produced;
            std::optional<EvaluationDiagnostic> operationFailure;
            bool operationCancelled = false;

            std::visit(
                Overloaded{
                    [&](const CompiledSolid& solid) {
                        const auto pixel =
                            render::solidPixelFromStraightReferenceLinearSrgb(solid.color);
                        if (!pixel) {
                            operationFailure = imageDiagnostic(*pixel.error(), operationSubject,
                                                               "Solid color is not evaluable");
                            return;
                        }
                        auto builder = render::Rgba32fImageBuilder::create(resolved.imageDescriptor,
                                                                           resolved.imageBytes);
                        if (!builder) {
                            operationFailure =
                                imageDiagnostic(*builder.error(), operationSubject,
                                                "Solid process image could not be allocated");
                            return;
                        }
                        const auto height = resolved.imageDescriptor.dataWindow().extent().height();
                        for (std::uint32_t rowIndex = 0; rowIndex < height; ++rowIndex) {
                            if (cancellation.isCancellationRequested()) {
                                operationCancelled = true;
                                return;
                            }
                            const auto y = resolved.imageDescriptor.dataWindow().originY() +
                                           static_cast<std::int64_t>(rowIndex);
                            auto row = builder.value()->row(y);
                            if (!row) {
                                operationFailure =
                                    imageDiagnostic(*row.error(), operationSubject,
                                                    "Solid output row could not be addressed");
                                return;
                            }
                            render::fillSolidRow(*row.value(), *pixel.value());
                            reportProgress(progress, {.stage = EvaluationProgressStage::Operation,
                                                      .operation = operationIndex,
                                                      .completed = rowIndex + 1,
                                                      .total = height});
                        }
                        auto frozen = std::move(*builder.value()).freeze();
                        if (!frozen) {
                            operationFailure =
                                imageDiagnostic(*frozen.error(), operationSubject,
                                                "Solid process image could not be published");
                            return;
                        }
                        produced.emplace(std::move(*frozen.value()));
                    },
                    [&](const CompiledLayerOutput& layer) {
                        const auto fullCenterX = static_cast<double>(plan->format.width()) / 2.0;
                        const auto fullCenterY = static_cast<double>(plan->format.height()) / 2.0;
                        const double translationX =
                            (layer.position.x - fullCenterX) * resolved.horizontalScale;
                        const double translationY =
                            (layer.position.y - fullCenterY) * resolved.verticalScale;
                        const auto parameters = render::TranslationOpacity::create(
                            translationX, translationY, layer.opacity);
                        if (!parameters) {
                            operationFailure =
                                imageDiagnostic(*parameters.error(), operationSubject,
                                                "Layer transform parameters are not evaluable");
                            return;
                        }
                        auto sourceView = slots[layer.input.value()]->view();
                        if (!sourceView) {
                            operationFailure =
                                imageDiagnostic(*sourceView.error(), operationSubject,
                                                "Layer source image is unavailable");
                            return;
                        }
                        auto builder = render::Rgba32fImageBuilder::create(resolved.imageDescriptor,
                                                                           resolved.imageBytes);
                        if (!builder) {
                            operationFailure =
                                imageDiagnostic(*builder.error(), operationSubject,
                                                "Transformed layer image could not be allocated");
                            return;
                        }
                        const auto window = resolved.imageDescriptor.dataWindow();
                        const auto height = window.extent().height();
                        for (std::uint32_t rowIndex = 0; rowIndex < height; ++rowIndex) {
                            if (cancellation.isCancellationRequested()) {
                                operationCancelled = true;
                                return;
                            }
                            const auto y = window.originY() + static_cast<std::int64_t>(rowIndex);
                            auto outputRow = builder.value()->row(y);
                            if (!outputRow) {
                                operationFailure =
                                    imageDiagnostic(*outputRow.error(), operationSubject,
                                                    "Layer output row could not be addressed");
                                return;
                            }
                            if (const auto rowStatus = render::translateOpacityBilinearRow(
                                    *sourceView.value(), window, y, *parameters.value(),
                                    *outputRow.value())) {
                                operationFailure =
                                    imageDiagnostic(*rowStatus, operationSubject,
                                                    "Layer translation could not be evaluated");
                                return;
                            }
                            reportProgress(progress, {.stage = EvaluationProgressStage::Operation,
                                                      .operation = operationIndex,
                                                      .completed = rowIndex + 1,
                                                      .total = height});
                        }
                        auto frozen = std::move(*builder.value()).freeze();
                        if (!frozen) {
                            operationFailure =
                                imageDiagnostic(*frozen.error(), operationSubject,
                                                "Transformed layer image could not be published");
                            return;
                        }
                        produced.emplace(std::move(*frozen.value()));
                    },
                    [&](const CompiledLayerStack& stack) {
                        auto builder = render::Rgba32fImageBuilder::create(
                            resolved.imageDescriptor, resolved.imageBytes,
                            render::Rgba32f::transparent());
                        if (!builder) {
                            operationFailure =
                                imageDiagnostic(*builder.error(), operationSubject,
                                                "Layer Stack image could not be allocated");
                            return;
                        }
                        const auto window = resolved.imageDescriptor.dataWindow();
                        const auto height = window.extent().height();
                        const std::uint64_t totalRows =
                            static_cast<std::uint64_t>(height) * stack.entries.size();
                        std::uint64_t completedRows = 0;
                        for (auto entry = stack.entries.rbegin(); entry != stack.entries.rend();
                             ++entry) {
                            auto sourceView = slots[entry->input.value()]->view();
                            if (!sourceView) {
                                operationFailure =
                                    imageDiagnostic(*sourceView.error(), operationSubject,
                                                    "Layer Stack source image is unavailable");
                                return;
                            }
                            for (std::uint32_t rowIndex = 0; rowIndex < height; ++rowIndex) {
                                if (cancellation.isCancellationRequested()) {
                                    operationCancelled = true;
                                    return;
                                }
                                const auto y =
                                    window.originY() + static_cast<std::int64_t>(rowIndex);
                                auto sourceRow = sourceView.value()->row(y);
                                auto destinationRow = builder.value()->row(y);
                                if (!sourceRow || !destinationRow) {
                                    operationFailure = imageDiagnostic(
                                        sourceRow ? *destinationRow.error() : *sourceRow.error(),
                                        operationSubject, "Layer Stack row could not be addressed");
                                    return;
                                }
                                if (const auto rowStatus = render::sourceOverReferenceLinearSrgbRow(
                                        *sourceRow.value(), *destinationRow.value())) {
                                    operationFailure = imageDiagnostic(
                                        *rowStatus, operationSubject,
                                        "Layer Stack source-over could not be evaluated");
                                    return;
                                }
                                ++completedRows;
                                reportProgress(progress,
                                               {.stage = EvaluationProgressStage::Operation,
                                                .operation = operationIndex,
                                                .completed = completedRows,
                                                .total = totalRows});
                            }
                        }
                        if (stack.entries.empty()) {
                            reportProgress(progress, {.stage = EvaluationProgressStage::Operation,
                                                      .operation = operationIndex,
                                                      .completed = 1,
                                                      .total = 1});
                        }
                        auto frozen = std::move(*builder.value()).freeze();
                        if (!frozen) {
                            operationFailure =
                                imageDiagnostic(*frozen.error(), operationSubject,
                                                "Layer Stack image could not be published");
                            return;
                        }
                        produced.emplace(std::move(*frozen.value()));
                    },
                    [&](const CompiledCompositionOutput& output) {
                        processImage.emplace(std::move(*slots[output.input.value()]));
                        slots[output.input.value()].reset();
                        reportProgress(progress, {.stage = EvaluationProgressStage::Operation,
                                                  .operation = operationIndex,
                                                  .completed = 1,
                                                  .total = 1});
                    },
                },
                plan->operations[index]);

            if (operationCancelled || cancellation.isCancellationRequested()) {
                return EvaluationResult::cancelled();
            }
            if (operationFailure.has_value()) {
                return EvaluationResult::failed(std::move(*operationFailure));
            }
            if (produced.has_value()) {
                slots[index].emplace(std::move(*produced));
            }
            if (index != request.output.value()) {
                forEachInput(plan->operations[index], [&](const OperationIndex input) {
                    auto& remaining = resolved.remainingConsumers[input.value()];
                    --remaining;
                    if (remaining == 0) {
                        slots[input.value()].reset();
                    }
                });
            }
        }

        if (!processImage.has_value() || cancellation.isCancellationRequested()) {
            return cancellation.isCancellationRequested()
                       ? EvaluationResult::cancelled()
                       : EvaluationResult::failed(
                             diagnostic(EvaluationDiagnosticCode::InternalInvariant,
                                        "Composition Output did not publish a process image"));
        }
        auto processView = processImage->view();
        if (!processView) {
            return EvaluationResult::failed(
                imageDiagnostic(*processView.error(), {}, "Published process image is invalid"));
        }
        auto displayBuilder = render::ReferenceDisplayBufferBuilder::create(
            resolved.displayDescriptor, resolved.displayBytes);
        if (!displayBuilder) {
            return EvaluationResult::failed(imageDiagnostic(
                *displayBuilder.error(), {}, "Display buffer could not be allocated"));
        }
        const auto window = resolved.displayDescriptor.displayWindow();
        const auto height = window.extent().height();
        for (std::uint32_t rowIndex = 0; rowIndex < height; ++rowIndex) {
            if (cancellation.isCancellationRequested()) {
                return EvaluationResult::cancelled();
            }
            const auto y = window.originY() + static_cast<std::int64_t>(rowIndex);
            auto outputRow = displayBuilder.value()->row(y);
            if (!outputRow) {
                return EvaluationResult::failed(imageDiagnostic(
                    *outputRow.error(), {}, "Display output row could not be addressed"));
            }
            if (const auto rowStatus = render::mapReferenceLinearSrgbToSrgbRow(
                    *processView.value(), window, y, *outputRow.value())) {
                return EvaluationResult::failed(imageDiagnostic(
                    *rowStatus, {}, "Reference display mapping could not be evaluated"));
            }
            reportProgress(progress, {.stage = EvaluationProgressStage::DisplayMapping,
                                      .operation = request.output,
                                      .completed = rowIndex + 1,
                                      .total = height});
        }
        if (cancellation.isCancellationRequested()) {
            return EvaluationResult::cancelled();
        }
        auto displayBuffer = std::move(*displayBuilder.value()).freeze();
        if (!displayBuffer) {
            return EvaluationResult::failed(imageDiagnostic(
                *displayBuffer.error(), {}, "Display buffer could not be published"));
        }

        EvaluationCacheIdentity identity{
            .plan = std::move(plan),
            .time = request.time,
            .output = request.output,
            .resolution = request.resolution,
            .quality = request.quality,
            .colorIntent = request.colorIntent,
            .provider = EvaluationProvider::CpuReference,
            .displayIntent = EvaluationDisplayIntent::ReferenceLinearSrgbToSrgb,
            .evaluatorSemanticsVersion = kCpuCompositionEvaluatorSemanticsVersion,
            .imagePrimitiveSemanticsVersion = render::kCpuImagePrimitiveSemanticsVersion,
            .displayMapperSemanticsVersion = kReferenceDisplayMapperSemanticsVersion,
        };
        auto frame = std::shared_ptr<const EvaluatedFrame>(new EvaluatedFrame(
            std::move(identity), std::move(*processImage), std::move(*displayBuffer.value())));
        return EvaluationResult::evaluated(std::move(frame));
    } catch (const std::bad_alloc&) {
        return unexpectedAllocationFailure();
    } catch (const std::length_error&) {
        return unexpectedAllocationFailure();
    }
}

} // namespace bloom::runtime
