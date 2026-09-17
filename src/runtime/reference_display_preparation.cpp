#include <bloom/runtime/reference_display_preparation.hpp>

#include <bloom/render/cpu_image_primitives.hpp>

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::runtime::DiagnosticSeverity;
using bloom::runtime::ReferenceDisplayDiagnostic;
using bloom::runtime::ReferenceDisplayDiagnosticCode;

[[nodiscard]] ReferenceDisplayDiagnostic diagnostic(const ReferenceDisplayDiagnosticCode code,
                                                    std::string summary, std::string detail = {}) {
    return {.code = code,
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail)};
}

[[nodiscard]] ReferenceDisplayDiagnostic imageDiagnostic(const ImageError& error,
                                                         std::string summary) {
    auto code = ReferenceDisplayDiagnosticCode::InternalInvariant;
    switch (error.code) {
    case ImageErrorCode::ArithmeticOverflow:
        code = ReferenceDisplayDiagnosticCode::ArithmeticOverflow;
        break;
    case ImageErrorCode::PixelStorageBudgetExceeded:
        code = ReferenceDisplayDiagnosticCode::PixelStorageBudgetExceeded;
        break;
    case ImageErrorCode::AllocationFailure:
        code = ReferenceDisplayDiagnosticCode::AllocationFailure;
        break;
    case ImageErrorCode::InvalidPixel:
    case ImageErrorCode::NonFiniteResult:
        code = ReferenceDisplayDiagnosticCode::InvalidPixel;
        break;
    case ImageErrorCode::UnsupportedFloatingPointEnvironment:
        code = ReferenceDisplayDiagnosticCode::UnsupportedFloatingPointEnvironment;
        break;
    case ImageErrorCode::IncompatibleImageDescriptor:
        code = ReferenceDisplayDiagnosticCode::IncompatibleImageDescriptor;
        break;
    case ImageErrorCode::InvalidExtent:
    case ImageErrorCode::InvalidWindow:
    case ImageErrorCode::InvalidStorageSize:
    case ImageErrorCode::CoordinateOutOfBounds:
    case ImageErrorCode::InvalidState:
    case ImageErrorCode::InvalidParameter:
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
    return diagnostic(code, std::move(summary), std::move(detail));
}

void reportProgress(const bloom::runtime::ReferenceDisplayProgressCallback& callback,
                    const bloom::runtime::ReferenceDisplayProgress& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring is best effort and must not change display pixels.
        return;
    }
}

} // namespace

