#include <bloom/runtime/qualified_display_preparation.hpp>

#include <utility>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::runtime::DiagnosticSeverity;
using bloom::runtime::QualifiedDisplayDiagnostic;
using bloom::runtime::QualifiedDisplayDiagnosticCode;

[[nodiscard]] QualifiedDisplayDiagnostic diagnostic(const QualifiedDisplayDiagnosticCode code,
                                                    std::string summary, std::string detail = {}) {
    return {.code = code,
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail)};
}

[[nodiscard]] QualifiedDisplayDiagnostic imageDiagnostic(const ImageError& error,
                                                         std::string summary) {
    auto code = QualifiedDisplayDiagnosticCode::InternalInvariant;
    switch (error.code) {
    case ImageErrorCode::PixelStorageBudgetExceeded:
        code = QualifiedDisplayDiagnosticCode::PixelStorageBudgetExceeded;
        break;
    case ImageErrorCode::AllocationFailure:
        code = QualifiedDisplayDiagnosticCode::AllocationFailure;
        break;
    case ImageErrorCode::InvalidPixel:
    case ImageErrorCode::NonFiniteResult:
        code = QualifiedDisplayDiagnosticCode::InvalidPixel;
        break;
    case ImageErrorCode::UnsupportedFloatingPointEnvironment:
        code = QualifiedDisplayDiagnosticCode::UnsupportedFloatingPointEnvironment;
        break;
    case ImageErrorCode::IncompatibleImageDescriptor:
        code = QualifiedDisplayDiagnosticCode::IncompatibleImageDescriptor;
        break;
    case ImageErrorCode::InvalidExtent:
    case ImageErrorCode::InvalidWindow:
    case ImageErrorCode::ArithmeticOverflow:
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

void reportProgress(const bloom::runtime::QualifiedDisplayProgressCallback& callback,
                    const bloom::runtime::QualifiedDisplayProgress& progress) noexcept {
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
qualifiedDisplayDiagnosticCodeId(const QualifiedDisplayDiagnosticCode code) noexcept {
    switch (code) {
    case QualifiedDisplayDiagnosticCode::InvalidRequest:
        return "bloom.runtime.qualified-display.invalid-request";
    case QualifiedDisplayDiagnosticCode::PixelStorageBudgetExceeded:
        return "bloom.runtime.qualified-display.pixel-storage-budget-exceeded";
    case QualifiedDisplayDiagnosticCode::AllocationFailure:
        return "bloom.runtime.qualified-display.allocation-failure";
    case QualifiedDisplayDiagnosticCode::InvalidPixel:
        return "bloom.runtime.qualified-display.invalid-pixel";
    case QualifiedDisplayDiagnosticCode::UnsupportedFloatingPointEnvironment:
        return "bloom.runtime.qualified-display.unsupported-floating-point-environment";
    case QualifiedDisplayDiagnosticCode::IncompatibleImageDescriptor:
        return "bloom.runtime.qualified-display.incompatible-image-descriptor";
    case QualifiedDisplayDiagnosticCode::InternalInvariant:
        return "bloom.runtime.qualified-display.internal-invariant";
    }
    return "bloom.runtime.qualified-display.unknown";
}

QualifiedDisplayFrame::QualifiedDisplayFrame(QualifiedDisplayFrameIdentity identity,
                                             std::shared_ptr<const ProcessFrame> processFrame,
                                             color::PreparedDisplayFrame buffer) noexcept
    : identity_(std::move(identity)), processFrame_(std::move(processFrame)),
      buffer_(std::move(buffer)) {}

QualifiedDisplayPreparationResult
QualifiedDisplayPreparationResult::prepared(std::shared_ptr<const QualifiedDisplayFrame> frame,
                                            std::vector<QualifiedDisplayDiagnostic> diagnostics) {
    if (frame == nullptr) {
        return failed(diagnostic(QualifiedDisplayDiagnosticCode::InternalInvariant,
                                 "Qualified display preparation produced no frame"));
    }
    return QualifiedDisplayPreparationResult(QualifiedDisplayPreparationStatus::Prepared,
                                             std::move(frame), std::move(diagnostics));
}

QualifiedDisplayPreparationResult
QualifiedDisplayPreparationResult::cancelled(std::vector<QualifiedDisplayDiagnostic> diagnostics) {
    return QualifiedDisplayPreparationResult(QualifiedDisplayPreparationStatus::Cancelled, {},
                                             std::move(diagnostics));
}

QualifiedDisplayPreparationResult
QualifiedDisplayPreparationResult::failed(QualifiedDisplayDiagnostic value) {
    std::vector<QualifiedDisplayDiagnostic> diagnostics;
    diagnostics.push_back(std::move(value));
    return failed(std::move(diagnostics));
}

QualifiedDisplayPreparationResult
QualifiedDisplayPreparationResult::failed(std::vector<QualifiedDisplayDiagnostic> diagnostics) {
    return QualifiedDisplayPreparationResult(QualifiedDisplayPreparationStatus::Failed, {},
                                             std::move(diagnostics));
}

QualifiedDisplayPreparationResult::QualifiedDisplayPreparationResult(
    const QualifiedDisplayPreparationStatus status,
    std::shared_ptr<const QualifiedDisplayFrame> frame,
    std::vector<QualifiedDisplayDiagnostic> diagnostics) noexcept
    : status_(status), frame_(std::move(frame)), diagnostics_(std::move(diagnostics)) {}

QualifiedDisplayPreparationResult
CpuQualifiedDisplayPreparer::prepare(std::shared_ptr<const ProcessFrame> processFrame,
                                     const QualifiedDisplayPreparationRequest& request,
                                     const CancellationToken& cancellation,
                                     const QualifiedDisplayProgressCallback& progress) const {
    if (cancellation.isCancellationRequested()) {
        return QualifiedDisplayPreparationResult::cancelled();
    }
    reportProgress(progress,
                   {.stage = QualifiedDisplayProgressStage::Preflight, .completed = 0, .total = 1});
    if (handle_ == nullptr || processFrame == nullptr || processFrame->identity().plan == nullptr ||
        request.chunkPixelCount == 0 || request.aggregatePixelStorageByteLimit == 0) {
        return QualifiedDisplayPreparationResult::failed(
            diagnostic(QualifiedDisplayDiagnosticCode::InvalidRequest,
                       "Qualified display preparation request is invalid"));
    }
    const auto processView = processFrame->processImage().view();
    if (!processView) {
        return QualifiedDisplayPreparationResult::failed(
            imageDiagnostic(*processView.error(), "Process frame image is invalid"));
    }
    reportProgress(progress,
                   {.stage = QualifiedDisplayProgressStage::Preflight, .completed = 1, .total = 1});
    if (cancellation.isCancellationRequested()) {
        return QualifiedDisplayPreparationResult::cancelled();
    }

    reportProgress(progress,
                   {.stage = QualifiedDisplayProgressStage::Applying, .completed = 0, .total = 0});
    // Bridges runtime's CancellationToken to color::CancellationPredicateRef -- bloom_color_ocio
    // stays free of a runtime dependency (see CancellationPredicateRef's own documentation): the
    // predicate is a plain non-owning callable, and C2's own chunk loop (produceBloomNeutralDisplay
    // Frame) already checks it between chunks and applies applyBloomNeutralDisplayChunk within one
    // chunk, satisfying "applies the C2 chunk API with cancellation checks at chunk boundaries".
    // Not const: color::CancellationPredicateRef's templated constructor binds a non-const F&, and
    // this lambda has no mutable state to protect anyway (it only reads `cancellation` by
    // reference).
    auto isCancelled = [&cancellation]() noexcept {
        return cancellation.isCancellationRequested();
    };
    auto produced = color::produceBloomNeutralDisplayFrame(
        *handle_, *processView.value(), request.chunkPixelCount,
        request.aggregatePixelStorageByteLimit, isCancelled);
    if (!produced) {
        if (cancellation.isCancellationRequested()) {
            return QualifiedDisplayPreparationResult::cancelled();
        }
        return QualifiedDisplayPreparationResult::failed(
            imageDiagnostic(*produced.error(), "Qualified display mapping could not be evaluated"));
    }
    if (cancellation.isCancellationRequested()) {
        return QualifiedDisplayPreparationResult::cancelled();
    }
    reportProgress(progress,
                   {.stage = QualifiedDisplayProgressStage::Applying, .completed = 1, .total = 1});

    QualifiedDisplayFrameIdentity identity{
        .processFrame = processFrame->identity(),
        .provider = QualifiedDisplayProvider::CpuBloomNeutral,
        .packing = QualifiedDisplayPacking::StraightRgba8,
        .preparerSemanticsVersion = kQualifiedDisplayPreparerSemanticsVersion,
    };
    auto frame = std::shared_ptr<const QualifiedDisplayFrame>(new QualifiedDisplayFrame(
        std::move(identity), std::move(processFrame), std::move(*produced.value())));
    return QualifiedDisplayPreparationResult::prepared(std::move(frame));
}

} // namespace bloom::runtime
