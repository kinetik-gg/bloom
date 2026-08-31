#include <bloom/output/output_analysis_attempt.hpp>

#include <bloom/output/output_analysis_digest.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/render/image.hpp>

#include <limits>
#include <utility>

namespace bloom::output {

OutputAnalysisAttemptBuildResultV1 OutputAnalysisAttemptBuildResultV1::success(
    std::shared_ptr<const OutputAnalysisAttemptV1> attempt) noexcept {
    OutputAnalysisAttemptBuildResultV1 result;
    result.attempt_ = std::move(attempt);
    result.error_ = OutputAnalysisAttemptErrorCodeV1::None;
    return result;
}

OutputAnalysisAttemptBuildResultV1
OutputAnalysisAttemptBuildResultV1::failure(const OutputAnalysisAttemptErrorCodeV1 error) noexcept {
    OutputAnalysisAttemptBuildResultV1 result;
    result.error_ = error == OutputAnalysisAttemptErrorCodeV1::None
                        ? OutputAnalysisAttemptErrorCodeV1::InternalInvariant
                        : error;
    return result;
}

OutputAnalysisAttemptV1::OutputAnalysisAttemptV1(
    std::shared_ptr<const runtime::ProcessFrame> frame,
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
    std::shared_ptr<const OutputAnalysisReportV1> report,
    const std::optional<core::Sha256Digest> digest, OutputAnalysisAttemptTargetV1 target,
    std::shared_ptr<ExportResourceReservationV1> reservation) noexcept
    : frame_(std::move(frame)), processIdentity_(std::move(processIdentity)),
      report_(std::move(report)), digest_(digest), target_(std::move(target)),
      reservation_(std::move(reservation)) {}

namespace {

// Checked sum of the retained-product bytes the completed attempt charges through the approval
// decision: the retained process-pixel bytes, the report's bounded descriptor storage, and a fixed
// bound for the digest's own bounded streaming preimage (never allocated in full --
// output_analysis_digest.hpp streams it -- but charged conservatively at its closed cap anyway, so
// this reservation is never an under-count of what Analyzing/digesting can touch).
[[nodiscard]] std::optional<std::uint64_t>
checkedAttemptRetainedBytes(const render::Rgba32fImageDescriptor& descriptor,
                            const OutputAnalysisReportV1& report) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total = descriptor.layout().pixelStorageBytes;
    const auto descriptorBytes = static_cast<std::uint64_t>(report.descriptorByteCount());
    if (descriptorBytes > maximum - total) {
        return std::nullopt;
    }
    total += descriptorBytes;
    constexpr auto digestPreimageBound =
        static_cast<std::uint64_t>(kOutputAnalysisDigestMaximumPreimageBytesV1);
    if (digestPreimageBound > maximum - total) {
        return std::nullopt;
    }
    total += digestPreimageBound;
    return total;
}

} // namespace

OutputAnalysisAttemptBuildResultV1
buildOutputAnalysisAttemptV1(OutputAnalysisAttemptBuildInputsV1 inputs,
                             ExportResourceLedgerV1& ledger) noexcept {
    if (!inputs.target.isValid()) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidTarget);
    }
    if (inputs.frame == nullptr || !inputs.frame->processImage().isValid() ||
        inputs.frame->processImage().descriptor() == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidFrame);
    }
    if (inputs.processIdentity == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidIdentity);
    }
    if (inputs.report == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidReport);
    }

    // A nonapprovable report is still a successful, completed attempt (frame-output.md: "A
    // nonapprovable but valid report is a successful analysis attempt"); only an approvable report
    // gets a digest at all.
    std::optional<core::Sha256Digest> digest;
    if (inputs.report->approvable()) {
        const auto digestResult =
            computeOutputAnalysisDigestV1(*inputs.processIdentity, inputs.report->view(), {});
        if (!digestResult) {
            return OutputAnalysisAttemptBuildResultV1::failure(
                OutputAnalysisAttemptErrorCodeV1::DigestFailed);
        }
        digest = *digestResult.digest();
    }

    const auto* descriptor = inputs.frame->processImage().descriptor();
    const auto retainedBytes = checkedAttemptRetainedBytes(*descriptor, *inputs.report);
    if (!retainedBytes.has_value()) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed);
    }

    ExportResourceAdmissionStatusV1 admissionStatus = ExportResourceAdmissionStatusV1::Reserved;
    auto reservation = ledger.reserve(*retainedBytes, admissionStatus);
    if (reservation == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed);
    }

    try {
        auto attempt = std::shared_ptr<OutputAnalysisAttemptV1>(new OutputAnalysisAttemptV1(
            std::move(inputs.frame), std::move(inputs.processIdentity), std::move(inputs.report),
            digest, std::move(inputs.target), std::move(reservation)));
        return OutputAnalysisAttemptBuildResultV1::success(std::move(attempt));
    } catch (const std::bad_alloc&) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed);
    }
}

} // namespace bloom::output
