#include <algorithm>
#include <array>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/media/provider/ffmpeg_launch.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/media_workers.hpp>
#include <bloom/media/provider/openh264_runtime.hpp>
#include <filesystem>
#include <random>

namespace bloom::media::provider {
namespace {
// One provider route of the encode chain (ADR 0035): a worker generation, its launch arguments
// and the handshake the host expects from it.
struct EncodeRoute final {
    std::string id;
    bool native = false;
    std::string executable;
    std::vector<std::string> arguments;
    Handshake hello;
    std::string openh264;
};
// The provider's own spelling of the requested codec family ("prores" for VideoToolbox,
// "prores_ks" for FFmpeg). Settings for a codec the provider does not declare stay unchanged.
EncodeSettingsV1 providerSettings(EncodeSettingsV1 settings, const Handshake& hello) {
    if (settings.videoCodec.empty())
        return settings;
    const auto family = mediaCodecFamilyV1(settings.videoCodec);
    for (const auto& declaration : hello.declarations)
        if (declaration.capability.role == Role::VideoEncode &&
            mediaCodecFamilyV1(declaration.capability.codec) == family) {
            settings.videoCodec = declaration.capability.codec;
            break;
        }
    return settings;
}
Unavailable processFailure(const platform::ProcessFailure& e) {
    switch (e.code) {
    case platform::ProcessError::Unavailable:
        return {Error::Unavailable, "Media worker unavailable on this platform"};
    case platform::ProcessError::Cancelled:
        return {Error::Cancelled, "Encode cancelled"};
    case platform::ProcessError::Timeout:
        return {Error::Timeout, "Encode watchdog expired"};
    case platform::ProcessError::Crashed:
        return {Error::Crashed, "Media worker crashed during encode"};
    case platform::ProcessError::ResourceLimit:
        return {Error::Oversized, "Media worker resource limit"};
    case platform::ProcessError::Spawn:
    case platform::ProcessError::Io:
        return {Error::Io, "Media worker I/O failed"};
    }
    return {Error::Io, "Media worker failed"};
}
std::optional<Unavailable> failure(const Payload& payload) {
    if (const auto* e = std::get_if<Unavailable>(&payload))
        return *e;
    return std::nullopt;
}
template <typename T> Result<T> take(Payload payload) {
    if (auto* v = std::get_if<T>(&payload))
        return std::move(*v);
    if (auto* e = std::get_if<Unavailable>(&payload))
        return std::move(*e);
    return Unavailable{Error::UnexpectedMessage, "Unexpected encode product"};
}
} // namespace
struct EncodeSessionV1::State {
    EncodeSessionOptionsV1 options;
    platform::ProcessCancellation cancel;
    Handshake hello;
    MediaProviderProvenanceV1 provenance;
    std::unique_ptr<platform::ProcessSupervisor> process;
    std::unique_ptr<HostProtocol> protocol;
    std::uint64_t session = 1, sequence = 0;
    std::optional<Unavailable> terminal;
    explicit State(EncodeSessionOptionsV1 o, platform::ProcessCancellation c)
        : options(std::move(o)), cancel(std::move(c)) {}
    Payload stop(Unavailable e) {
        terminal = e;
        if (process)
            process->stop();
        process.reset();
        return e;
    }
    // Frame and audio messages are written without waiting for their acknowledgements, so the
    // worker encodes one product while the host prepares the next. The worker answers strictly in
    // order; every other request first collects the outstanding acknowledgements, and a failed
    // acknowledgement surfaces on the next call. The bound keeps the unread replies far below the
    // pipe buffer.
    static constexpr std::size_t kMaxUnacknowledged = 16;
    std::size_t unacknowledged = 0;
    std::optional<Unavailable> send(MessageKind kind, Payload payload) {
        if (terminal)
            return *terminal;
        if (cancel && cancel())
            return std::get<Unavailable>(stop({Error::Cancelled, "Encode cancelled"}));
        if (!process)
            return std::get<Unavailable>(
                stop({Error::UnexpectedMessage, "Encode session is not running"}));
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        const auto request = encodeMessage({kind, session, ++sequence, std::move(payload)});
        if (const auto* e = std::get_if<Unavailable>(&request))
            return std::get<Unavailable>(stop(*e));
        const auto written = process->write(std::get<Bytes>(request), deadline, cancel);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&written))
            return std::get<Unavailable>(stop(processFailure(*e)));
        return std::nullopt;
    }
    Payload receive(MessageKind request, MessageKind expected) {
        if (terminal)
            return *terminal;
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        std::array<std::byte, 4> prefix{};
        auto received = process->read(prefix, deadline, cancel);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&received))
            return stop(processFailure(*e));
        const auto length = messageLength(prefix);
        if (const auto* e = std::get_if<Unavailable>(&length))
            return stop(*e);
        // Encode replies never contain an unbounded frame; fail before allocating hostile lengths.
        if (request != MessageKind::Handshake &&
            std::get<std::uint32_t>(length) > kEncodeChunkBytes + 4096)
            return stop({Error::Oversized, "Oversized encode response"});
        Bytes response(prefix.begin(), prefix.end());
        response.resize(4U + std::get<std::uint32_t>(length));
        received = process->read(std::span(response).subspan(4), deadline, cancel);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&received))
            return stop(processFailure(*e));
        auto message = protocol->receive(response, expected);
        if (const auto* e = std::get_if<Unavailable>(&message))
            return stop(*e);
        auto result = std::get<Message>(std::move(message)).payload;
        if (const auto* e = std::get_if<Unavailable>(&result))
            return stop(*e);
        return result;
    }
    std::optional<Unavailable> collect(const std::size_t keep) {
        while (unacknowledged > keep) {
            --unacknowledged;
            if (const auto error = failure(receive(MessageKind::Frame, MessageKind::Ack)))
                return error;
        }
        return std::nullopt;
    }
    std::optional<Unavailable> post(MessageKind kind, Payload payload) {
        if (const auto error = collect(kMaxUnacknowledged - 1))
            return error;
        if (const auto error = send(kind, std::move(payload)))
            return error;
        ++unacknowledged;
        return std::nullopt;
    }
    Payload exchange(MessageKind kind, Payload payload, MessageKind expected) {
        if (const auto error = collect(0))
            return *error;
        if (const auto error = send(kind, std::move(payload)))
            return *error;
        return receive(kind, expected);
    }
    // One complete attempt on one provider route: launch, handshake and EncodeBegin. A refusal
    // reaps the worker and leaves the session ready for the next route; nothing was produced.
    std::optional<Unavailable> start(const EncodeRoute& route, const EncodeSettingsV1& settings) {
        process.reset();
        protocol.reset();
        terminal.reset();
        sequence = 0;
        unacknowledged = 0;
        hello = route.hello;
        platform::ProcessOptions launch;
        launch.executable = route.executable;
        // Issue #352: the export worker gets the address space its frame extent needs.
        launch.addressSpaceBytes = workerAddressSpaceBytes(settings.width, settings.height);
        if (options.stagedOutput) {
            // Streaming into the staged artifact is an FFmpeg worker protocol (ADR 0033 section
            // 16); the caller expects the bytes there, so a platform route cannot serve it.
            if (route.native)
                return Unavailable{Error::Unavailable, "Streamed staged output needs FFmpeg"};
            launch.stagedOutput = options.stagedOutput;
            launch.arguments.push_back("--staged-output");
        }
        launch.arguments.insert(launch.arguments.end(), route.arguments.begin(),
                                route.arguments.end());
        if (!route.native)
            configureFfmpegWorkerEnvironment(launch, launch.executable,
                                             std::filesystem::path(route.openh264));
        auto launched = platform::ProcessSupervisor::launch(launch);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&launched))
            return processFailure(*e);
        process = std::get<std::unique_ptr<platform::ProcessSupervisor>>(std::move(launched));
        if (options.launched)
            options.launched(process->processId());
        std::random_device random;
        session = (static_cast<std::uint64_t>(random()) << 32U) | random();
        if (session == 0)
            session = 1;
        protocol = std::make_unique<HostProtocol>(session, hello);
        if (const auto error =
                failure(exchange(MessageKind::Handshake, hello, MessageKind::Handshake)))
            return error;
        return failure(exchange(MessageKind::EncodeBegin, settings, MessageKind::Ack));
    }
};
EncodeSessionV1::EncodeSessionV1(EncodeSessionOptionsV1 options,
                                 platform::ProcessCancellation cancel)
    : state_(std::make_unique<State>(std::move(options), std::move(cancel))) {}
