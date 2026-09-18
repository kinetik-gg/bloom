#pragma once
#include <bloom/media/provider/protocol.hpp>
#include <bloom/platform/process_supervisor.hpp>
#include <optional>

namespace bloom::media::provider {
struct EncodeSessionOptionsV1 {
    std::string executable;
    std::chrono::milliseconds timeout{120000};
    std::function<void(std::int64_t)> launched;
};
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
    [[nodiscard]] std::optional<Unavailable> video(FrameProduct frame);
    [[nodiscard]] std::optional<Unavailable> audio(AudioBlock block);
    [[nodiscard]] Result<EncodeQcV1> finish();
    [[nodiscard]] Result<EncodedChunkV1> read(std::uint64_t offset);
    [[nodiscard]] std::optional<Unavailable> close();
    [[nodiscard]] const ProviderExecutionKeyV1& execution() const;
    [[nodiscard]] static std::string defaultWorker();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bloom::media::provider
