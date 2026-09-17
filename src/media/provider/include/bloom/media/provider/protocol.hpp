#pragma once
#include <bloom/media/provider/registry.hpp>

namespace bloom::media::provider {
inline constexpr std::uint16_t kProtocolVersion = 1, kSchemaVersion = 1;
inline constexpr std::uint32_t kEnvelopeBytes = 25;
enum class MessageKind : std::uint8_t {
    Handshake = 1,
    Call = 2,
    Probe = 3,
    Frame = 4,
    Cancel = 5,
    Shutdown = 6,
    Ack = 7,
    Failure = 8
};
struct Handshake {
    ProviderExecutionKeyV1 execution;
    std::vector<ProviderDeclaration> declarations;
    std::vector<Digest> pipelines;
    std::vector<Transport> transports;
    friend bool operator==(const Handshake&, const Handshake&) = default;
};
struct CallRequest {
    MediaCapabilityKeyV1 capability;
    std::string source;
    std::uint32_t width = 1, height = 1;
    std::uint64_t frame = 0;
};
using Payload =
    std::variant<Handshake, CallRequest, ProbeResult, FrameProduct, std::monostate, Unavailable>;
struct Message {
    MessageKind kind = MessageKind::Ack;
    std::uint64_t session = 0, sequence = 0;
    Payload payload = std::monostate{};
};
[[nodiscard]] Result<Bytes> encodeMessage(const Message& message);
// Includes the u32 frame length. Validates the entire frame before publishing a value.
[[nodiscard]] Result<Message> decodeMessage(std::span<const std::byte> frame);
[[nodiscard]] Result<std::uint32_t> messageLength(std::span<const std::byte> prefix);
// Host-side stateful validation; rejected frames cannot advance session state.
class HostProtocol final {
  public:
    HostProtocol(std::uint64_t session, Handshake expected)
        : session_(session), expected_(std::move(expected)) {}
    [[nodiscard]] Result<Message> receive(std::span<const std::byte> frame,
                                          MessageKind expectedKind);
    [[nodiscard]] const Handshake& expectedHandshake() const { return expected_; }

  private:
    std::uint64_t session_ = 0, lastSequence_ = 0;
    Handshake expected_;
    bool handshaken_ = false;
};
} // namespace bloom::media::provider