EncodeSessionV1::~EncodeSessionV1() = default;
std::optional<Unavailable> EncodeSessionV1::begin(const EncodeSettingsV1& settings) {
    auto& s = *state_;
    if (!valid(settings) || s.sequence != 0 || s.process ||
        s.options.timeout <= std::chrono::milliseconds(0) ||
        s.options.timeout > std::chrono::minutes(5))
        return Unavailable{Error::InvalidValue, "Invalid encode session request"};
    if (s.cancel && s.cancel())
        return Unavailable{Error::Cancelled, "Encode cancelled"};
    const bool h264 = settings.videoCodec == "h264";
    // Software H.264 in the FFmpeg worker is Cisco's OpenH264 where Bloom verifies that binary
    // (Linux x86-64). Elsewhere the FFmpeg route does not declare H.264 and the platform media
    // framework serves it.
    bool openh264 = false;
    if (h264 && OpenH264Runtime::platformSupported()) {
        const auto root = s.options.openh264Directory.empty()
                              ? std::filesystem::path{}
                              : std::filesystem::path(s.options.openh264Directory);
        const auto status = OpenH264Runtime(root).verify();
        openh264 = status.installed;
        if (openh264) {
            s.options.openh264Directory = status.directory.string();
            s.options.openh264Version = status.version;
            s.options.openh264Digest = status.digest;
        }
    }
    std::vector<EncodeRoute> routes;
    // The platform media framework first (ADR 0035). Windows has a Media Foundation slot with no
    // provider yet; it reports NotInstalled and FFmpeg serves the encode.
    if (const auto native = nativeMediaProviderId(); !native.empty()) {
        EncodeRoute route;
        route.id = std::string(native);
        route.native = true;
        route.executable = nativeWorkerExecutable();
        route.hello = nativeMediaHandshake();
        routes.push_back(std::move(route));
    }
    const auto ffmpeg =
        s.options.executable.empty() ? ffmpegWorkerExecutable() : s.options.executable;
    if (s.options.vaapi) {
        EncodeRoute route;
        route.id = std::string(kFfmpegVaapiProviderId);
        route.executable = ffmpeg;
        route.arguments = {"--vaapi"};
        route.hello = ffmpegHandshake(true, false, {}, {});
        routes.push_back(std::move(route));
    }
    {
        EncodeRoute route;
        route.id = std::string(kFfmpegProviderId);
        route.executable = ffmpeg;
        route.hello =
            ffmpegHandshake(false, openh264, s.options.openh264Version, s.options.openh264Digest);
        if (openh264) {
            route.arguments = {"--openh264-dir", s.options.openh264Directory, "--openh264-sha256",
                               s.options.openh264Digest};
            route.openh264 = s.options.openh264Directory;
        }
        routes.push_back(std::move(route));
    }
    std::vector<MediaProviderCandidateV1> candidates;
    candidates.reserve(routes.size());
    for (const auto& route : routes)
        candidates.push_back({route.id, route.native,
                              [&route] { return regularFileExists(route.executable); },
                              [&route](const MediaOperationV1& operation) {
                                  return declaresMediaOperationV1(route.hello, operation);
                              }});
    const MediaOperationV1 operation{
        MediaDirectionV1::Encode,
        mediaCodecFamilyV1(settings.videoCodec.empty() ? settings.audioCodec : settings.videoCodec),
        settings.container};
    const MediaProviderChainV1 chain(std::move(candidates));
    const auto result = chain.run<std::monostate>(
        operation,
        [&](const MediaProviderCandidateV1& candidate) -> Result<std::monostate> {
            const auto& route = *std::ranges::find_if(
                routes, [&](const EncodeRoute& r) { return r.id == candidate.id; });
            if (const auto error = s.start(route, providerSettings(settings, route.hello)))
                return *error;
            return std::monostate{};
        },
        s.provenance);
    if (const auto* error = std::get_if<Unavailable>(&result)) {
        // Nothing was attempted for software H.264: the consented Cisco install is missing.
        const bool attempted = std::ranges::any_of(s.provenance.fallbacks, [](const auto& f) {
            return f.cause == MediaFallbackCauseV1::Failed;
        });
        if (!attempted && h264 && !s.options.vaapi && OpenH264Runtime::platformSupported() &&
            !openh264)
            return Unavailable{Error::Unavailable, "H.264 encoder not installed"};
        return *error;
    }
    return std::nullopt;
}
std::optional<Unavailable> EncodeSessionV1::video(FrameProduct frame) {
    return state_->post(MessageKind::Frame, std::move(frame));
}
std::optional<Unavailable> EncodeSessionV1::audio(AudioBlock block) {
    return state_->post(MessageKind::Audio, std::move(block));
}
Result<EncodeQcV1> EncodeSessionV1::finish() {
    return take<EncodeQcV1>(
        state_->exchange(MessageKind::EncodeFinish, std::monostate{}, MessageKind::EncodeQc));
}
Result<EncodedChunkV1> EncodeSessionV1::read(std::uint64_t offset) {
    auto result = take<EncodedChunkV1>(
        state_->exchange(MessageKind::EncodeRead, offset, MessageKind::EncodedChunk));
    if (const auto* chunk = std::get_if<EncodedChunkV1>(&result);
        chunk && (chunk->offset != offset || chunk->bytes.empty()))
        return std::get<Unavailable>(
            state_->stop({Error::IdentityMismatch, "Encoded chunk offset differs"}));
    return result;
}
std::optional<Unavailable> EncodeSessionV1::close() {
    if (const auto e =
            failure(state_->exchange(MessageKind::Shutdown, std::monostate{}, MessageKind::Ack)))
        return e;
    const auto ended =
        state_->process->finish(std::chrono::steady_clock::now() + state_->options.timeout);
    if (const auto* e = std::get_if<platform::ProcessFailure>(&ended))
        return processFailure(*e);
    state_->process.reset();
    return std::nullopt;
}
const ProviderExecutionKeyV1& EncodeSessionV1::execution() const { return state_->hello.execution; }
const Handshake& EncodeSessionV1::handshake() const { return state_->hello; }
const MediaProviderProvenanceV1& EncodeSessionV1::provenance() const { return state_->provenance; }
std::string EncodeSessionV1::defaultWorker() { return ffmpegWorkerExecutable(); }
} // namespace bloom::media::provider
