#include <bloom/host/frame_export_publication.hpp>

#include <bloom/core/sha256.hpp>
#include <bloom/output/output_limits.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::host {

FrameExportApprovalResultV1::FrameExportApprovalResultV1(
    const FrameExportApprovalStatusV1 status) noexcept
    : status_(status) {}

FrameExportApprovalResultV1::FrameExportApprovalResultV1(
    std::unique_ptr<FrameExportRequestV1> request) noexcept
    : status_(FrameExportApprovalStatusV1::Approved), request_(std::move(request)) {}

FrameExportApprovalResultV1::~FrameExportApprovalResultV1() = default;

std::unique_ptr<FrameExportRequestV1> FrameExportApprovalResultV1::takeRequest() && noexcept {
    return std::move(request_);
}

FrameExportRequestV1::FrameExportRequestV1(
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt, PublicationTargetClaim claim,
    const FrameExportLimitsV1 limits) noexcept
    : attempt_(std::move(attempt)), claim_(std::move(claim)), limits_(limits) {}

FrameExportApprovalResultV1
approveFrameExportV1(PublicationCoordinator& coordinator,
                     std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt,
                     const core::Sha256Digest approverDigest, const FrameExportLimitsV1 limits,
                     PublicationAdmission* const preAdmitted) {
    if (attempt == nullptr || !attempt->approvable()) {
        return FrameExportApprovalResultV1(FrameExportApprovalStatusV1::NotApprovable);
    }
    if (!attempt->digest().has_value() || *attempt->digest() != approverDigest) {
        return FrameExportApprovalResultV1(FrameExportApprovalStatusV1::DigestMismatch);
    }

    std::optional<PublicationAdmission> admission;
    if (preAdmitted != nullptr) {
        admission.emplace(std::move(*preAdmitted));
    } else {
        auto admissionResult = coordinator.admit();
        if (!admissionResult) {
            FrameExportApprovalResultV1 result(FrameExportApprovalStatusV1::AdmissionFailed);
            result.admissionFailure_ = admissionResult.status();
            return result;
        }
        admission.emplace(std::move(admissionResult).takeAdmission());
    }

    auto registrationResult =
        coordinator.registerTarget(std::move(*admission), attempt->target().targetKey);
    if (!registrationResult) {
        FrameExportApprovalResultV1 result(FrameExportApprovalStatusV1::RegistrationFailed);
        result.registrationFailure_ = registrationResult.status();
        return result;
    }
    auto claim = std::move(registrationResult).takeClaim();

    try {
        auto request = std::make_unique<FrameExportRequestV1>(
            FrameExportRequestV1(std::move(attempt), std::move(claim), limits));
        return FrameExportApprovalResultV1(std::move(request));
    } catch (const std::bad_alloc&) {
        // The claim (and, transitively, its target-claim record) abandons via RAII on this path;
        // nothing else was published or entered.
        return FrameExportApprovalResultV1(FrameExportApprovalStatusV1::AdmissionFailed);
    }
}

FrameExportPublicationResultV1
FrameExportPublicationResultV1::published(platform::StagedArtifactPublicationResult publication,
                                          const PublicationIntentId intentId,
                                          const std::optional<core::Sha256Digest> semanticDigest,
                                          const std::optional<core::Sha256Digest> artifactDigest,
                                          const std::uint64_t artifactByteCount) noexcept {
    FrameExportPublicationResultV1 result;
    result.publication_ = publication;
    result.intentId_ = intentId;
    result.semanticDigest_ = semanticDigest;
    result.artifactDigest_ = artifactDigest;
    result.artifactByteCount_ = artifactByteCount;
    return result;
}

FrameExportPublicationResultV1 FrameExportPublicationResultV1::failure(
    FrameExportPublicationFailureV1 failureValue,
    const std::optional<platform::StagedArtifactError> rejectDiagnostic) noexcept {
    FrameExportPublicationResultV1 result;
    result.failure_.emplace(failureValue); // trivially copyable; std::move() would be a no-op
    result.rejectDiagnostic_ = rejectDiagnostic;
    return result;
}

namespace {

[[nodiscard]] std::filesystem::path
uniqueScratchFilePath(const std::filesystem::path& directory,
                      const std::string_view extension) noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const auto suffix = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return directory / (".bloom-export-scratch-" + std::to_string(now) + "-" +
                        std::to_string(suffix) + std::string(extension));
}

