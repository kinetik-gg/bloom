#pragma once
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

namespace bloom::output {
struct CompositionOutputSourceV1 {
    document::Snapshot snapshot;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    core::RationalTime origin;
    std::filesystem::path assetBaseDirectory;
};
// Blocking output adapter. Owns the isolated encoder and offline mix clients. The caller
// serializes calls on an I/O executor and retains the admission reservation through close.
class CompositionOutputStreamV1 final {
  public:
    CompositionOutputStreamV1(CompositionOutputSourceV1, media::provider::EncodeSettingsV1,
                              media::provider::EncodeSessionOptionsV1,
                              std::shared_ptr<ExportResourceReservationV1>,
                              platform::ProcessCancellation);
    ~CompositionOutputStreamV1();
    CompositionOutputStreamV1(const CompositionOutputStreamV1&) = delete;
    CompositionOutputStreamV1& operator=(const CompositionOutputStreamV1&) = delete;
    [[nodiscard]] std::optional<media::provider::Unavailable>
    writeFrame(const render::Rgba32fImage&, std::uint64_t frame, std::uint64_t samplesThrough,
               runtime::TaskContext&);
    [[nodiscard]] media::provider::Result<media::provider::EncodeQcV1> finish();
    [[nodiscard]] media::provider::Result<media::provider::EncodedChunkV1>
    read(std::uint64_t offset);
    [[nodiscard]] std::optional<media::provider::Unavailable> close();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
[[nodiscard]] media::provider::Result<media::provider::MediaQcEvidenceV1>
makeMediaQcEvidenceV1(const MediaOutputAnalysisV1&, const media::provider::EncodeQcV1&,
                      core::Sha256Digest approval);
} // namespace bloom::output
