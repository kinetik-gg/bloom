#include "support.hpp"
#include <bloom/media/provider/protocol.hpp>
using namespace bloom::media::provider;
namespace {
void rejected(const Bytes& bytes, Error error) {
    const auto result = decodeMessage(bytes);
    test::check(std::holds_alternative<Unavailable>(result) &&
                    std::get<Unavailable>(result).reason == error,
                "typed hostile-frame rejection");
}
void run() {
    Handshake h{test::execution(),
                {{test::capability(), test::execution(), test::evidence()}},
                {},
                {Transport::PipeCopiesV0}};
    const auto bytes = std::get<Bytes>(encodeMessage({MessageKind::Handshake, 42, 1, h}));
    // Every handshake byte is either framing or bound identity. Mutating any bit must be
    // rejected without advancing the receiver, including otherwise well-formed string changes.
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto changed = bytes;
            changed[index] ^= static_cast<std::byte>(1U << bit);
            HostProtocol validation(42, h);
            test::check(std::holds_alternative<Unavailable>(
                            validation.receive(changed, MessageKind::Handshake)),
                        "mutated handshake rejected");
            test::check(
                std::holds_alternative<Message>(validation.receive(bytes, MessageKind::Handshake)),
                "rejected handshake leaves state unchanged");
        }
    }
    HostProtocol host(42, h);
    test::check(std::holds_alternative<Message>(host.receive(bytes, MessageKind::Handshake)),
                "host handshake");
    test::check(std::get<Unavailable>(host.receive(bytes, MessageKind::Handshake)).reason ==
                    Error::Replay,
                "replayed frame");
    for (std::size_t size = 0; size < bytes.size(); ++size)
        test::check(
            std::holds_alternative<Unavailable>(decodeMessage(std::span(bytes).first(size))),
            "every truncation rejected");
    auto bad = bytes;
    bad[0] = std::byte{255};
    bad[1] = std::byte{255};
    bad[2] = std::byte{255};
    bad[3] = std::byte{255};
    rejected(bad, Error::Oversized);
    bad = bytes;
    bad[12] = std::byte{255};
    rejected(bad, Error::BadEnum);
    bad = bytes;
    bad[8] = std::byte{2};
    rejected(bad, Error::VersionMismatch);
    bad = bytes;
    bad[29] = std::byte{255};
    bad[30] = std::byte{15};
    rejected(bad, Error::BadLength);
    bad = bytes;
    bad.push_back(std::byte{0});
    rejected(bad, Error::BadLength);
    auto different = h;
    different.execution.dependencyLock = digestBytes(bytes);
    HostProtocol mismatch(42, different);
    test::check(std::get<Unavailable>(mismatch.receive(bytes, MessageKind::Handshake)).reason ==
                    Error::IdentityMismatch,
                "lock digest mismatch");
    HostProtocol otherSession(43, h);
    test::check(std::get<Unavailable>(otherSession.receive(bytes, MessageKind::Handshake)).reason ==
                    Error::Replay,
                "cross-session replay");
    FrameProduct frame;
    CpuPlane plane;
    plane.width = 1;
    plane.height = 1;
    plane.stride = 4;
    plane.bytes = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    plane.digest = digestBytes(plane.bytes);
    frame.planes = {plane};
    auto encoded = std::get<Bytes>(encodeMessage({MessageKind::Frame, 42, 2, frame}));
    test::check(std::get<FrameProduct>(
                    std::get<Message>(host.receive(encoded, MessageKind::Frame)).payload) == frame,
                "digest-verified copied plane");
    auto excessivePlanes = encoded;
    excessivePlanes[62] = std::byte{5};
    rejected(excessivePlanes, Error::Oversized);
    encoded.back() ^= std::byte{1};
    rejected(encoded, Error::DigestMismatch);
}
} // namespace
int main() {
    try {
        run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
