#include "fake_provider.hpp"
#include "worker_io.hpp"
#include <bloom/platform/process_supervisor.hpp>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <poll.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
void raw(const bloom::media::provider::Bytes& bytes) {
    std::cout.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    std::cout.flush();
}
} // namespace
int main(int argc, char** argv) {
    try {
        using namespace bloom::media;
        if (!bloom::platform::processWorkerBootstrap() || argc < 2)
            return 2;
        const std::string mode = argv[1];
        auto incoming = worker::read();
        const auto* hello = std::get_if<provider::Message>(&incoming);
        if (!hello)
            return 3;
        auto h = fake::handshake();
        if (mode == "lock")
            h.execution.dependencyLock = provider::digestBytes(std::as_bytes(std::span(mode)));
        for (auto& d : h.declarations)
            d.execution = h.execution;
        auto helloBytes = std::get<provider::Bytes>(
            provider::encodeMessage({provider::MessageKind::Handshake, hello->session, 1, h}));
        if (mode == "version")
            helloBytes[8] = std::byte{2};
        raw(helloBytes);
        auto call = worker::read();
        const auto* request = std::get_if<provider::Message>(&call);
        if (!request)
            return 4;
        if (argc >= 3) {
            std::ofstream marker(argv[2]);
            marker << "call-received";
        }
        if (mode == "hang" || mode == "kill") {
            std::signal(SIGTERM, SIG_IGN);
            for (;;)
                ::poll(nullptr, 0, 1000);
        }
        if (mode == "crash") {
            ::raise(SIGKILL);
            return 5;
        }
        if (mode == "cancel") {
            auto stop = worker::read();
            const auto* m = std::get_if<provider::Message>(&stop);
            if (!m || m->kind != provider::MessageKind::Cancel)
                return 6;
            if (argc >= 3) {
                std::ofstream marker(argv[2]);
                marker << "cancel-received";
            }
            return 0;
        }
        if (mode == "term") {
            for (;;)
                ::poll(nullptr, 0, 1000);
        }
        if (mode == "limits") {
            if (std::getenv("HOME") || std::getenv("LD_LIBRARY_PATH") || std::getenv("LD_PRELOAD"))
                return 11;
            rlimit address{}, files{}, core{};
            if (::getrlimit(RLIMIT_AS, &address) != 0 || ::getrlimit(RLIMIT_NOFILE, &files) != 0 ||
                ::getrlimit(RLIMIT_CORE, &core) != 0 ||
                address.rlim_cur != 2ULL * 1024 * 1024 * 1024 || files.rlim_cur != 64 ||
                core.rlim_cur != 0)
                return 7;
        }
        if (mode == "replay") {
            raw(helloBytes);
            return 0;
        }
        auto payload = fake::call(std::get<provider::CallRequest>(request->payload));
        const auto kind = std::holds_alternative<provider::FrameProduct>(payload)
                              ? provider::MessageKind::Frame
                              : provider::MessageKind::Probe;
        auto bytes = std::get<provider::Bytes>(
            provider::encodeMessage({kind, hello->session, 2, std::move(payload)}));
        if (mode == "truncated")
            bytes.resize(bytes.size() - 1);
        if (mode == "oversized")
            for (std::size_t i = 0; i < 4; ++i)
                bytes[i] = std::byte{255};
        if (mode == "enum")
            bytes[12] = std::byte{255};
        if (mode == "length") {
            bytes[29] = std::byte{255};
            bytes[30] = std::byte{15};
        }
        if (mode == "digest")
            bytes.back() ^= std::byte{1};
        raw(bytes);
        if (mode != "limits")
            return 0;
        auto shutdown = worker::read();
        const auto* m = std::get_if<provider::Message>(&shutdown);
        if (!m || m->kind != provider::MessageKind::Shutdown)
            return 8;
        return worker::write(
                   {provider::MessageKind::Ack, m->session, m->sequence, std::monostate{}})
                   ? 0
                   : 9;
    } catch (const std::exception&) {
        return 10;
    }
}
