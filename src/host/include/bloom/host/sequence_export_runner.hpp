#pragma once
#include <bloom/host/frame_export_publication.hpp>
#include <bloom/host/frame_range_runner.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

namespace bloom::host {
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
    std::uint64_t queueByteLimit = 512ULL * 1024U * 1024U;
    // Shared GPU final-render provider for every frame of the sequence. Null keeps the unchanged
    // CPU reference path. The runner copies this exact provider into each per-frame attempt, so the
    // evaluator stays alive across the whole range and an application-owned provider may retire
    // independently.
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
    std::uint64_t gpuReadbacks = 0;
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
                           output::ExportResourceLedgerV1&, SequenceExportRequestV1);
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
