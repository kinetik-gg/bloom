#pragma once
#include <bloom/media/provider/protocol.hpp>
#include <bloom/media/provider/provider_chain.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <optional>

namespace bloom::media::provider {
struct EncodeSessionOptionsV1 {
    // The FFmpeg worker. Empty selects the installed worker beside the application, else this
    // build tree's worker. The platform media framework's worker is always located by the host.
    std::string executable;
    bool vaapi = false;
    std::chrono::milliseconds timeout{120000};
    std::function<void(std::int64_t)> launched;
    // ADR 0033 section 16: the host's staged artifact. The worker writes the container into it
    // and verifies it in place; the host then adopts it instead of reading chunks. Linux FFmpeg
    // routes only; a platform media framework route ignores it.
    std::shared_ptr<const platform::StagedArtifactInheritableWriter> stagedOutput;
};
// begin() selects the provider through the platform's encode chain (ADR 0035): the platform media
// framework first, then the bundled FFmpeg worker (Linux: FFmpeg with VA-API when requested, then
// software). A provider that is missing, does not declare the codec/container, or refuses the
// launch, handshake or EncodeBegin is skipped silently and recorded in provenance(). Once begin()
// succeeds the provider is pinned for the whole attempt.
// BlockingIo-only, one request/ack at a time. A failure poisons this session and reaps its worker.
// Destruction cancels unfinished work. Neither worker nor client can publish a destination.
class EncodeSessionV1 final {
  public:
    explicit EncodeSessionV1(EncodeSessionOptionsV1 options,
                             platform::ProcessCancellation cancellation = {});
    ~EncodeSessionV1();
    EncodeSessionV1(const EncodeSessionV1&) = delete;
    EncodeSessionV1& operator=(const EncodeSessionV1&) = delete;
    [[nodiscard]] std::optional<Unavailable> begin(const EncodeSettingsV1& settings);
    // Frames and audio blocks are written in order without waiting for the worker's
    // acknowledgement (a bounded number stay unread); a refused product surfaces as the error of a
    // later call, and finish/read/close first collect every outstanding acknowledgement.
    [[nodiscard]] std::optional<Unavailable> video(FrameProduct frame);
    [[nodiscard]] std::optional<Unavailable> audio(AudioBlock block);
    [[nodiscard]] Result<EncodeQcV1> finish();
    [[nodiscard]] Result<EncodedChunkV1> read(std::uint64_t offset);
    [[nodiscard]] std::optional<Unavailable> close();
    [[nodiscard]] const ProviderExecutionKeyV1& execution() const;
    // The serving provider's handshake and the selection provenance (diagnostics only).
    [[nodiscard]] const Handshake& handshake() const;
    [[nodiscard]] const MediaProviderProvenanceV1& provenance() const;
    [[nodiscard]] static std::string defaultWorker();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bloom::media::provider