// The doc's ordered stage vocabulary, mapped onto the existing TaskContext::reportProgress()
// phase strings (frame-output.md, "Non-Blocking Execution"). EXR only ever produces Writing,
// Verifying, and Publishing here -- exactly the three the previous inline ternary produced.
[[nodiscard]] std::string exportStagePhase(const output::OutputExportStageV1 stage) {
    switch (stage) {
    case output::OutputExportStageV1::Resolving:
        return "Resolving";
    case output::OutputExportStageV1::Evaluating:
        return "Evaluating";
    case output::OutputExportStageV1::Identifying:
        return "Identifying";
    case output::OutputExportStageV1::ColorPreparing:
        return "ColorPreparing";
    case output::OutputExportStageV1::Analyzing:
        return "Analyzing";
    case output::OutputExportStageV1::PreparingOutput:
        return "PreparingOutput";
    case output::OutputExportStageV1::Writing:
        return "Writing";
    case output::OutputExportStageV1::Verifying:
        return "Verifying";
    case output::OutputExportStageV1::Publishing:
        break;
    }
    return "Publishing";
}

// The preset-independent shape of "the staged bytes exist at the scratch path and carry these two
// digests", so the publication half below stays one code path for both presets.
struct WriteVerifyOutcomeV1 final {
    bool written = false;
    bool cancelled = false;
    FrameExportPublicationStageV1 stage = FrameExportPublicationStageV1::Writing;
    FrameExportPublicationFailurePayloadV1 payload{};
    core::Sha256Digest semanticDigest;
    core::Sha256Digest artifactDigest;
    std::uint64_t artifactByteCount = 0;
};

// Maps the PNG bridge's typed error onto the publication stage it happened at. ColorPreparing and
// PreparingOutput both belong to the doc's node 2 ("dependent output preparation"); the remaining
// codes belong to node 3's writer/verifier halves.
[[nodiscard]] FrameExportPublicationStageV1
pngFailureStage(const output::PngExportWriteErrorCodeV1 error) noexcept {
    switch (error) {
    case output::PngExportWriteErrorCodeV1::MissingDisplayProducts:
    case output::PngExportWriteErrorCodeV1::PreparedBytesLimitExceeded:
    case output::PngExportWriteErrorCodeV1::ColorPrepareFailed:
        return FrameExportPublicationStageV1::ColorPreparing;
    case output::PngExportWriteErrorCodeV1::VerifyFailed:
        return FrameExportPublicationStageV1::Verifying;
    case output::PngExportWriteErrorCodeV1::None:
    case output::PngExportWriteErrorCodeV1::InvalidAttempt:
    case output::PngExportWriteErrorCodeV1::WriteFailed:
    case output::PngExportWriteErrorCodeV1::ArtifactHashFailed:
    case output::PngExportWriteErrorCodeV1::InternalInvariant:
        break;
    }
    return FrameExportPublicationStageV1::Writing;
}

class ScratchFileGuard final {
  public:
    explicit ScratchFileGuard(std::filesystem::path path) noexcept : path_(std::move(path)) {}
    ScratchFileGuard(const ScratchFileGuard&) = delete;
    ScratchFileGuard& operator=(const ScratchFileGuard&) = delete;
    ~ScratchFileGuard() {
        std::error_code errorCode;
        std::filesystem::remove(path_, errorCode);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] FrameExportPublicationResultV1
cancelledResult(const FrameExportPublicationStageV1 /*lastStage*/) noexcept {
    // The stage a cancellation checkpoint was observed at is not separately distinguished --
    // mirrors SavePublicationStage::Cancelled's identical established rationale (every checkpoint
    // here never calls lease.publish(), so the caller-observable contract, "no replacement
    // occurred", is identical at all of them).
    return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
        FrameExportPublicationStageV1::Cancelled, std::monostate{}));
}

} // namespace

