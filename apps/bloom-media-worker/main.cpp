#ifdef BLOOM_FAKE_WORKER
#include "fake_provider.hpp"
#else
#include "ffmpeg_provider.hpp"
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#endif
#include "worker_io.hpp"
#include <bloom/platform/process_supervisor.hpp>
#include <string_view>

int main(int argc, char** argv) {
    using namespace bloom::media;
    if (!bloom::platform::processWorkerBootstrap())
        return 2;
#ifdef BLOOM_FAKE_WORKER
    (void)argc;
    (void)argv;
    const auto handshake = fake::handshake();
#else
    const bool hardware = argc == 2 && std::string_view(argv[1]) == "--vaapi";
    const auto handshake = provider::ffmpegHandshake(hardware);
    ffmpeg::Encoder encoder;
#endif
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
                    std::get<provider::Handshake>(message->payload) != handshake)
                    return 6;
                if (!worker::write(
                        {provider::MessageKind::Handshake, session, sequence, handshake}))
                    return 7;
                ready = true;
                continue;
            }
            if (message->kind != provider::MessageKind::Call &&
                message->kind != provider::MessageKind::EncodeBegin &&
                message->kind != provider::MessageKind::Frame &&
                message->kind != provider::MessageKind::Audio &&
                message->kind != provider::MessageKind::EncodeFinish &&
                message->kind != provider::MessageKind::EncodeRead)
                return 8;
#ifdef BLOOM_FAKE_WORKER
            if (message->kind != provider::MessageKind::Call)
                return 8;
            auto payload = fake::call(std::get<provider::CallRequest>(message->payload));
#else
            auto payload =
                message->kind == provider::MessageKind::Call
                    ? ffmpeg::call(std::get<provider::CallRequest>(message->payload), hardware)
                    : (hardware
                           ? provider::Payload(provider::Unavailable{
                                 provider::Error::Unavailable, "Hardware encode is not qualified"})
                           : encoder.call(message->kind, message->payload));
#endif
            auto kind = provider::MessageKind::Failure;
            if (std::holds_alternative<provider::ProbeResult>(payload))
                kind = provider::MessageKind::Probe;
            if (std::holds_alternative<provider::FrameProduct>(payload))
                kind = provider::MessageKind::Frame;
            if (std::holds_alternative<provider::DemuxIndex>(payload))
                kind = provider::MessageKind::Index;
            if (std::holds_alternative<provider::AudioBlock>(payload))
                kind = provider::MessageKind::Audio;
            if (std::holds_alternative<std::monostate>(payload))
                kind = provider::MessageKind::Ack;
            if (std::holds_alternative<provider::EncodeQcV1>(payload))
                kind = provider::MessageKind::EncodeQc;
            if (std::holds_alternative<provider::EncodedChunkV1>(payload))
                kind = provider::MessageKind::EncodedChunk;
            if (!worker::write({kind, session, sequence, std::move(payload)}))
                return 9;
        }
    } catch (const std::exception&) {
        return 10;
    }
}
