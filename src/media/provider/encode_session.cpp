#include <array>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/media/provider/ffmpeg_launch.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/openh264_runtime.hpp>
#include <filesystem>
#include <random>

namespace bloom::media::provider {
namespace {
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
    Handshake hello = ffmpegHandshake();
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
    Payload exchange(MessageKind kind, Payload payload, MessageKind expected) {
        if (terminal)
            return *terminal;
        if (cancel && cancel())
            return stop({Error::Cancelled, "Encode cancelled"});
        if (!process)
            return stop({Error::UnexpectedMessage, "Encode session is not running"});
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        const auto request = encodeMessage({kind, session, ++sequence, std::move(payload)});
        if (const auto* e = std::get_if<Unavailable>(&request))
            return stop(*e);
        const auto written = process->write(std::get<Bytes>(request), deadline, cancel);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&written))
            return stop(processFailure(*e));
        std::array<std::byte, 4> prefix{};
        auto received = process->read(prefix, deadline, cancel);
        if (const auto* e = std::get_if<platform::ProcessFailure>(&received))
            return stop(processFailure(*e));
        const auto length = messageLength(prefix);
        if (const auto* e = std::get_if<Unavailable>(&length))
            return stop(*e);
        // Encode replies never contain an unbounded frame; fail before allocating hostile lengths.
        if (kind != MessageKind::Handshake &&
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
};
EncodeSessionV1::EncodeSessionV1(EncodeSessionOptionsV1 options,
                                 platform::ProcessCancellation cancel)
    : state_(std::make_unique<State>(std::move(options), std::move(cancel))) {}
EncodeSessionV1::~EncodeSessionV1() = default;
std::optional<Unavailable> EncodeSessionV1::begin(const EncodeSettingsV1& settings) {
    auto& s = *state_;
    if (!valid(settings) || s.sequence != 0 || s.options.timeout <= std::chrono::milliseconds(0) ||
        s.options.timeout > std::chrono::minutes(5))
        return Unavailable{Error::InvalidValue, "Invalid encode session request"};
    if (s.cancel && s.cancel())
        return Unavailable{Error::Cancelled, "Encode cancelled"};
    const bool h264 = settings.videoCodec == "h264";
    const bool h264Software = h264 && !s.options.vaapi;
    OpenH264RuntimeStatus openh264;
    if (h264Software) {
        const auto root = s.options.openh264Directory.empty()
                              ? std::filesystem::path{}
                              : std::filesystem::path(s.options.openh264Directory);
        openh264 = OpenH264Runtime(root).verify();
        if (!openh264.installed)
            return Unavailable{Error::Unavailable, "H.264 encoder not installed"};
        s.options.openh264Directory = openh264.directory.string();
        s.options.openh264Version = openh264.version;
        s.options.openh264Digest = openh264.digest;
    }
    s.hello = ffmpegHandshake(s.options.vaapi, h264Software, s.options.openh264Version,
                              s.options.openh264Digest);
    if (s.options.executable.empty())
        s.options.executable = defaultWorker();
    platform::ProcessOptions options;
    options.executable = s.options.executable;
    if (s.options.vaapi)
        options.arguments.push_back("--vaapi");
    if (h264Software) {
        options.arguments.insert(options.arguments.end(),
                                 {"--openh264-dir", s.options.openh264Directory,
                                  "--openh264-sha256", s.options.openh264Digest});
    }
    configureFfmpegWorkerEnvironment(options, options.executable,
                                     h264Software
                                         ? std::filesystem::path(s.options.openh264Directory)
                                         : std::filesystem::path{});
    auto launched = platform::ProcessSupervisor::launch(options);
    if (const auto* e = std::get_if<platform::ProcessFailure>(&launched))
        return processFailure(*e);
    s.process = std::get<std::unique_ptr<platform::ProcessSupervisor>>(std::move(launched));
    if (s.options.launched)
        s.options.launched(s.process->processId());
    std::random_device random;
    s.session = (static_cast<std::uint64_t>(random()) << 32U) | random();
    if (s.session == 0)
        s.session = 1;
    s.protocol = std::make_unique<HostProtocol>(s.session, s.hello);
    if (const auto error =
            failure(s.exchange(MessageKind::Handshake, s.hello, MessageKind::Handshake)))
        return error;
    return failure(s.exchange(MessageKind::EncodeBegin, settings, MessageKind::Ack));
}
std::optional<Unavailable> EncodeSessionV1::video(FrameProduct frame) {
    return failure(state_->exchange(MessageKind::Frame, std::move(frame), MessageKind::Ack));
}
std::optional<Unavailable> EncodeSessionV1::audio(AudioBlock block) {
    return failure(state_->exchange(MessageKind::Audio, std::move(block), MessageKind::Ack));
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
std::string EncodeSessionV1::defaultWorker() {
#if defined(__linux__)
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto installed = executable.parent_path().parent_path() /
                               "libexec/bloom/media/ffmpeg-v1/bloom-media-worker";
        if (std::filesystem::is_regular_file(installed, error) && !error)
            return installed.string();
    }
#endif
#ifdef BLOOM_ENCODE_WORKER
    return BLOOM_ENCODE_WORKER;
#else
    return {};
#endif
}
} // namespace bloom::media::provider