FrameExportPublicationResultV1 executeExportPublication(
    runtime::TaskContext& context, platform::StagedArtifactCoordinator& artifacts,
    FrameExportRequestV1 request, const std::filesystem::path& scratchDirectory,
    output::OutputExportClockV1 clock, output::OutputExportProgressCallbackV1 progress) noexcept {
    if (!clock) {
        clock = output::systemOutputExportClockV1();
    }
    const auto start = clock();
    auto lastProgress = start;
    const auto& limits = request.limits();

    const auto deadlineExceeded = [&](const std::chrono::steady_clock::time_point now) noexcept {
        return now - start > limits.totalDeadline;
    };
    const auto noProgressExceeded = [&](const std::chrono::steady_clock::time_point now) noexcept {
        return now - lastProgress > limits.noProgressInterval;
    };

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Preflight);
    }

    const auto& attempt = *request.attempt();
    const auto& target = attempt.target();

    // --- Preflight (design decision 4 node 1: "CPU output preflight validates the retained
    // attempt/request binding and checked aggregate resources without reevaluating the
    // composition or rehashing the process frame") -- revalidates the SAME platform preflight the
    // attempt's own Resolving stage already ran, reusing artifacts.preflight() rather than
    // reimplementing any of the checks it owns.
    auto preflightResult = artifacts.preflight({.targetPath = target.targetPath,
                                                .overwritePolicy = target.overwritePolicy,
                                                .expectedTarget = target.observation});
    if (!preflightResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Preflight, preflightResult.error()));
    }
    auto platformTarget = std::move(preflightResult).takeTarget();
    if (platformTarget.targetKey() != target.targetKey) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Preflight, FrameExportUnexpectedFailureV1{}));
    }

    // PNG's prepared straight-RGBA8 stream is the one buffer the approved job adds on top of the
    // attempt's already-charged retained products, so it is both checked against its own closed
    // limit and folded into the expanded reservation below -- BEFORE anything is allocated or any
    // file is created (frame-output.md: "An insufficient or overflowing reservation rejects the
    // stage before allocation or file creation"). EXR exposes the retained rows directly and adds
    // nothing here, which is why its peak arithmetic below is unchanged.
    const bool isPng = attempt.preset() == output::OutputPresetV1::PngRgba8SrgbV1;
    std::uint64_t preparedBytes = 0;
    if (isPng) {
        const auto counted = output::checkedPngPreparedByteCountV1(attempt);
        if (!counted.has_value()) {
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::Preflight, FrameExportUnexpectedFailureV1{}));
        }
        const auto preparedLimit =
            std::min(limits.preparedPngByteLimit, output::kOutputExportPreparedPngBytesMaximumV1);
        if (preparedLimit == 0 || *counted > preparedLimit) {
            return FrameExportPublicationResultV1::failure(
                FrameExportPublicationFailureV1(FrameExportPublicationStageV1::ColorPreparing,
                                                FrameExportPreparedBytesExceededV1{}));
        }
        preparedBytes = *counted;
    }

    // Approved-job admission transactionally expands the SAME reservation the attempt already
    // holds (design decision 5: "Approved-job admission transactionally expands that reservation
    // to the checked peak ... it neither double-charges shared retained storage") -- the checked
    // peak here is the retained bytes already charged, plus PNG's prepared display/output pixels
    // (zero for EXR), plus one streaming chunk of encoder/verifier/copy scratch. The attempt's own
    // retained storage is never re-added, so nothing is double-charged.
    const auto currentCharge = attempt.resources()->chargedBytes();
    constexpr auto chunkBound =
        static_cast<std::uint64_t>(output::kOutputAdapterMaximumStreamingChunkBytesV1);
    constexpr auto maximumBytes = std::numeric_limits<std::uint64_t>::max();
    auto peak =
        currentCharge > maximumBytes - preparedBytes ? maximumBytes : currentCharge + preparedBytes;
    peak = peak > maximumBytes - chunkBound ? maximumBytes : peak + chunkBound;
    if (attempt.resources()->expand(peak) != output::ExportResourceAdmissionStatusV1::Reserved) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Preflight, FrameExportResourceExhaustedV1{}));
    }

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Preflight);
    }

    // --- Staging.
    auto stageResult = artifacts.stage(std::move(platformTarget));
    if (!stageResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Staging, stageResult.error()));
    }
    auto lease = std::move(stageResult).takeLease();

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Staging);
    }

    // --- Writing + Verifying (design decision 4 node 3's first half): F1's writer/verifier
    // against a caller-owned scratch path bridging their path-based OpenEXR API to the lease's
    // byte-oriented staging (see bloom/output/flat_exr_export_write.hpp's own namespace comment
    // for the full rationale). The bridging progress callback also resets the no-progress clock;
    // a mid-call deadline/no-progress violation can only be detected once F1's own (synchronous,
    // uninterruptible from here) call returns -- runtime::CancellationToken exposes no
    // caller-side "request my own cancellation" seam, and changing task_scheduler.hpp is out of
    // this task's scope. See the implementor's report for the full limitation.
    const ScratchFileGuard scratch(
        uniqueScratchFilePath(scratchDirectory, isPng ? ".png" : ".exr"));
    const auto stageProgressSink = [&](const output::OutputExportProgressV1& stageProgress) {
        lastProgress = clock();
        context.reportProgress({.phase = exportStagePhase(stageProgress.stage),
                                .subphase = "",
                                .completed = stageProgress.completed,
                                .total = stageProgress.total});
        if (progress) {
            progress(stageProgress);
        }
    };

    WriteVerifyOutcomeV1 writeVerify;
    if (isPng) {
        const output::PngExportWriterV1 pngWriter;
        const auto pngResult = pngWriter.run(attempt, scratch.path(), context.cancellation(),
                                             stageProgressSink, limits.preparedPngByteLimit);
        writeVerify.cancelled = pngResult.status() == output::PngExportWriteStatusV1::Cancelled;
        writeVerify.written = pngResult.status() == output::PngExportWriteStatusV1::Written;
        if (writeVerify.written) {
            writeVerify.semanticDigest = pngResult.semanticDigest();
            writeVerify.artifactDigest = pngResult.artifactDigest();
            writeVerify.artifactByteCount = pngResult.artifactByteCount();
        } else if (!writeVerify.cancelled) {
            writeVerify.stage = pngFailureStage(pngResult.error());
            writeVerify.payload = pngResult.error();
        }
    } else {
        const output::FlatExrExportWriterV1 writer;
        const auto exrResult =
            writer.run(attempt, scratch.path(), context.cancellation(), stageProgressSink);
        writeVerify.cancelled = exrResult.status() == output::FlatExrExportWriteStatusV1::Cancelled;
        writeVerify.written = exrResult.status() == output::FlatExrExportWriteStatusV1::Written;
        if (writeVerify.written) {
            writeVerify.semanticDigest = exrResult.semanticDigest();
            writeVerify.artifactDigest = exrResult.artifactDigest();
            writeVerify.artifactByteCount = exrResult.artifactByteCount();
        } else if (!writeVerify.cancelled) {
            writeVerify.stage =
                exrResult.error() == output::FlatExrExportWriteErrorCodeV1::WriteFailed
                    ? FrameExportPublicationStageV1::Writing
                    : FrameExportPublicationStageV1::Verifying;
            writeVerify.payload = exrResult.error();
        }
    }

    const auto afterWriteVerify = clock();
    if (deadlineExceeded(afterWriteVerify)) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Writing, FrameExportDeadlineExceededV1{}));
    }
    if (noProgressExceeded(afterWriteVerify)) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Writing, FrameExportNoProgressExceededV1{}));
    }
    if (writeVerify.cancelled) {
        return cancelledResult(FrameExportPublicationStageV1::Writing);
    }
    if (!writeVerify.written) {
        return FrameExportPublicationResultV1::failure(
            FrameExportPublicationFailureV1(writeVerify.stage, writeVerify.payload));
    }

    // --- Artifact copy (design decision 4 node 3's "artifact SHA-256 + flush"): stream the
    // verified scratch bytes into the lease's own exclusive staged file, checking cancellation
    // and this stage's deadline/no-progress interval BETWEEN chunks -- the one segment of this
    // pipeline this code fully owns and can therefore genuinely preempt.
    {
        std::ifstream scratchStream(scratch.path(), std::ios::binary);
        if (!scratchStream) {
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
        }
        std::vector<std::byte> buffer(output::kOutputAdapterMaximumStreamingChunkBytesV1);
        while (scratchStream) {
            const auto now = clock();
            if (context.isCancellationRequested()) {
                return cancelledResult(FrameExportPublicationStageV1::ArtifactCopy);
            }
            if (deadlineExceeded(now)) {
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy, FrameExportDeadlineExceededV1{}));
            }
            if (noProgressExceeded(now)) {
                return FrameExportPublicationResultV1::failure(
                    FrameExportPublicationFailureV1(FrameExportPublicationStageV1::ArtifactCopy,
                                                    FrameExportNoProgressExceededV1{}));
            }
            scratchStream.read(reinterpret_cast<char*>(buffer.data()),
                               static_cast<std::streamsize>(buffer.size()));
            const auto readCount = static_cast<std::size_t>(scratchStream.gcount());
            if (readCount == 0) {
                break;
            }
            const auto writeResult = lease.write(std::span(buffer).first(readCount));
            if (!writeResult) {
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy, writeResult.error));
            }
            lastProgress = clock();
        }
        if (scratchStream.bad()) {
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
        }
    }

    const auto finishResult = lease.finishWriting();
    if (!finishResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::ArtifactCopy, finishResult.error));
    }

    // Read the lease's own staged bytes back and recompute their SHA-256, proving the exclusive
    // staged file the coordinator will atomically publish is byte-identical to what F1 verified --
    // "F1 verifier on the staged identity (Verifying)" extended to the file that is actually
    // published, not only the bridging scratch copy.
    {
        core::Sha256Hasher rehasher;
        std::vector<std::byte> buffer(output::kOutputAdapterMaximumStreamingChunkBytesV1);
        std::uint64_t offset = 0;
        while (true) {
            const auto now = clock();
            if (context.isCancellationRequested()) {
                static_cast<void>(lease.rejectVerification());
                return cancelledResult(FrameExportPublicationStageV1::ArtifactCopy);
            }
            if (deadlineExceeded(now) || noProgressExceeded(now)) {
                static_cast<void>(lease.rejectVerification());
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy,
                    deadlineExceeded(now)
                        ? FrameExportPublicationFailurePayloadV1(FrameExportDeadlineExceededV1{})
                        : FrameExportPublicationFailurePayloadV1(
                              FrameExportNoProgressExceededV1{})));
            }
            const auto readResult = lease.readForVerification(offset, std::span(buffer));
            if (!readResult) {
                static_cast<void>(lease.rejectVerification());
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy, readResult.error));
            }
            if (readResult.bytesRead > 0 && !rehasher.update(std::span(buffer).first(
                                                static_cast<std::size_t>(readResult.bytesRead)))) {
                static_cast<void>(lease.rejectVerification());
                return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                    FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
            }
            offset += readResult.bytesRead;
            lastProgress = clock();
            if (readResult.endOfFile) {
                break;
            }
        }
        if (rehasher.finalize() != writeVerify.artifactDigest) {
            static_cast<void>(lease.rejectVerification());
            return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
                FrameExportPublicationStageV1::ArtifactCopy, FrameExportUnexpectedFailureV1{}));
        }
    }

    const auto acceptResult = lease.acceptVerification();
    if (!acceptResult) {
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::ArtifactCopy, acceptResult.error));
    }

    if (context.isCancellationRequested()) {
        return cancelledResult(FrameExportPublicationStageV1::Guard);
    }

    // --- Guard + Publishing: identical composition to executeCopyPublication()'s own final
    // switch (see its declaration comment for the full unreachability argument for
    // InvalidClaim/TargetBusy/AlreadyEntered in a single-executor composition).
    auto& claim = FrameExportRequestAccessV1::claim(request);
    auto guardResult = claim.tryEnterPublication();
    const auto intentId = claim.intentId();
    switch (guardResult.status()) {
    case PublicationGuardStatus::Entered: {
        auto guard = std::move(guardResult).takeGuard();
        auto publication = lease.publish(platform::PublicationDisposition::Proceed);
        return FrameExportPublicationResultV1::published(
            publication, intentId, writeVerify.semanticDigest, writeVerify.artifactDigest,
            writeVerify.artifactByteCount);
    }
    case PublicationGuardStatus::Superseded:
        return FrameExportPublicationResultV1::published(
            lease.publish(platform::PublicationDisposition::Superseded), intentId, std::nullopt,
            std::nullopt, 0);
    case PublicationGuardStatus::InvalidClaim:
    case PublicationGuardStatus::TargetBusy:
    case PublicationGuardStatus::AlreadyEntered:
        return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
            FrameExportPublicationStageV1::Guard, guardResult.status()));
    }
    return FrameExportPublicationResultV1::failure(FrameExportPublicationFailureV1(
        FrameExportPublicationStageV1::Guard, guardResult.status()));
}

} // namespace bloom::host
