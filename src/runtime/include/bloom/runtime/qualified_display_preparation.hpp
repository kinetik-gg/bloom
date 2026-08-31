#pragma once

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The qualified counterpart of reference_display_preparation.hpp (issue #97, task C3): applies the
// C2 qualified Bloom Neutral CPU display processor (bloom/color/ocio_cpu_display_frame.hpp) to a
// ProcessFrame instead of the temporary built-in reference mapper. This header deliberately mirrors
// CpuReferenceDisplayPreparer's shape (request/identity/frame/result/preparer) so the two paths
// stay side by side; see docs/architecture/color-management.md, "CPU Display Processor Boundary"
// and "Process And Display Separation".
//
// Display-window mechanism (design decision 1): produceBloomNeutralDisplayFrame() windows its
// output to the source ProcessFrame's DATA window, not its display window (C2's recorded scope
// decision -- "the prepared frame's window equals the source process frame's data window, no
// padding composite"). CpuReferenceDisplayPreparer instead composites into the process descriptor's
// full DISPLAY window, padding any area outside the data window with transparent black
// (bloom::render::mapLinearRec709SceneToSrgbRow). This preparer reproduces that reference behavior
// exactly under the one invariant every ProcessFrame this application produces currently satisfies:
// bloom::runtime::CpuCompositionEvaluator always constructs Rgba32fImageDescriptor with dataWindow
// == displayWindow (see cpu_composition_evaluator.cpp's Rgba32fImageDescriptor::create(window,
// window, pixelAspect) call) -- so C2's data-window-sized output already covers the process frame's
// entire display window and no separate padding composite is ever needed to match the reference
// path bit-for-bit. If a future evaluator slice introduces a data window narrower than the display
// window, the two paths would visibly diverge (this preparer would report the narrower data window
// rather than a padded display window) until a later change teaches this preparer to composite
// around C2's frame the way the reference preparer composites around its own row-mapped pixels --
// this is a disclosed, currently-inert scope edge, not a silent behavior gap.
namespace bloom::runtime {

// Bounded pixels-per-call granularity handed to color::produceBloomNeutralDisplayFrame's own
// internal chunking/cancellation loop. Overridable per request for tests; production requests use
// this default.
inline constexpr std::size_t kDefaultQualifiedDisplayChunkPixelCount = 65536;

inline constexpr std::uint32_t kQualifiedDisplayPreparerSemanticsVersion = 1;

struct QualifiedDisplayPreparationRequest final {
    std::size_t aggregatePixelStorageByteLimit = 0;
    std::size_t chunkPixelCount = kDefaultQualifiedDisplayChunkPixelCount;
};

enum class QualifiedDisplayProvider : std::uint8_t {
    CpuBloomNeutral,
};

enum class QualifiedDisplayPacking : std::uint8_t {
    StraightRgba8,
};

// Deliberately does NOT embed color::DisplayProcessorIdentityV1 (move-construct-only, no
// operator==; see display_processor_identity.hpp) -- the complete portable display-processor
// identity is retained where C2 already owns it, on the embedded color::PreparedDisplayFrame's own
// identity() accessor (QualifiedDisplayFrame::buffer().identity()), exactly as
// ReferenceDisplayFrameIdentity does not duplicate render::PreparedReferenceDisplayBuffer's own
// layout/window fields either. This identity struct stays a small, comparable value like its
// reference counterpart.
struct QualifiedDisplayFrameIdentity final {
    ProcessFrameIdentity processFrame;
    QualifiedDisplayProvider provider = QualifiedDisplayProvider::CpuBloomNeutral;
    QualifiedDisplayPacking packing = QualifiedDisplayPacking::StraightRgba8;
    std::uint32_t preparerSemanticsVersion = kQualifiedDisplayPreparerSemanticsVersion;

    friend bool operator==(const QualifiedDisplayFrameIdentity&,
                           const QualifiedDisplayFrameIdentity&) = default;
};

enum class QualifiedDisplayPreparationStatus : std::uint8_t {
    Prepared,
    Cancelled,
    Failed,
};

enum class QualifiedDisplayDiagnosticCode : std::uint8_t {
    InvalidRequest,
    PixelStorageBudgetExceeded,
    AllocationFailure,
    InvalidPixel,
    UnsupportedFloatingPointEnvironment,
    IncompatibleImageDescriptor,
    InternalInvariant,
};

[[nodiscard]] std::string_view
qualifiedDisplayDiagnosticCodeId(QualifiedDisplayDiagnosticCode code) noexcept;

struct QualifiedDisplayDiagnostic final {
    QualifiedDisplayDiagnosticCode code = QualifiedDisplayDiagnosticCode::InternalInvariant;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string summary;
    std::string detail;

    friend bool operator==(const QualifiedDisplayDiagnostic&,
                           const QualifiedDisplayDiagnostic&) = default;
};

enum class QualifiedDisplayProgressStage : std::uint8_t {
    Preflight,
    Applying,
};

struct QualifiedDisplayProgress final {
    QualifiedDisplayProgressStage stage = QualifiedDisplayProgressStage::Preflight;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;

    friend bool operator==(const QualifiedDisplayProgress&,
                           const QualifiedDisplayProgress&) = default;
};

using QualifiedDisplayProgressCallback = std::function<void(const QualifiedDisplayProgress&)>;

// Boundary product 1 of this task: identity + retained shared_ptr<const ProcessFrame> + the color
// module's PreparedDisplayFrame, the same ownership split as ReferenceDisplayFrame (design decision
// 1). Move-assignment is deleted, not omitted: color::PreparedDisplayFrame itself deletes
// move-assignment (one of its own members -- DisplayProcessorIdentityV1 -- is not
// move-assignable), so this class cannot be either.
class QualifiedDisplayFrame final {
  public:
    QualifiedDisplayFrame(const QualifiedDisplayFrame&) = delete;
    QualifiedDisplayFrame& operator=(const QualifiedDisplayFrame&) = delete;
    QualifiedDisplayFrame(QualifiedDisplayFrame&&) noexcept = default;
    QualifiedDisplayFrame& operator=(QualifiedDisplayFrame&&) = delete;
    ~QualifiedDisplayFrame() = default;

    [[nodiscard]] const QualifiedDisplayFrameIdentity& identity() const& noexcept {
        return identity_;
    }
    [[nodiscard]] const QualifiedDisplayFrameIdentity& identity() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const& noexcept {
        return processFrame_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const&& = delete;
    [[nodiscard]] const color::PreparedDisplayFrame& buffer() const& noexcept { return buffer_; }
    [[nodiscard]] const color::PreparedDisplayFrame& buffer() const&& = delete;

  private:
    friend class CpuQualifiedDisplayPreparer;

    QualifiedDisplayFrame(QualifiedDisplayFrameIdentity identity,
                          std::shared_ptr<const ProcessFrame> processFrame,
                          color::PreparedDisplayFrame buffer) noexcept;

    QualifiedDisplayFrameIdentity identity_;
    std::shared_ptr<const ProcessFrame> processFrame_;
    color::PreparedDisplayFrame buffer_;
};

class QualifiedDisplayPreparationResult final {
  public:
    [[nodiscard]] static QualifiedDisplayPreparationResult
    prepared(std::shared_ptr<const QualifiedDisplayFrame> frame,
             std::vector<QualifiedDisplayDiagnostic> diagnostics = {});
    [[nodiscard]] static QualifiedDisplayPreparationResult
    cancelled(std::vector<QualifiedDisplayDiagnostic> diagnostics = {});
    [[nodiscard]] static QualifiedDisplayPreparationResult
    failed(QualifiedDisplayDiagnostic diagnostic);
    [[nodiscard]] static QualifiedDisplayPreparationResult
    failed(std::vector<QualifiedDisplayDiagnostic> diagnostics);

    [[nodiscard]] QualifiedDisplayPreparationStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::shared_ptr<const QualifiedDisplayFrame>& frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] const std::vector<QualifiedDisplayDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    QualifiedDisplayPreparationResult(QualifiedDisplayPreparationStatus status,
                                      std::shared_ptr<const QualifiedDisplayFrame> frame,
                                      std::vector<QualifiedDisplayDiagnostic> diagnostics) noexcept;

    QualifiedDisplayPreparationStatus status_ = QualifiedDisplayPreparationStatus::Failed;
    std::shared_ptr<const QualifiedDisplayFrame> frame_;
    std::vector<QualifiedDisplayDiagnostic> diagnostics_;
};

// Mirrors CpuReferenceDisplayPreparer's shape exactly: a small value type whose prepare() takes
// (processFrame, request, cancellation, progress). The one addition is the
// PreparedCpuDisplayProcessorHandle this preparer applies, supplied at CONSTRUCTION (design
// decision 3) rather than as a fifth prepare() parameter -- callers construct a fresh preparer
// around whichever handle QualifiedDisplayProcessorProvider currently publishes (see
// qualified_display_processor_provider.hpp), exactly the way CpuReferenceDisplayPreparer itself is
// a stateless value constructed fresh per use. The handle reference must outlive every prepare()
// call made through this preparer.
class CpuQualifiedDisplayPreparer final {
  public:
    explicit CpuQualifiedDisplayPreparer(
        const color::PreparedCpuDisplayProcessorHandle& handle) noexcept
        : handle_(&handle) {}

    [[nodiscard]] QualifiedDisplayPreparationResult
    prepare(std::shared_ptr<const ProcessFrame> processFrame,
            const QualifiedDisplayPreparationRequest& request,
            const CancellationToken& cancellation,
            const QualifiedDisplayProgressCallback& progress = {}) const;

  private:
    const color::PreparedCpuDisplayProcessorHandle* handle_;
};

} // namespace bloom::runtime
