#pragma once

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/artifact_target_key.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

// docs/architecture/frame-output.md "Pre-Approval Output Analysis Attempt" and "Immutable Export
// Request": the immutable, owning product a completed OutputAnalysisAttempt graph publishes.
// Design decision 2: "The completed attempt RETAINS everything the doc's 'Immutable Export
// Request' bullet list names (snapshot + revision, identities, frame, report, preset/profile,
// canonical target preflight -- reuse the platform preflight/ArtifactTargetKey machinery)."
//
// This type is pure/Qt-free (no platform::StagedArtifactCoordinator, no
// bloom::runtime::TaskScheduler): it only RETAINS the plain-data outcome of a platform preflight
// call (ArtifactTargetKey + ArtifactTargetObservation, both value types), never a live
// platform::StagedArtifactTarget/Lease handle. bloom::host owns making the actual
// StagedArtifactCoordinator::preflight() call (the "Resolving" task) and the multi-task attempt
// controller that drives Resolving -> Evaluating -> Identifying -> Analyzing and then calls
// buildOutputAnalysisAttemptV1() with this file's inputs (bloom/host/
// output_analysis_attempt_runner.hpp) -- design decision 1's module split ("src/output owns the
// Qt-free attempt/job orchestration library ... src/host owns executeExportPublication that binds
// the output orchestration to the application-wide PublicationCoordinator + platform
// StagedArtifactCoordinator").
namespace bloom::output {

struct OutputAnalysisAttemptTargetV1 final {
    core::ArtifactTargetKey targetKey;
    platform::ArtifactTargetObservation observation = platform::ArtifactTargetObservation::absent();
    std::filesystem::path targetPath;
    platform::ArtifactOverwritePolicy overwritePolicy =
        platform::ArtifactOverwritePolicy::CreateOrReplace;

    [[nodiscard]] bool isValid() const noexcept { return targetKey.isValid(); }
};

// The PNG-only products the attempt graph's step 4 produces ("for PNG, bounded color/config
// resolution plus preparation or exact reuse of the qualified display processor and its canonical
// DisplayProcessorIdentity; EXR has no display product") and that the "Immutable Export Request"
// bullet list requires a completed attempt to retain ("the qualified PNG display-processor handle
// and identity when applicable"). Every field is empty for EXR, and for a PNG attempt whose color
// resolution or adapter was not Ready -- such an attempt still completes with a truthful
// non-approvable report (frame-output.md: "A failed or unavailable stage may still produce the
// truthful nonapprovable report ... but it never produces a placeholder identity or approval
// digest").
//
// `identity` is expected to alias `processor`'s own DisplayProcessorIdentityV1 (the aliasing
// std::shared_ptr constructor), never an independently-adopted copy: the doc forbids pairing a
// frame or processor with a substitute identity at approval or export, and aliasing makes that
// pairing structural rather than checked.
struct OutputAnalysisAttemptDisplayProductsV1 final {
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> processor;
    std::shared_ptr<const color::DisplayProcessorIdentityV1> identity;
    std::optional<core::Sha256Digest> expectedOcioRevision;

    [[nodiscard]] bool isPresent() const noexcept {
        return processor != nullptr && identity != nullptr && expectedOcioRevision.has_value();
    }
    [[nodiscard]] bool isAbsent() const noexcept {
        return processor == nullptr && identity == nullptr && !expectedOcioRevision.has_value();
    }
};

enum class OutputAnalysisAttemptErrorCodeV1 : std::uint8_t {
    None,
    InvalidTarget,
    InvalidFrame,
    InvalidIdentity,
    InvalidReport,
    // The retained display products do not match the report's own preset: EXR carries any of them
    // at all, a PNG report carries a partially-populated set, or an APPROVABLE PNG report carries
    // none (an approvable PNG report exists only when color resolution and the adapter were both
    // Ready, which is exactly when the processor/identity/revision triple exists).
    InvalidDisplayProducts,
    DigestFailed,
    ResourceReservationFailed,
    InternalInvariant,
};

class OutputAnalysisAttemptV1;

struct OutputAnalysisAttemptBuildInputsV1 final {
    std::shared_ptr<const runtime::ProcessFrame> frame;
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity;
    std::shared_ptr<const OutputAnalysisReportV1> report;
    OutputAnalysisAttemptTargetV1 target;
    OutputAnalysisAttemptDisplayProductsV1 display;
};

class [[nodiscard]] OutputAnalysisAttemptBuildResultV1 final {
  public:
    [[nodiscard]] bool hasAttempt() const noexcept { return attempt_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasAttempt(); }
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisAttemptV1>& attempt() const& noexcept {
        return attempt_;
    }
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisAttemptV1>& attempt() const&& = delete;
    [[nodiscard]] OutputAnalysisAttemptErrorCodeV1 error() const noexcept { return error_; }

    [[nodiscard]] static OutputAnalysisAttemptBuildResultV1
    success(std::shared_ptr<const OutputAnalysisAttemptV1> attempt) noexcept;
    [[nodiscard]] static OutputAnalysisAttemptBuildResultV1
    failure(OutputAnalysisAttemptErrorCodeV1 error) noexcept;