namespace bloom::runtime {

std::string_view
referenceDisplayDiagnosticCodeId(const ReferenceDisplayDiagnosticCode code) noexcept {
    switch (code) {
    case ReferenceDisplayDiagnosticCode::InvalidRequest:
        return "bloom.runtime.reference-display.invalid-request";
    case ReferenceDisplayDiagnosticCode::ArithmeticOverflow:
        return "bloom.runtime.reference-display.arithmetic-overflow";
    case ReferenceDisplayDiagnosticCode::PixelStorageBudgetExceeded:
        return "bloom.runtime.reference-display.pixel-storage-budget-exceeded";
    case ReferenceDisplayDiagnosticCode::AllocationFailure:
        return "bloom.runtime.reference-display.allocation-failure";
    case ReferenceDisplayDiagnosticCode::InvalidPixel:
        return "bloom.runtime.reference-display.invalid-pixel";
    case ReferenceDisplayDiagnosticCode::UnsupportedFloatingPointEnvironment:
        return "bloom.runtime.reference-display.unsupported-floating-point-environment";
    case ReferenceDisplayDiagnosticCode::IncompatibleImageDescriptor:
        return "bloom.runtime.reference-display.incompatible-image-descriptor";
    case ReferenceDisplayDiagnosticCode::InternalInvariant:
        return "bloom.runtime.reference-display.internal-invariant";
    }
    return "bloom.runtime.reference-display.unknown";
}

ReferenceDisplayFrame::ReferenceDisplayFrame(ReferenceDisplayFrameIdentity identity,
                                             std::shared_ptr<const ProcessFrame> processFrame,
                                             render::PreparedReferenceDisplayBuffer buffer) noexcept
    : identity_(std::move(identity)), processFrame_(std::move(processFrame)),
      buffer_(std::move(buffer)) {}

ReferenceDisplayPreparationResult
ReferenceDisplayPreparationResult::prepared(std::shared_ptr<const ReferenceDisplayFrame> frame,
                                            std::vector<ReferenceDisplayDiagnostic> diagnostics) {
    if (frame == nullptr) {
        return failed(diagnostic(ReferenceDisplayDiagnosticCode::InternalInvariant,
                                 "Display preparation produced no frame"));
    }
    return ReferenceDisplayPreparationResult(ReferenceDisplayPreparationStatus::Prepared,
                                             std::move(frame), std::move(diagnostics));
}

ReferenceDisplayPreparationResult
ReferenceDisplayPreparationResult::cancelled(std::vector<ReferenceDisplayDiagnostic> diagnostics) {
    return ReferenceDisplayPreparationResult(ReferenceDisplayPreparationStatus::Cancelled, {},
                                             std::move(diagnostics));
}

ReferenceDisplayPreparationResult
ReferenceDisplayPreparationResult::failed(ReferenceDisplayDiagnostic value) {
    std::vector<ReferenceDisplayDiagnostic> diagnostics;
    diagnostics.push_back(std::move(value));
    return failed(std::move(diagnostics));
}

ReferenceDisplayPreparationResult
ReferenceDisplayPreparationResult::failed(std::vector<ReferenceDisplayDiagnostic> diagnostics) {
    return ReferenceDisplayPreparationResult(ReferenceDisplayPreparationStatus::Failed, {},
                                             std::move(diagnostics));
}

ReferenceDisplayPreparationResult::ReferenceDisplayPreparationResult(
    const ReferenceDisplayPreparationStatus status,
    std::shared_ptr<const ReferenceDisplayFrame> frame,
    std::vector<ReferenceDisplayDiagnostic> diagnostics) noexcept
    : status_(status), frame_(std::move(frame)), diagnostics_(std::move(diagnostics)) {}

ReferenceDisplayPreparationResult CpuReferenceDisplayPreparer::prepare(
    std::shared_ptr<const ProcessFrame> processFrame,
    const ReferenceDisplayPreparationRequest& request, const CancellationToken& cancellation,
    const ReferenceDisplayProgressCallback& progress, CpuRowBandExecutor* const rowBands) const {
    try {
        if (cancellation.isCancellationRequested()) {
            return ReferenceDisplayPreparationResult::cancelled();
        }
        reportProgress(
            progress,
            {.stage = ReferenceDisplayProgressStage::Preflight, .completed = 0, .total = 1});
        if (processFrame == nullptr || processFrame->identity().plan == nullptr ||
            request.intent != ReferenceDisplayIntent::LinearRec709SceneToSrgb ||
            request.aggregatePixelStorageByteLimit == 0 || !request.viewAdjust.valid()) {
            return ReferenceDisplayPreparationResult::failed(
                diagnostic(ReferenceDisplayDiagnosticCode::InvalidRequest,
                           "Reference display preparation request is invalid"));
        }
        const auto* processDescriptor = processFrame->processImage().descriptor();
        const auto processView = processFrame->processImage().view();
        if (processDescriptor == nullptr || !processView) {
            return ReferenceDisplayPreparationResult::failed(
                processView
                    ? diagnostic(ReferenceDisplayDiagnosticCode::InternalInvariant,
                                 "Process frame has no image descriptor")
                    : imageDiagnostic(*processView.error(), "Process frame image is invalid"));
        }
        const auto displayDescriptorResult = render::ReferenceDisplayBufferDescriptor::create(
            processDescriptor->displayWindow(), processDescriptor->pixelAspect());
        if (!displayDescriptorResult) {
            return ReferenceDisplayPreparationResult::failed(imageDiagnostic(
                *displayDescriptorResult.error(), "Reference display descriptor is invalid"));
        }

        const auto processBytes = processDescriptor->layout().pixelStorageBytes;
        const auto displayBytes = displayDescriptorResult.value()->layout().pixelStorageBytes;
        if (displayBytes > std::numeric_limits<std::size_t>::max() - processBytes) {
            return ReferenceDisplayPreparationResult::failed(
                diagnostic(ReferenceDisplayDiagnosticCode::ArithmeticOverflow,
                           "Reference display working set overflowed"));
        }
        const auto requiredBytes = processBytes + displayBytes;
        if (requiredBytes > request.aggregatePixelStorageByteLimit) {
            return ReferenceDisplayPreparationResult::failed(diagnostic(
                ReferenceDisplayDiagnosticCode::PixelStorageBudgetExceeded,
                "Reference display preparation exceeds its pixel-storage budget",
                "requiredPeakBytes=" + std::to_string(requiredBytes) +
                    " byteLimit=" + std::to_string(request.aggregatePixelStorageByteLimit)));
        }
        reportProgress(
            progress,
            {.stage = ReferenceDisplayProgressStage::Preflight, .completed = 1, .total = 1});
        if (cancellation.isCancellationRequested()) {
            return ReferenceDisplayPreparationResult::cancelled();
        }

        auto displayBuilder = render::ReferenceDisplayBufferBuilder::create(
            *displayDescriptorResult.value(), displayBytes);
        if (!displayBuilder) {
            return ReferenceDisplayPreparationResult::failed(imageDiagnostic(
                *displayBuilder.error(), "Reference display buffer could not be allocated"));
        }
        const auto window = displayDescriptorResult.value()->displayWindow();
        const auto height = window.extent().height();
        // The mapping runs in row BANDS (task PERF1), so the two Mapping progress events bracket
        // the pass instead of counting rows: a band runs on another thread and the callback belongs
        // to the task that owns the frame. `completed == 0` is the start of the pass.
        reportProgress(
            progress,
            {.stage = ReferenceDisplayProgressStage::Mapping, .completed = 0, .total = height});
        auto& buffer = *displayBuilder.value();
        const auto& source = *processView.value();
        const auto outcome = runRowBandPass(
            rowBands, cancellation, height, window.originY(),
            [&buffer, &source, adjust = request.viewAdjust,
             window](const std::int64_t y) -> std::optional<ReferenceDisplayDiagnostic> {
                auto outputRow = buffer.row(y);
                if (!outputRow) {
                    return imageDiagnostic(*outputRow.error(),
                                           "Reference display row could not be addressed");
                }
                if (const auto rowStatus = render::mapLinearRec709SceneToSrgbRow(
                        source, window, y, *outputRow.value())) {
                    return imageDiagnostic(*rowStatus,
                                           "Reference display mapping could not be evaluated");
                }
                if (!adjust.neutral()) {
                    for (std::size_t x = 0; x < outputRow.value()->size(); ++x) {
                        const auto value =
                            source.read(window.originX() + static_cast<std::int64_t>(x), y);
                        if (!value || value.value()->alpha() == 0)
                            continue;
                        const auto pixel = *value.value();
                        const auto alpha = static_cast<double>(pixel.alpha());
                        auto& output = (*outputRow.value())[x];
                        output.red = ViewAdjust::quantize(
                            adjust.fromLinear(static_cast<double>(pixel.red()) / alpha));
                        output.green = ViewAdjust::quantize(
                            adjust.fromLinear(static_cast<double>(pixel.green()) / alpha));
                        output.blue = ViewAdjust::quantize(
                            adjust.fromLinear(static_cast<double>(pixel.blue()) / alpha));
                    }
                }
                return std::nullopt;
            });
        if (outcome.cancelled) {
            return ReferenceDisplayPreparationResult::cancelled();
        }
        if (outcome.failure.has_value()) {
            return ReferenceDisplayPreparationResult::failed(*outcome.failure);
        }
        if (outcome.incomplete) {
            return ReferenceDisplayPreparationResult::failed(
                diagnostic(ReferenceDisplayDiagnosticCode::AllocationFailure,
                           "A display mapping row band could not complete",
                           "A parallel row band ended in an exception; nothing was published."));
        }
        reportProgress(progress, {.stage = ReferenceDisplayProgressStage::Mapping,
                                  .completed = height,
                                  .total = height});
        if (cancellation.isCancellationRequested()) {
            return ReferenceDisplayPreparationResult::cancelled();
        }
        auto displayBuffer = std::move(*displayBuilder.value()).freeze();
        if (!displayBuffer) {
            return ReferenceDisplayPreparationResult::failed(imageDiagnostic(
                *displayBuffer.error(), "Reference display buffer could not be published"));
        }

        ReferenceDisplayFrameIdentity identity{
            .processFrame = processFrame->identity(),
            .intent = request.intent,
            .provider = ReferenceDisplayProvider::CpuReference,
            .pipeline = ReferenceDisplayPipeline::UnqualifiedLinearRec709SceneToSrgb,
            .packing = ReferenceDisplayPacking::StraightRgba8,
            .mapperSemanticsVersion = kReferenceDisplayMapperSemanticsVersion,
            .viewAdjust = request.viewAdjust,
        };
        auto frame = std::shared_ptr<const ReferenceDisplayFrame>(new ReferenceDisplayFrame(
            std::move(identity), std::move(processFrame), std::move(*displayBuffer.value())));
        return ReferenceDisplayPreparationResult::prepared(std::move(frame));
    } catch (const std::bad_alloc&) {
        return ReferenceDisplayPreparationResult::failed(
            diagnostic(ReferenceDisplayDiagnosticCode::AllocationFailure,
                       "Reference display control storage could not be allocated"));
    } catch (const std::length_error&) {
        return ReferenceDisplayPreparationResult::failed(
            diagnostic(ReferenceDisplayDiagnosticCode::AllocationFailure,
                       "Reference display control storage could not be allocated"));
    }
}

} // namespace bloom::runtime
