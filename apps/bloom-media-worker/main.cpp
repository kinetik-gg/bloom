#include "fake_provider.hpp"
#include "worker_io.hpp"
#include <bloom/platform/process_supervisor.hpp>

int main() {
    using namespace bloom::media;
    if (!bloom::platform::processWorkerBootstrap())
        return 2;
    try {
        std::uint64_t session = 0, sequence = 0;
        bool ready = false;
        while (true) {
            auto incoming = worker::read();
            const auto* message = std::get_if<provider::Message>(&incoming);
            if (!message)
                return 3;
            if (message->sequence != sequence + 1 || (ready && message->session != session))
                return 4;
            session = message->session;
            sequence = message->sequence;
            if (message->kind == provider::MessageKind::Cancel ||
                message->kind == provider::MessageKind::Shutdown)
                return worker::write(
                           {provider::MessageKind::Ack, session, sequence, std::monostate{}})
                           ? 0
                           : 5;
            if (!ready) {
                if (message->kind != provider::MessageKind::Handshake ||
                    std::get<provider::Handshake>(message->payload) != fake::handshake())
                    return 6;
                if (!worker::write(
                        {provider::MessageKind::Handshake, session, sequence, fake::handshake()}))
                    return 7;
                ready = true;
                continue;
            }
            if (message->kind != provider::MessageKind::Call)
                return 8;
            auto payload = fake::call(std::get<provider::CallRequest>(message->payload));
            auto kind = provider::MessageKind::Failure;
            if (std::holds_alternative<provider::ProbeResult>(payload))
                kind = provider::MessageKind::Probe;
            if (std::holds_alternative<provider::FrameProduct>(payload))
                kind = provider::MessageKind::Frame;
            if (!worker::write({kind, session, sequence, std::move(payload)}))
                return 9;
        }
    } catch (const std::exception&) {
        return 10;
    }
}