  private:
    std::shared_ptr<const OutputAnalysisAttemptV1> attempt_;
    OutputAnalysisAttemptErrorCodeV1 error_ = OutputAnalysisAttemptErrorCodeV1::InternalInvariant;
};

// Retains, as one inseparable immutable product: the evaluated ProcessFrame (which itself retains
// the document snapshot/revision and project/composition/output/time identity through its own
// ProcessFrameIdentity), the frame-bound ProcessFrameSemanticIdentityV1, the owning
// OutputAnalysisReportV1, the approval OutputAnalysisDigest (present only when the report is
// approvable -- frame-output.md: "OutputAnalysisDigest becomes available only when a validated
// frame-bound process identity exists ... Approval requires both that digest and all eleven
// derived permission bits"), the typed preset, the qualified PNG display-processor handle and its
// canonical identity when applicable, the canonical target preflight, and its own resource
// reservation
// (released when the last shared_ptr to this attempt, or to a FrameExportRequestV1 that still
// retains it, is dropped -- "Dismissal or supersession cancels unfinished work and releases the
// completed attempt and its reservations when no admitted export retains them").
class OutputAnalysisAttemptV1 final {
  public:
    OutputAnalysisAttemptV1(const OutputAnalysisAttemptV1&) = delete;
    OutputAnalysisAttemptV1& operator=(const OutputAnalysisAttemptV1&) = delete;
    OutputAnalysisAttemptV1(OutputAnalysisAttemptV1&&) = delete;
    OutputAnalysisAttemptV1& operator=(OutputAnalysisAttemptV1&&) = delete;
    ~OutputAnalysisAttemptV1() = default;

    [[nodiscard]] const std::shared_ptr<const runtime::ProcessFrame>& frame() const& noexcept {
        return frame_;
    }
    [[nodiscard]] const std::shared_ptr<const runtime::ProcessFrame>& frame() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const& noexcept {
        return processIdentity_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const& noexcept {
        return report_;
    }
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const&& = delete;
    [[nodiscard]] bool approvable() const noexcept { return digest_.has_value(); }
    [[nodiscard]] std::optional<core::Sha256Digest> digest() const noexcept { return digest_; }
    // The typed preset this attempt was analyzed for -- read straight off the retained report, so
    // it can never disagree with the report's own facet vocabulary or descriptor schema.
    [[nodiscard]] OutputPresetV1 preset() const noexcept { return preset_; }
    [[nodiscard]] const OutputAnalysisAttemptDisplayProductsV1& display() const& noexcept {
        return display_;
    }
    [[nodiscard]] const OutputAnalysisAttemptDisplayProductsV1& display() const&& = delete;
    [[nodiscard]] const OutputAnalysisAttemptTargetV1& target() const noexcept { return target_; }
    [[nodiscard]] const std::shared_ptr<ExportResourceReservationV1>& resources() const& noexcept {
        return reservation_;
    }
    [[nodiscard]] const std::shared_ptr<ExportResourceReservationV1>& resources() const&& = delete;

  private:
    friend OutputAnalysisAttemptBuildResultV1
    buildOutputAnalysisAttemptV1(OutputAnalysisAttemptBuildInputsV1 inputs,
                                 ExportResourceLedgerV1& ledger) noexcept;

    OutputAnalysisAttemptV1(std::shared_ptr<const runtime::ProcessFrame> frame,
                            std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
                            std::shared_ptr<const OutputAnalysisReportV1> report,
                            std::optional<core::Sha256Digest> digest, OutputPresetV1 preset,
                            OutputAnalysisAttemptTargetV1 target,
                            OutputAnalysisAttemptDisplayProductsV1 display,
                            std::shared_ptr<ExportResourceReservationV1> reservation) noexcept;

    std::shared_ptr<const runtime::ProcessFrame> frame_;
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity_;
    std::shared_ptr<const OutputAnalysisReportV1> report_;
    std::optional<core::Sha256Digest> digest_;
    OutputPresetV1 preset_ = OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    OutputAnalysisAttemptTargetV1 target_;
    OutputAnalysisAttemptDisplayProductsV1 display_;
    std::shared_ptr<ExportResourceReservationV1> reservation_;
};

// The Analyzing stage's typed successor: computes the OutputAnalysisDigest (when approvable),
// reserves the checked retained bytes against `ledger` (process-pixel bytes + report descriptor
// bytes + the closed digest-preimage bound), and publishes the immutable attempt only after every
// check succeeds -- "Cancellation, allocation failure, a resource-limit failure, or any invariant
// failure publishes no partial identity" extended here to "no partial attempt".
[[nodiscard]] OutputAnalysisAttemptBuildResultV1
buildOutputAnalysisAttemptV1(OutputAnalysisAttemptBuildInputsV1 inputs,
                             ExportResourceLedgerV1& ledger) noexcept;

} // namespace bloom::output
