#pragma once
#include <bloom/host/frame_export_publication.hpp>
#include <bloom/host/frame_range_runner.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <functional>
#include <memory>

namespace bloom::host {
namespace detail {
struct SequenceExportWorkV1;
// The queue's checked byte envelope enters the same processing root as per-frame final output.
// Exposed for zero-allocation admission regressions; production calls it on the export worker.
[[nodiscard]] std::shared_ptr<output::ExportResourceReservationV1>
reserveSequenceExportQueueV1(output::ExportResourceLedgerV1&, std::uint64_t bytes,
                             std::optional<std::uint64_t> explicitJobLimit,
                             std::shared_ptr<GpuExportProvider>, const runtime::CancellationToken&,
                             output::ExportResourceAdmissionStatusV1&) noexcept;
}
class SequenceExportRunnerV1;

// One export-specific cleanup worker, owned by the application or a headless caller. A runner
// reserves one of four slots before it can start; its slot remains valid after beginShutdown(),
// so an admission refusal can still retire a live encoder without using TaskScheduler. The owner
// must outlive its runners and their borrowed dependencies. collect() joins only after the worker
// has reported exit and is intended for post-event-loop/headless teardown.
class SequenceExportRetirementOwnerV1 final {
  public:
    struct TestGate {
        // Deterministic lifecycle tests may hold the cleanup lane before it touches Work.
        // Runs on the retirement worker; production callers leave this empty.
        std::function<void()> beforeWorkCleanup;
    };
    static std::shared_ptr<SequenceExportRetirementOwnerV1> create(TestGate gate = {});
    ~SequenceExportRetirementOwnerV1();
    SequenceExportRetirementOwnerV1(const SequenceExportRetirementOwnerV1&) = delete;
    SequenceExportRetirementOwnerV1& operator=(const SequenceExportRetirementOwnerV1&) = delete;
    void beginShutdown() noexcept;
    [[nodiscard]] bool retirementComplete() const noexcept;
    [[nodiscard]] bool collect() noexcept;

  private:
    friend class SequenceExportRunnerV1;
    struct Ticket;
    struct State;
    explicit SequenceExportRetirementOwnerV1(TestGate gate);
    [[nodiscard]] std::unique_ptr<Ticket> reserve();
    void retire(std::unique_ptr<Ticket>, std::shared_ptr<detail::SequenceExportWorkV1>) noexcept;
    void release(std::unique_ptr<Ticket>) noexcept;
    std::unique_ptr<State> state_;
};

struct SequenceExportRequestV1 {
    runtime::SnapshotCompileRequest composition;
    FrameRangeRequestV1 range;
    output::OutputPresetV1 preset = output::OutputPresetV1::ProResMovV1;
    std::string profile = "hq";
    bool audio = true;
    bool hardware = false;
    bool openh264Consent = false;
    std::uint32_t sampleRate = 48000;
    std::string pcmCodec = "pcm_s16le", bwfDescription;
    std::filesystem::path assetBaseDirectory;
    media::provider::EncodeSessionOptionsV1 worker;
    std::optional<std::uint64_t> startFrame = {};
    std::uint32_t framePadding = 4;
    std::string namePattern = "<base>.####.<ext>";
    std::string workingColorSpaceId = std::string(runtime::kLinearRec709SceneColorSpaceId);
    core::Sha256Digest ocioConfigRevision = {};
    std::string ocioConfigUri = std::string(runtime::kBloomNeutralOcioConfigUri);
    std::string displayName = {}, viewName = {};
    // Explicit caller cap on retained queue and decoded audio claims. Omission uses sampled root
    // headroom; the provider protocol's separate frame/extent/index limits still apply.
    std::optional<std::uint64_t> queueByteLimit = std::nullopt;
    // Shared final-render provider for every frame of the sequence. Null creates one runner-owned,
    // disabled CPU provider with a worker-sampled processing root. The runner copies the exact
    // provider into every per-frame attempt; an injected application provider retains its shared
    // policy and independent retirement ownership.
    std::shared_ptr<GpuExportProvider> gpuProvider = nullptr;
};
enum class SequenceExportStageV1 : std::uint8_t {
    Compiling,
    Analyzing,
    AwaitingApproval,
    Encoding,
    Verifying,
    Complete
};
struct SequenceExportResultV1 {
    platform::StagedArtifactPublicationResult publication;
    std::optional<media::provider::Unavailable> failure;
    std::optional<media::provider::MediaQcEvidenceV1> evidence;
    std::uint64_t encodedFrames = 0;
    // Native provenance/counters accumulated over the per-frame output attempts (diagnostics
    // only). `gpuEvaluatedFrames` counts frames whose attempt actually ran the GPU bridge;
    // `gpuNativeDispatches` and `gpuReadbacks` are the summed per-attempt counters. Zero when no
    // provider was supplied or every frame fell back to the CPU reference path.
    std::uint64_t gpuEvaluatedFrames = 0;
    std::uint64_t gpuNativeDispatches = 0;
    // `gpuReadbacks` is the pre-existing compatibility name for the summed final readback
    // submissions (one per GPU-evaluated frame); `gpuReadbackSubmissions` is the explicit
    // combined-readback name. `gpuTransferredPayloads` is the summed distinct payload count (one
    // for the identity arm, two for the display arm) and the process/encoded byte totals are the
    // exact combined-readback bytes. Never derived from frame dimensions.
    std::uint64_t gpuReadbacks = 0;
    std::uint64_t gpuReadbackSubmissions = 0;
    std::uint64_t gpuTransferredPayloads = 0;
    std::uint64_t gpuProcessPayloadBytes = 0;
    std::uint64_t gpuEncodedPayloadBytes = 0;
    // Genuine native device ownership epoch observed on the GPU-evaluated frames (last nonzero
    // value; zero when no frame used a device). Diagnostics only.
    std::uint64_t gpuDeviceOwnershipEpoch = 0;
    [[nodiscard]] bool published() const noexcept { return publication.targetWasPublished(); }
};
// Authoring-thread driver. poll() never blocks: it composes the existing attempt/approval stages.
// One compile, exact SCRIPT-0 frame mapping, one acknowledged product at a time, one publication.
class SequenceExportRunnerV1 final {
  public:
    SequenceExportRunnerV1(runtime::TaskScheduler&, const runtime::SnapshotCompiler&,
                           PublicationCoordinator&, platform::StagedArtifactCoordinator&,
                           output::ExportResourceLedgerV1&, SequenceExportRequestV1,
                           SequenceExportRetirementOwnerV1&,
                           std::shared_ptr<output::ExportResourceLedgerV1> ledgerLifetime = {});
    ~SequenceExportRunnerV1();
    SequenceExportRunnerV1(const SequenceExportRunnerV1&) = delete;
    SequenceExportRunnerV1& operator=(const SequenceExportRunnerV1&) = delete;
    void poll();
    void cancel();
    [[nodiscard]] SequenceExportStageV1 stage() const;
    [[nodiscard]] const output::MediaOutputAnalysisV1* analysis() const;
    [[nodiscard]] std::optional<core::Sha256Digest> frameApprovalDigest() const;
    [[nodiscard]] bool approve(core::Sha256Digest mediaDigest, core::Sha256Digest frameDigest);
    [[nodiscard]] std::uint64_t encodedFrames() const;
    [[nodiscard]] std::uint64_t totalFrames() const;
    [[nodiscard]] const std::optional<SequenceExportResultV1>& result() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bloom::host
