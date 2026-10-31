#include "ffmpeg_provider.hpp"
#include "ffmpeg_reopen_compare.hpp"
#include <algorithm>
#include <array>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/platform/file_snapshot.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <thread>
#include <utility>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
namespace bloom::media::ffmpeg {
namespace {
using namespace provider;
constexpr std::uint64_t kMaximumDecodeThreads = 8;
void require(bool ok, const char* detail, Error code = Error::Corrupt) {
    if (!ok)
        throw Unavailable{code, detail};
}
// A codec this build cannot decode, refused by name (issue #398).
void requireDecoder(const AVCodecParameters& parameters) {
    if (avcodec_find_decoder(parameters.codec_id) == nullptr)
        throw Unavailable{Error::Unavailable, std::string("Codec not available in this build: ") +
                                                  avcodec_get_name(parameters.codec_id)};
}
Rational rational(std::int64_t n, std::int64_t d) {
    require(d > 0 && n != std::numeric_limits<std::int64_t>::min(), "Invalid timestamp");
    const auto divisor = std::gcd(n, d);
    return {n / divisor, d / divisor};
}
Rational timestamp(std::int64_t value, AVRational base) {
    require(value != AV_NOPTS_VALUE && base.num > 0 && base.den > 0 &&
                value <= std::numeric_limits<std::int64_t>::max() / base.num &&
                value >= std::numeric_limits<std::int64_t>::min() / base.num,
            "Missing or excessive timestamp");
    return rational(value * base.num, base.den);
}
struct Input {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* transfer = av_frame_alloc();
    SwsContext* scale = nullptr;
    // Source layout the cached conversion context was built for.
    std::array<int, 6> scaleKey{};
    SwrContext* resample = nullptr;
    std::unique_ptr<platform::WorkerSnapshotInput> snapshot;
    AVIOContext* io = nullptr;
    std::uint64_t offset = 0;
    ~Input() {
        swr_free(&resample);
        sws_freeContext(scale);
        av_frame_free(&transfer);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        if (io) {
            av_freep(static_cast<void*>(&io->buffer));
            avio_context_free(&io);
        }
    }
    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
};
int snapshotRead(void* opaque, std::uint8_t* bytes, const int count) {
    auto& input = *static_cast<Input*>(opaque);
    if (count <= 0)
        return AVERROR(EINVAL);
    const auto result = input.snapshot->read(
        input.offset, std::as_writable_bytes(std::span(bytes, static_cast<std::size_t>(count))));
    const auto* read = std::get_if<std::size_t>(&result);
    if (!read)
        return AVERROR(EIO);
    input.offset += *read;
    return *read == 0 ? AVERROR_EOF : static_cast<int>(*read);
}
std::int64_t snapshotSeek(void* opaque, const std::int64_t offset, int whence) {
    auto& input = *static_cast<Input*>(opaque);
    const auto size = static_cast<std::int64_t>(input.snapshot->size());
    if (whence == AVSEEK_SIZE)
        return size;
    whence &= ~AVSEEK_FORCE;
    const auto base = whence == SEEK_SET   ? std::int64_t{0}
                      : whence == SEEK_CUR ? static_cast<std::int64_t>(input.offset)
                      : whence == SEEK_END ? size
                                           : std::int64_t{-1};
    if (base < 0 || offset < -base || offset > size - base)
        return AVERROR(EINVAL);
    input.offset = static_cast<std::uint64_t>(base + offset);
    return static_cast<std::int64_t>(input.offset);
}
std::pair<Digest, std::uint64_t> sourceIdentity(const std::string& path) {
    const std::filesystem::path file(path);
    require(file.is_absolute() && std::filesystem::is_regular_file(file),
            "Media source must be a local regular file", Error::Io);
    const auto size = std::filesystem::file_size(file);
    require(size > 0 && size <= Limits::sourceBytes, "Media source exceeds limits",
            Error::Oversized);
    std::ifstream input(file, std::ios::binary);
    core::Sha256Hasher hasher;
    std::array<char, 65536> buffer{};
    std::uint64_t total = 0;
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        total += count;
        require(total <= size && hasher.update(std::as_bytes(std::span(buffer.data(), count))),
                "Media source changed while hashing", Error::SourceChanged);
    }
    require(input.eof() && total == size, "Media source read failed", Error::Io);
    return {hasher.finalize(), size};
}
// Display orientation from the stream's display matrix: clockwise quarter turns and a horizontal
// mirror, applied to decoded pixels as source interpretation. Non-right angles are not applied.
struct Orientation final {
    unsigned quarterTurns = 0;
    bool mirror = false;
    [[nodiscard]] bool identity() const { return quarterTurns == 0 && !mirror; }
};
Orientation orientation(const AVCodecParameters& p) {
    const auto* side =
        av_packet_side_data_get(p.coded_side_data, p.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (side == nullptr || side->size < 9 * sizeof(std::int32_t))
        return {};
    const auto* matrix = reinterpret_cast<const std::int32_t*>(side->data);
    const auto degrees = av_display_rotation_get(matrix);
    if (!std::isfinite(degrees))
        return {};
    // av_display_rotation_get is counter-clockwise; display needs the clockwise correction.
    const auto clockwise = std::lround(-degrees);
    if (clockwise % 90 != 0)
        return {};
    Orientation result;
    result.quarterTurns = static_cast<unsigned>(((clockwise / 90) % 4 + 4) % 4);
    const auto determinant =
        static_cast<double>(matrix[0]) * matrix[4] - static_cast<double>(matrix[1]) * matrix[3];
    result.mirror = determinant < 0;
    return result;
}
// Square-pixel width for a non-square sample aspect ratio (container value first, then the
// bitstream's). Anamorphic pictures are resampled horizontally to square pixels at decode, so the
// rest of Bloom sees display geometry; a missing or unit SAR keeps the coded width.
int displayWidth(const AVStream& stream) {
    auto sar = stream.sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0)
        sar = stream.codecpar->sample_aspect_ratio;
    const auto width = stream.codecpar->width;
    if (sar.num <= 0 || sar.den <= 0 || sar.num == sar.den)
        return width;
    const auto scaled = av_rescale(width, sar.num, sar.den);
    require(scaled > 0 && scaled <= static_cast<std::int64_t>(Limits::dimension),
            "Display width exceeds limits", Error::Oversized);
    return static_cast<int>(scaled);
}
ColourTags colour(const AVCodecParameters& p) {
    return {p.color_primaries, p.color_trc, p.color_space, p.color_range};
}
std::vector<std::string> channels(const AVChannelLayout& layout) {
    require(layout.nb_channels > 0 && layout.nb_channels <= static_cast<int>(Limits::channels),
            "Unsupported audio channel count", Error::Oversized);
    std::vector<std::string> result;
    for (int i = 0; i < layout.nb_channels; ++i) {
        std::array<char, 64> name{};
        const auto channel =
            av_channel_layout_channel_from_index(&layout, static_cast<unsigned>(i));
        if (channel == AV_CHAN_UNKNOWN || channel == AV_CHAN_NONE)
            result.push_back("channel-" + std::to_string(i));
        else {
            require(av_channel_name(name.data(), name.size(), channel) >= 0,
                    "Invalid channel layout");
            result.emplace_back(name.data());
        }
    }
    return result;
}
void extent(int w, int h) {
    require(w > 0 && h > 0 && w <= static_cast<int>(Limits::dimension) &&
                h <= static_cast<int>(Limits::dimension) &&
                static_cast<std::uint64_t>(w) * static_cast<unsigned>(h) <= Limits::pixels,
            "Video dimensions exceed limits", Error::Oversized);
}
ProbeResult probe(Input& input, const Digest& digest, std::uint64_t bytes) {
    ProbeResult result;
    result.container = input.format->iformat->name;
    result.version = "FFmpeg-8.1.2";
    result.sourceBytes = bytes;
    result.sourceDigest = digest;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        const auto& stream = *input.format->streams[i];
        const auto& p = *stream.codecpar;
        StreamDescriptor s;
        s.id = i;
        s.codec = avcodec_get_name(p.codec_id);
        const auto* profile = avcodec_profile_name(p.codec_id, p.profile);
        s.profile = profile ? profile : "unknown";
        s.timebase = rational(stream.time_base.num, stream.time_base.den);
        if (stream.duration != AV_NOPTS_VALUE && stream.duration >= 0)
            s.duration = timestamp(stream.duration, stream.time_base);
        else if (input.format->duration > 0)
            s.duration = rational(input.format->duration, AV_TIME_BASE);
        s.frameCount = static_cast<std::uint64_t>(std::max<std::int64_t>(0, stream.nb_frames));
        const auto* timecode = av_dict_get(stream.metadata, "timecode", nullptr, 0);
        if (!timecode)
            timecode = av_dict_get(input.format->metadata, "timecode", nullptr, 0);
        if (timecode) {
            require(std::strlen(timecode->value) <= Limits::stringBytes, "Timecode exceeds limits");
            s.timecode = timecode->value;
        }
        if (p.codec_type == AVMEDIA_TYPE_VIDEO) {
            extent(p.width, p.height);
            const auto turn = orientation(p);
            const auto width = displayWidth(stream);
            extent(width, p.height);
            s.width = static_cast<std::uint32_t>(turn.quarterTurns % 2 ? p.height : width);
            s.height = static_cast<std::uint32_t>(turn.quarterTurns % 2 ? width : p.height);
            const auto rate = av_guess_frame_rate(input.format, input.format->streams[i], nullptr);
            require(rate.num > 0 && rate.den > 0, "Video rate unavailable", Error::Unavailable);
            s.rate = rational(rate.num, rate.den);
            // A variable-cadence stream is addressed on its nominal grid (see video()), so its
            // frame count is the number of nominal slots its duration covers, not its packets.
            if (stream.duration != AV_NOPTS_VALUE && stream.duration > 0) {
                const auto slots =
                    av_rescale_q_rnd(stream.duration, stream.time_base, av_inv_q(rate),
                                     static_cast<AVRounding>(AV_ROUND_UP | AV_ROUND_PASS_MINMAX));
                if (slots > 0 && stream.nb_frames > 0 &&
                    (slots > stream.nb_frames + 1 || slots + 1 < stream.nb_frames))
                    s.frameCount = static_cast<std::uint64_t>(slots);
            }
            const auto* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(p.format));
            s.pixelFormat = name ? name : "unknown";
            const auto* layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(p.format));
            s.format = p.format == AV_PIX_FMT_YUV420P && turn.identity() ? PixelFormat::Yuv420p8
                       : layout && (layout->flags & AV_PIX_FMT_FLAG_RGB) != 0
                           ? PixelFormat::Rgba16
                           : PixelFormat::Yuva444p16;
            s.colour = colour(p);
            s.fieldOrder = p.field_order >= AV_FIELD_UNKNOWN && p.field_order <= AV_FIELD_BT
                               ? static_cast<std::uint8_t>(p.field_order)
                               : 0;
        } else if (p.codec_type == AVMEDIA_TYPE_AUDIO) {
            s.kind = MediaKind::Audio;
            require(p.sample_rate > 0 && p.sample_rate <= static_cast<int>(Limits::sampleRate),
                    "Audio rate exceeds limits", Error::Oversized);
            s.sampleRate = static_cast<std::uint32_t>(p.sample_rate);
            s.rate = {p.sample_rate, 1};
            s.channelLayout = channels(p.ch_layout);
        } else
            s.kind = MediaKind::Data;
        result.streams.push_back(std::move(s));
    }
    // Issue #398: a stream whose codec this build cannot decode (VP9, AV1) is refused at probe by
    // name, so import says why instead of failing later or on another path. The first video stream
    // is the one an asset uses; attached pictures (cover art) are not video.
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        const auto& stream = *input.format->streams[i];
        if (stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
            (stream.disposition & AV_DISPOSITION_ATTACHED_PIC) != 0)
            continue;
        requireDecoder(*stream.codecpar);
        break;
    }
    require(valid(result), "Invalid probe metadata");
    return result;
}
DemuxIndex index(Input& input) {
    DemuxIndex result;
    int status = 0;
    while ((status = av_read_frame(input.format, input.packet)) >= 0) {
        const auto& packet = *input.packet;
        if ((packet.flags & AV_PKT_FLAG_KEY) != 0) {
            require(result.keyframes.size() < Limits::indexEntries, "Index exceeds limits",
                    Error::Oversized);
            require(packet.stream_index >= 0 &&
                        static_cast<unsigned>(packet.stream_index) < input.format->nb_streams,
                    "Invalid packet stream");
            const auto base = input.format->streams[packet.stream_index]->time_base;
            result.keyframes.push_back({static_cast<std::uint32_t>(packet.stream_index),
                                        timestamp(packet.pts, base), timestamp(packet.dts, base)});
        }
        av_packet_unref(input.packet);
    }
    require(status == AVERROR_EOF, "Malformed demux input");
    return result;
}
// Prefer the VA-API surface when the hwaccel offers it. When hwaccel initialisation refuses the
// stream, FFmpeg asks again without it; answer with software so the caller can observe the miss
// on the first frame instead of failing inside the decoder.
AVPixelFormat hardwareFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (const auto* p = formats; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    for (const auto* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const auto* desc = av_pix_fmt_desc_get(*p);
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0)
            return *p;
    }
    return AV_PIX_FMT_NONE;
}
// The address-space limit the host launched this worker with (the media-v1 floor if unreadable).
std::uint64_t workerAddressSpace() {
#if defined(__unix__) || defined(__APPLE__)
    rlimit limit{};
    if (::getrlimit(RLIMIT_AS, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
        limit.rlim_cur >= kWorkerAddressSpaceFloor)
        return std::min<std::uint64_t>(limit.rlim_cur, kWorkerAddressSpaceCeiling);
#elif defined(_WIN32)
    // The host's job object carries the per-process committed-memory limit.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof(job),
                                  nullptr) &&
        (job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0 &&
        job.ProcessMemoryLimit >= kWorkerAddressSpaceFloor)
        return std::min<std::uint64_t>(job.ProcessMemoryLimit, kWorkerAddressSpaceCeiling);
#endif
    return kWorkerAddressSpaceFloor;
}
// Thrown when a VA-API decoder silently produced software frames for this device.
struct HardwareMiss final {};
// Bounded software decoder parallelism. Frame threads keep extra reference and output frames in
// flight, so the count is sized against half of this worker's address space (sized per stream by
// the host, issue #352) as well as the machine. Intra-only codecs use slice threads: a
// random-access request then waits for one frame rather than for a pipeline of following frames.
void configureThreads(AVCodecContext& codec, const AVCodecParameters& p) {
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(p.format));
    const std::uint64_t sampleBytes = desc && desc->comp[0].depth > 8 ? 2U : 1U;
    const auto frameBytes = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::max(p.width, 1)) *
               static_cast<std::uint64_t>(std::max(p.height, 1)) * 3U * sampleBytes);
    const auto budget = workerAddressSpace() / 2U;
    const auto byMemory = budget / (frameBytes * 4U);
    const auto cores = std::max(1U, std::thread::hardware_concurrency());
    codec.thread_count = static_cast<int>(std::clamp<std::uint64_t>(
        std::min<std::uint64_t>(cores, byMemory), 1, kMaximumDecodeThreads));
    const bool intra = p.codec_id == AV_CODEC_ID_PRORES || p.codec_id == AV_CODEC_ID_DNXHD ||
                       p.codec_id == AV_CODEC_ID_MJPEG || p.codec_id == AV_CODEC_ID_TIFF;
    codec.thread_type = intra ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE;
}
// A null device selects the software reference decoder.
void decoder(Input& input, const AVStream& stream, AVBufferRef* device) {
    requireDecoder(*stream.codecpar);
    const auto* codec = avcodec_find_decoder(stream.codecpar->codec_id);
    input.codec = avcodec_alloc_context3(codec);
    require(input.codec != nullptr, "Decoder allocation failed", Error::Oversized);
    require(avcodec_parameters_to_context(input.codec, stream.codecpar) >= 0,
            "Invalid codec parameters");
    input.codec->max_pixels = Limits::pixels;
    input.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    // The TIFF decoder treats every unrecognized (often purely descriptive) tag as an error under
    // AV_EF_EXPLODE, including tags its own encoder writes; decoding errors still fail the frame.
    if (stream.codecpar->codec_id == AV_CODEC_ID_TIFF)
        input.codec->err_recognition &= ~AV_EF_EXPLODE;
    if (device) {
        input.codec->thread_count = 1;
        input.codec->hw_device_ctx = av_buffer_ref(device);
        require(input.codec->hw_device_ctx != nullptr, "VA-API device reference failed",
                Error::Oversized);
        input.codec->get_format = hardwareFormat;
    } else
        configureThreads(*input.codec, *stream.codecpar);
    require(avcodec_open2(input.codec, codec, nullptr) >= 0, "Decoder refused stream",
            Error::Unavailable);
}
// Render nodes in numeric order, discovered rather than assumed. Bounded to the DRM minor range.
std::vector<std::string> renderNodes() {
    std::vector<std::pair<unsigned, std::string>> nodes;
    std::error_code error;
    for (std::filesystem::directory_iterator it("/dev/dri", error), end; !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with("renderD") || name.size() > 10)
            continue;
        unsigned number = 0;
        bool digits = name.size() > 7;
        for (const auto c : name.substr(7)) {
            digits = digits && c >= '0' && c <= '9';
            number = number * 10U + static_cast<unsigned>(c - '0');
        }
        if (digits && nodes.size() < 64)
            nodes.emplace_back(number, it->path().string());
    }
    std::ranges::sort(nodes);
    std::vector<std::string> result;
    for (auto& node : nodes)
        result.push_back(std::move(node.second));
    return result;
}
// Receive before sending; drain delayed frames at EOF. A packet is never dropped on EAGAIN.
bool next(Input& input, unsigned stream, bool& eof) {
    while (true) {
        const int received = avcodec_receive_frame(input.codec, input.frame);
        if (received == 0) {
            require((input.frame->flags & AV_FRAME_FLAG_CORRUPT) == 0, "Corrupt decoded frame");
            return true;
        }
        if (received == AVERROR_EOF)
            return false;
        require(received == AVERROR(EAGAIN) && !eof, "Decode failed");
        int read = 0;
        do {
            av_packet_unref(input.packet);
            read = av_read_frame(input.format, input.packet);
        } while (read >= 0 && input.packet->stream_index != static_cast<int>(stream));
        if (read == AVERROR_EOF) {
            eof = true;
            require(avcodec_send_packet(input.codec, nullptr) >= 0, "Decoder flush failed");
        } else {
            require(read >= 0 && avcodec_send_packet(input.codec, input.packet) >= 0,
                    "Invalid media packet");
        }
    }
}
// Applies a display orientation to full-resolution planes (16-bit samples, RGBA16 pixels).
void orient(FrameProduct& product, const Orientation turn) {
    for (auto& plane : product.planes) {
        const auto pixelBytes = product.format == PixelFormat::Rgba16 ? 8U : 2U;
        const auto width = plane.width, height = plane.height;
        const bool swap = turn.quarterTurns % 2 != 0;
        CpuPlane rotated;
        rotated.width = swap ? height : width;
        rotated.height = swap ? width : height;
        rotated.stride = rotated.width * pixelBytes;
        rotated.bytes.resize(static_cast<std::size_t>(rotated.stride) * rotated.height);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                // Mirror first (horizontal), then turn clockwise.
                const auto mx = turn.mirror ? width - 1 - x : x;
                std::uint32_t dx = mx, dy = y;
                switch (turn.quarterTurns) {
                case 1:
                    dx = height - 1 - y;
                    dy = mx;
                    break;
                case 2:
                    dx = width - 1 - mx;
                    dy = height - 1 - y;
                    break;
                case 3:
                    dx = y;
                    dy = width - 1 - mx;
                    break;
                default:
                    break;
                }
                std::memcpy(rotated.bytes.data() + static_cast<std::size_t>(dy) * rotated.stride +
                                static_cast<std::size_t>(dx) * pixelBytes,
                            plane.bytes.data() + static_cast<std::size_t>(y) * plane.stride +
                                static_cast<std::size_t>(x) * pixelBytes,
                            pixelBytes);
            }
        plane = std::move(rotated);
    }
}
using FrameHandle = std::shared_ptr<AVFrame>;
FrameHandle cloneFrame(const AVFrame* source) {
    auto* copy = av_frame_clone(source);
    require(copy != nullptr, "Frame reference failed", Error::Oversized);
    return {copy, [](AVFrame* frame) { av_frame_free(&frame); }};
}
// FFmpeg's bwdif (Bob Weaver) deinterlacer, field-aware and honouring each frame's field order
// (parity=auto), applied to one interlaced frame with its decoded neighbours. bwdif needs the
// previous and next frame; at a seek point or the stream end it uses only what exists.
FrameHandle bwdif(const AVStream& stream, const FrameHandle& previous, const FrameHandle& current,
                  const FrameHandle& following) {
    struct Graph final {
        AVFilterGraph* graph = avfilter_graph_alloc();
        ~Graph() { avfilter_graph_free(&graph); }
    } owner;
    require(owner.graph != nullptr, "Deinterlacer allocation failed", Error::Oversized);
    std::array<char, 256> args{};
    std::snprintf(args.data(), args.size(),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1", current->width,
                  current->height, current->format, stream.time_base.num, stream.time_base.den);
    AVFilterContext *source = nullptr, *deinterlace = nullptr, *sink = nullptr;
    require(avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "in", args.data(),
                                         nullptr, owner.graph) >= 0 &&
                avfilter_graph_create_filter(&deinterlace, avfilter_get_by_name("bwdif"), "bwdif",
                                             "mode=send_frame:parity=auto:deint=all", nullptr,
                                             owner.graph) >= 0 &&
                avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out",
                                             nullptr, nullptr, owner.graph) >= 0 &&
                avfilter_link(source, 0, deinterlace, 0) >= 0 &&
                avfilter_link(deinterlace, 0, sink, 0) >= 0 &&
                avfilter_graph_config(owner.graph, nullptr) >= 0,
            "Deinterlacer unavailable", Error::Unavailable);
    for (const auto* frame : {previous.get(), current.get(), following.get()}) {
        if (frame == nullptr)
            continue;
        auto input = cloneFrame(frame);
        input->pts = frame->best_effort_timestamp;
        require(av_buffersrc_add_frame_flags(source, input.get(), 0) >= 0,
                "Deinterlacer input refused");
    }
    if (!following)
        require(av_buffersrc_add_frame(source, nullptr) >= 0, "Deinterlacer flush refused");
    auto output = cloneFrame(current.get());
    av_frame_unref(output.get());
    while (true) {
        const auto status = av_buffersink_get_frame(sink, output.get());
        require(status >= 0, "Deinterlaced frame unavailable", Error::Unavailable);
        // bwdif halves the time base (it can emit one frame per field); compare in stream time.
        if (av_rescale_q(output->pts, av_buffersink_get_time_base(sink), stream.time_base) ==
            current->best_effort_timestamp) {
            output->pts = output->best_effort_timestamp = current->best_effort_timestamp;
            return output;
        }
        av_frame_unref(output.get());
    }
}
FrameProduct frame(Input& input, const AVStream& stream, AVFrame* decoded) {
    if (decoded->format == AV_PIX_FMT_VAAPI) {
        av_frame_unref(input.transfer);
        require(av_hwframe_transfer_data(input.transfer, decoded, 0) >= 0,
                "Hardware download failed");
        require(av_frame_copy_props(input.transfer, decoded) >= 0, "Hardware metadata copy failed");
        decoded = input.transfer;
    }
    extent(decoded->width, decoded->height);
    FrameProduct product;
    product.pts = timestamp(decoded->best_effort_timestamp, stream.time_base);
    product.colour = {decoded->color_primaries, decoded->color_trc, decoded->colorspace,
                      decoded->color_range};
    // Some decoders omit container colour metadata on AVFrame. Preserve explicit
    // stream tags when the frame has no tag; never guess a missing interpretation.
    const auto streamColour = colour(*stream.codecpar);
    if (product.colour.primaries == AVCOL_PRI_UNSPECIFIED)
        product.colour.primaries = streamColour.primaries;
    if (product.colour.transfer == AVCOL_TRC_UNSPECIFIED)
        product.colour.transfer = streamColour.transfer;
    if (product.colour.matrix == AVCOL_SPC_UNSPECIFIED)
        product.colour.matrix = streamColour.matrix;
    if (product.colour.range == AVCOL_RANGE_UNSPECIFIED)
        product.colour.range = streamColour.range;
    const auto format = static_cast<AVPixelFormat>(decoded->format);
    // A downloaded limited-range NV12 surface carries the same 8-bit 4:2:0 samples as the
    // software YUV420P product; de-interleave it so both routes publish one layout.
    // Unspecified 4:2:0 siting is the H.273/MPEG-2 default: co-sited left, vertically centred.
    const auto siting = decoded->chroma_location == AVCHROMA_LOC_UNSPECIFIED
                            ? AVCHROMA_LOC_LEFT
                            : decoded->chroma_location;
    const bool leftSited = siting == AVCHROMA_LOC_LEFT;
    const bool nv12 =
        format == AV_PIX_FMT_NV12 && product.colour.range != AVCOL_RANGE_JPEG && leftSited;
    // The compact 4:2:0 product carries left-sited chroma, which the host reconstructs with a
    // filter. Other sitings are reconstructed here, where the frame's chroma location is known.
    // A rotated or mirrored picture is reconstructed to full-resolution planes first, so its
    // chroma siting stays correct after the turn.
    const auto turn = orientation(*stream.codecpar);
    const auto outputWidth = displayWidth(stream);
    const bool anamorphic = outputWidth != stream.codecpar->width;
    const bool direct =
        turn.identity() && !anamorphic && ((format == AV_PIX_FMT_YUV420P && leftSited) || nv12);
    // RGB codecs (QuickTime Animation, PNG/TIFF in MOV, GBR planar) keep RGB: one packed
    // RGBA 16-bit plane, with alpha when the source has it and opaque otherwise.
    const auto* layout = av_pix_fmt_desc_get(format);
    require(layout != nullptr, "Pixel layout unavailable", Error::Unavailable);
    const bool rgb = (layout->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    product.format = direct ? PixelFormat::Yuv420p8
                     : rgb  ? PixelFormat::Rgba16
                            : PixelFormat::Yuva444p16;
    std::array<std::uint8_t*, 4> data{};
    std::array<int, 4> strides{};
    for (unsigned i = 0; i < (direct ? 3U : rgb ? 1U : 4U); ++i) {
        CpuPlane plane;
        plane.width = static_cast<std::uint32_t>(direct && i != 0 ? (decoded->width + 1) / 2
                                                 : direct         ? decoded->width
                                                                  : outputWidth);
        plane.height = static_cast<std::uint32_t>(direct && i != 0 ? (decoded->height + 1) / 2
                                                                   : decoded->height);
        plane.stride = plane.width * (direct ? 1U : rgb ? 8U : 2U);
        plane.bytes.resize(static_cast<std::size_t>(plane.stride) * plane.height);
        product.planes.push_back(std::move(plane));
        data[i] = reinterpret_cast<std::uint8_t*>(product.planes.back().bytes.data());
        strides[i] = static_cast<int>(product.planes.back().stride);
    }
    if (nv12) {
        for (unsigned y = 0; y < product.planes[0].height; ++y)
            std::memcpy(data[0] + static_cast<std::size_t>(y) * product.planes[0].stride,
                        decoded->data[0] + static_cast<std::ptrdiff_t>(y) * decoded->linesize[0],
                        product.planes[0].stride);
        for (unsigned y = 0; y < product.planes[1].height; ++y) {
            const auto* uv =
                decoded->data[1] + static_cast<std::ptrdiff_t>(y) * decoded->linesize[1];
            auto* u = data[1] + static_cast<std::size_t>(y) * product.planes[1].stride;
            auto* v = data[2] + static_cast<std::size_t>(y) * product.planes[2].stride;
            for (unsigned x = 0; x < product.planes[1].width; ++x) {
                u[x] = uv[2U * x];
                v[x] = uv[2U * x + 1U];
            }
        }
    } else if (direct) {
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned y = 0; y < product.planes[i].height; ++y)
                std::memcpy(data[i] + static_cast<std::size_t>(y) * product.planes[i].stride,
                            decoded->data[i] +
                                static_cast<std::ptrdiff_t>(y) * decoded->linesize[i],
                            product.planes[i].stride);
    } else {
        // Filtered chroma reconstruction honouring the frame's chroma siting. Source and
        // destination share coefficients and range, so no matrix or range change occurs. The
        // context is reused across a retained decoder's frames while the layout is unchanged.
        const int fullRange = product.colour.range == AVCOL_RANGE_JPEG ? 1 : 0;
        const std::array<int, 6> key{decoded->width, decoded->height, decoded->format,
                                     fullRange,      outputWidth,     static_cast<int>(siting)};
        if (!input.scale || input.scaleKey != key) {
            sws_freeContext(input.scale);
            input.scale = sws_alloc_context();
            require(input.scale != nullptr, "Pixel layout unavailable", Error::Oversized);
            // Siting applies per axis, and only along an axis the layout subsamples: 4:2:2 has
            // full vertical chroma resolution, so its chroma rows sit on the luma rows whatever
            // the declared location (whose vertical half is a 4:2:0 statement). Positions are in
            // 1/256 luma samples; 0 is co-sited with the first luma sample of the pair.
            int horizontal = 0, vertical = 0;
            require(av_chroma_location_enum_to_pos(&horizontal, &vertical, siting) >= 0,
                    "Chroma siting unavailable", Error::Unavailable);
            if (layout->log2_chroma_w == 0)
                horizontal = 0;
            if (layout->log2_chroma_h == 0)
                vertical = 0;
            auto* options = static_cast<void*>(input.scale);
            require(av_opt_set_int(options, "srcw", decoded->width, 0) >= 0 &&
                        av_opt_set_int(options, "srch", decoded->height, 0) >= 0 &&
                        av_opt_set_int(options, "src_format", decoded->format, 0) >= 0 &&
                        av_opt_set_int(options, "dstw", outputWidth, 0) >= 0 &&
                        av_opt_set_int(options, "dsth", decoded->height, 0) >= 0 &&
                        av_opt_set_int(options, "dst_format",
                                       rgb ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_YUVA444P16LE,
                                       0) >= 0 &&
                        av_opt_set_int(options, "sws_flags",
                                       SWS_BILINEAR | SWS_ACCURATE_RND | SWS_BITEXACT, 0) >= 0 &&
                        av_opt_set_int(options, "src_h_chr_pos", horizontal, 0) >= 0 &&
                        av_opt_set_int(options, "src_v_chr_pos", vertical, 0) >= 0 &&
                        sws_init_context(input.scale, nullptr, nullptr) >= 0,
                    "Pixel layout unavailable", Error::Unavailable);
            input.scaleKey = key;
        }
        const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        require(sws_setColorspaceDetails(input.scale, coefficients, fullRange, coefficients,
                                         fullRange, 0, 1 << 16, 1 << 16) >= 0,
                "Pixel range preservation unavailable", Error::Unavailable);
        require(sws_scale(input.scale, decoded->data, decoded->linesize, 0, decoded->height,
                          data.data(), strides.data()) == decoded->height,
                "Pixel layout conversion failed");
    }
    if (!turn.identity())
        orient(product, turn);
    assignPlaneDigests(product.planes);
    require(validLayout(product), "Invalid video product");
    return product;
}
struct VideoCursor final {
    std::vector<std::int64_t> timestamps;
    // Key packets as (presentation, decode) timestamps sorted by presentation: the random-access
    // points a seek can land on.
    std::vector<std::pair<std::int64_t, std::int64_t>> keyframes;
    // Presentation timestamp of the last frame this decoder returned.
    std::optional<std::int64_t> position;
    bool indexed = false;
    // The last decoded frame before the current position, and a frame decoded past it. Both feed
    // the deinterlacer; the lookahead is served to the next forward request before decoding.
    FrameHandle previous;
    FrameHandle lookahead;
    // Packet presentation times do not follow the nominal cadence (phone, screen and OBS
    // recordings). Requests then name a slot on the nominal grid instead of a packet ordinal.
    bool variable = false;
    bool eof = false;
    // Issue #352: a container that declares a constant cadence (its sample count equals its
    // duration in frame periods) and carries an in-memory demuxer index is addressed without the
    // whole-file packet scan. Frame N is then assumed at start + N / rate; any decode that
    // contradicts it (a missing presentation, an end before the frame) falls back to the scan.
    bool provisional = false;
    std::uint64_t provisionalFrames = 0;
    // Every indexed sample is a random-access point (intra-only codecs): seek straight to it.
    bool allKeys = false;
};
// The provisional constant-cadence assumption did not hold for this request.
struct ProvisionalMiss final {};
// Admits the lazy constant-cadence path from container metadata and the demuxer's index alone.
bool admitProvisional(AVStream& stream, const AVRational rate, VideoCursor& cursor) {
    if (stream.nb_frames <= 0 || stream.duration == AV_NOPTS_VALUE || stream.duration <= 0 ||
        static_cast<std::uint64_t>(stream.nb_frames) > Limits::indexEntries)
        return false;
    const auto slots = av_rescale_q(stream.duration, stream.time_base, av_inv_q(rate));
    if (slots != stream.nb_frames ||
        av_rescale_q(slots, av_inv_q(rate), stream.time_base) != stream.duration)
        return false;
    const auto entries = avformat_index_get_entries_count(&stream);
    if (entries <= 0)
        return false;
    std::vector<std::pair<std::int64_t, std::int64_t>> keyframes;
    for (int i = 0; i < entries; ++i) {
        const auto* entry = avformat_index_get_entry(&stream, i);
        if (entry == nullptr || entry->timestamp == AV_NOPTS_VALUE)
            return false;
        if ((entry->flags & AVINDEX_KEYFRAME) != 0)
            keyframes.emplace_back(entry->timestamp, entry->timestamp);
    }
    if (keyframes.empty())
        return false;
    cursor.allKeys = keyframes.size() == static_cast<std::size_t>(entries);
    // Index timestamps are decode times; they never follow the presentation they key, so a seek
    // by one can land late only on a reordered stream, which the decode loop corrects.
    if (!cursor.allKeys) {
        std::ranges::sort(keyframes);
        cursor.keyframes = std::move(keyframes);
    }
    cursor.provisional = true;
    cursor.provisionalFrames = static_cast<std::uint64_t>(slots);
    return true;
}

// A forward request decodes on from the current position when no later random-access point
// precedes the target: seeking would restart from a keyframe at or before where the decoder
// already is, so continuing is never more work. Backward requests and requests past a later
// keyframe seek to the keyframe at or before the target.
bool continueForward(const VideoCursor& cursor, const std::int64_t target) {
    if (!cursor.position || *cursor.position >= target || cursor.allKeys)
        return false;
    const auto after = std::ranges::upper_bound(cursor.keyframes, target, {},
                                                &std::pair<std::int64_t, std::int64_t>::first);
    return after == cursor.keyframes.begin() || std::prev(after)->first <= *cursor.position;
}
// Demuxers such as MOV index random access by decode time. With reordered (B-frame) streams a key
// packet's decode time precedes its presentation time, so seeking by the target presentation
// time can land on a keyframe presented after the target. Seek by the decode time of the last
// key packet presented at or before the target instead; an earlier landing only costs decoding.
std::int64_t seekTimestamp(const VideoCursor& cursor, const std::int64_t target) {
    if (cursor.allKeys)
        return target;
    const auto after = std::ranges::upper_bound(cursor.keyframes, target, {},
                                                &std::pair<std::int64_t, std::int64_t>::first);
    if (after == cursor.keyframes.begin())
        return target;
    const auto [pts, dts] = *std::prev(after);
    return dts == AV_NOPTS_VALUE ? pts : std::min(pts, dts);
}
FrameProduct video(Input& input, const CallRequest& request, AVBufferRef* device,
                   VideoCursor* retained = nullptr) {
    const auto& stream = *input.format->streams[request.stream];
    require(stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO, "Requested stream is not video");
    extent(stream.codecpar->width, stream.codecpar->height);
    VideoCursor local;
    auto& cursor = retained ? *retained : local;
    const auto rate =
        av_guess_frame_rate(input.format, input.format->streams[request.stream], nullptr);
    require(rate.num > 0 && rate.den > 0 && request.frame < Limits::indexEntries,
            "Frame index exceeds limits", Error::Oversized);
    const auto start = stream.start_time == AV_NOPTS_VALUE ? 0 : stream.start_time;
    // Qualify the packet presentation cadence before using a CFR ordinal. Without
    // this check a VFR stream could coincidentally contain the requested timestamp
    // while assigning it a different frame number. The admitted codecs/demuxers
    // produce one access unit per packet; missing or duplicate PTS are unavailable.
    const auto scan = [&] {
        cursor.provisional = false;
        cursor.allKeys = false;
        cursor.timestamps.clear();
        cursor.keyframes.clear();
        cursor.position.reset();
        cursor.previous.reset();
        cursor.lookahead.reset();
        cursor.variable = false;
        if (cursor.indexed) {
            // A provisional request moved the demuxer: rewind to its first indexed sample (the
            // provisional path is only admitted with a demuxer index).
            const auto* first = avformat_index_get_entry(input.format->streams[request.stream], 0);
            require(first != nullptr &&
                        av_seek_frame(input.format, static_cast<int>(request.stream),
                                      std::min(start, first->timestamp), AVSEEK_FLAG_BACKWARD) >= 0,
                    "Cannot seek video", Error::Unavailable);
            avcodec_flush_buffers(input.codec);
            cursor.eof = false;
        }
        int read = 0;
        while ((read = av_read_frame(input.format, input.packet)) >= 0) {
            if (input.packet->stream_index == static_cast<int>(request.stream)) {
                require(input.packet->pts != AV_NOPTS_VALUE, "Video packet timestamp unavailable",
                        Error::Unavailable);
                if (input.packet->pts >= start) {
                    require(cursor.timestamps.size() < Limits::indexEntries,
                            "Frame index exceeds limits", Error::Oversized);
                    cursor.timestamps.push_back(input.packet->pts);
                    if ((input.packet->flags & AV_PKT_FLAG_KEY) != 0)
                        cursor.keyframes.emplace_back(input.packet->pts, input.packet->dts);
                }
            }
            av_packet_unref(input.packet);
        }
        require(read == AVERROR_EOF, "Malformed video packets");
        std::ranges::sort(cursor.timestamps);
        std::ranges::sort(cursor.keyframes);
        require(!cursor.timestamps.empty() &&
                    std::ranges::adjacent_find(cursor.timestamps) == cursor.timestamps.end(),
                "Duplicate or missing video presentation timestamps", Error::Unavailable);
        for (std::size_t i = 0; i < cursor.timestamps.size() && !cursor.variable; ++i) {
            const auto expected =
                av_rescale_q(static_cast<std::int64_t>(i), av_inv_q(rate), stream.time_base);
            require(expected >= 0 && start <= std::numeric_limits<std::int64_t>::max() - expected,
                    "Frame timestamp overflow");
            cursor.variable = cursor.timestamps[i] != start + expected;
        }
        cursor.indexed = true;
    };
    if (!cursor.indexed) {
        decoder(input, stream, device);
        if (admitProvisional(*input.format->streams[request.stream], rate, cursor))
            cursor.indexed = true;
        else
            scan();
    }
    const auto decodeAt = [&]() -> FrameProduct {
        // Constant cadence: frame N is the Nth presentation. Variable cadence: frame N is the
        // presentation shown at nominal time start + N / rate (the last packet presented at or
        // before it), so composition time maps to source time by timestamp, not packet count.
        std::int64_t target = 0;
        if (cursor.provisional) {
            require(request.frame < cursor.provisionalFrames, "Frame is outside the video duration",
                    Error::Unavailable);
            const auto offset = av_rescale_q(static_cast<std::int64_t>(request.frame),
                                             av_inv_q(rate), stream.time_base);
            require(offset >= 0 && start <= std::numeric_limits<std::int64_t>::max() - offset,
                    "Frame timestamp overflow", Error::Oversized);
            target = start + offset;
        } else if (!cursor.variable) {
            require(request.frame < cursor.timestamps.size(), "Frame is outside the video duration",
                    Error::Unavailable);
            target = cursor.timestamps[request.frame];
        } else {
            const auto slot = av_rescale_q(static_cast<std::int64_t>(request.frame), av_inv_q(rate),
                                           stream.time_base);
            require(slot >= 0 && start <= std::numeric_limits<std::int64_t>::max() - slot,
                    "Frame timestamp overflow", Error::Oversized);
            const auto time = start + slot;
            const auto end = stream.duration != AV_NOPTS_VALUE && stream.duration > 0
                                 ? start + stream.duration
                                 : cursor.timestamps.back() + 1;
            require(time < end, "Frame is outside the video duration", Error::Unavailable);
            const auto shown = std::ranges::upper_bound(cursor.timestamps, time);
            target =
                shown == cursor.timestamps.begin() ? cursor.timestamps.front() : *std::prev(shown);
        }
        av_frame_unref(input.frame);
        // A provisional seek by decode time can land after the target on a reordered stream:
        // seek again earlier by a doubling distance before giving up on the assumption.
        bool seeked = false;
        std::int64_t backoff =
            std::max<std::int64_t>(1, av_rescale_q(16, av_inv_q(rate), stream.time_base));
        unsigned attempts = 0;
        const auto seekTo = [&](const std::int64_t timestamp) {
            cursor.position.reset();
            cursor.previous.reset();
            cursor.lookahead.reset();
            require(av_seek_frame(input.format, static_cast<int>(request.stream), timestamp,
                                  AVSEEK_FLAG_BACKWARD) >= 0,
                    "Cannot seek video", Error::Unavailable);
            avcodec_flush_buffers(input.codec);
            cursor.eof = false;
            seeked = true;
        };
        if (!continueForward(cursor, target))
            seekTo(seekTimestamp(cursor, target));
        const auto pull = [&] {
            if (cursor.lookahead) {
                av_frame_unref(input.frame);
                av_frame_move_ref(input.frame, cursor.lookahead.get());
                cursor.lookahead.reset();
                return true;
            }
            return next(input, request.stream, cursor.eof);
        };
        while (pull()) {
            const auto pts = input.frame->best_effort_timestamp;
            require(pts != AV_NOPTS_VALUE, "Frame timestamp unavailable", Error::Unavailable);
            if (device && input.frame->format != AV_PIX_FMT_VAAPI)
                throw HardwareMiss{};
            const bool firstAfterSeek = seeked;
            seeked = false;
            if (pts == target) {
                auto current = cloneFrame(input.frame);
                cursor.position = pts;
                if ((current->flags & AV_FRAME_FLAG_INTERLACED) == 0) {
                    cursor.previous = current;
                    return frame(input, stream, current.get());
                }
                require(!device, "Interlaced video is deinterlaced by the software decoder",
                        Error::Unavailable);
                FrameHandle following;
                if (pull())
                    following = cloneFrame(input.frame);
                const auto deinterlaced = bwdif(stream, cursor.previous, current, following);
                cursor.previous = current;
                cursor.lookahead = following;
                return frame(input, stream, deinterlaced.get());
            }
            if (pts > target && cursor.provisional) {
                if (firstAfterSeek && attempts < 10 && target - start > 0) {
                    ++attempts;
                    av_frame_unref(input.frame);
                    seekTo(std::max(start, target - backoff));
                    backoff = backoff > std::numeric_limits<std::int64_t>::max() / 2 ? backoff
                                                                                     : backoff * 2;
                    continue;
                }
                throw ProvisionalMiss{};
            }
            require(pts < target, "Exact frame timestamp unavailable (variable frame rate)",
                    Error::Unavailable);
            cursor.position = pts;
            cursor.previous = cloneFrame(input.frame);
            av_frame_unref(input.frame);
        }
        if (cursor.provisional)
            throw ProvisionalMiss{};
        throw Unavailable{Error::Unavailable, "Frame is outside the video duration"};
    };
    try {
        return decodeAt();
    } catch (const ProvisionalMiss&) {
        // The container's constant-cadence claim did not hold: index every packet and retry.
        scan();
        return decodeAt();
    }
}
// Resumable audio decode position. A retained worker keeps one so consecutive blocks continue
// from the previous block's end: N sequential blocks decode the stream once, not N times.
struct AudioCursor final {
    std::unique_ptr<Input> input;
    unsigned stream = 0;
    int rate = 0;
    std::vector<std::string> layout;
    // Both node outputs use the first video presentation as their local origin.
    std::int64_t origin = 0;
    bool eof = false;
    std::optional<std::int64_t> previousEnd;
    // Converted samples decoded past the previous block's end, starting at `pendingBegin`.
    std::int64_t pendingBegin = 0;
    std::vector<std::vector<float>> pending;
    // Sample following the last delivered block; a request starting here continues.
    std::optional<std::int64_t> next;
};
void openAudio(AudioCursor& cursor, std::unique_ptr<Input> input, const unsigned streamIndex) {
    cursor = {};
    cursor.input = std::move(input);
    cursor.stream = streamIndex;
    auto& in = *cursor.input;
    const auto& stream = *in.format->streams[streamIndex];
    require(stream.codecpar->codec_type == AVMEDIA_TYPE_AUDIO, "Requested stream is not audio");
    cursor.rate = stream.codecpar->sample_rate;
    require(cursor.rate > 0 && cursor.rate <= static_cast<int>(Limits::sampleRate),
            "Audio request exceeds limits", Error::Oversized);
    decoder(in, stream, nullptr);
    cursor.layout = channels(stream.codecpar->ch_layout);
    // Container timestamps can start well after zero (for example MPEG-TS).
    for (unsigned i = 0; i < in.format->nb_streams; ++i) {
        const auto& candidate = *in.format->streams[i];
        if (candidate.codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (candidate.start_time != AV_NOPTS_VALUE)
                cursor.origin =
                    av_rescale_q(candidate.start_time, candidate.time_base, {1, cursor.rate});
            break;
        }
    }
}
AudioBlock audio(AudioCursor& cursor, const CallRequest& request) {
    require(cursor.input && request.stream == cursor.stream, "Requested stream is not audio");
    auto& input = *cursor.input;
    const auto& stream = *input.format->streams[request.stream];
    const auto rate = cursor.rate;
    require(request.samples > 0 && request.samples <= Limits::audioSamples &&
                request.frame <=
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                        request.samples,
            "Audio request exceeds limits", Error::Oversized);
    AudioBlock result;
    result.pts = rational(static_cast<std::int64_t>(request.frame), rate);
    result.sampleRate = static_cast<std::uint32_t>(rate);
    result.channelLayout = cursor.layout;
    result.channels.resize(result.channelLayout.size());
    const auto first = static_cast<std::int64_t>(request.frame);
    // Appends converted samples beginning at local sample `begin`; returns true when the block
    // is complete. Samples past the block are kept for a continuing request.
    const auto deliver = [&](const std::int64_t begin, std::vector<std::vector<float>>& planes) {
        const auto filled = static_cast<std::int64_t>(result.channels.front().size());
        const auto available = static_cast<std::int64_t>(planes.front().size());
        if (begin + available <= first + filled)
            return false;
        require(begin <= first + filled, "Audio timestamp gap");
        const auto skip = static_cast<std::size_t>(first + filled - begin);
        const auto count = std::min(static_cast<std::size_t>(available) - skip,
                                    request.samples - static_cast<std::size_t>(filled));
        for (std::size_t c = 0; c < planes.size(); ++c)
            result.channels[c].insert(
                result.channels[c].end(), planes[c].begin() + static_cast<std::ptrdiff_t>(skip),
                planes[c].begin() + static_cast<std::ptrdiff_t>(skip + count));
        if (skip + count < static_cast<std::size_t>(available)) {
            cursor.pendingBegin = begin + static_cast<std::int64_t>(skip + count);
            for (auto& plane : planes)
                plane.erase(plane.begin(),
                            plane.begin() + static_cast<std::ptrdiff_t>(skip + count));
            cursor.pending = std::move(planes);
        }
        return result.channels.front().size() == request.samples;
    };
    const auto complete = [&] {
        require(valid(result), "Invalid audio product");
        cursor.next = first + static_cast<std::int64_t>(request.samples);
        return result;
    };
    if (!cursor.pending.empty()) {
        auto planes = std::move(cursor.pending);
        cursor.pending.clear();
        if (deliver(cursor.pendingBegin, planes))
            return complete();
    }
    while (next(input, request.stream, cursor.eof)) {
        const auto& f = *input.frame;
        require(f.sample_rate == rate && f.nb_samples > 0 &&
                    f.nb_samples <= static_cast<int>(Limits::audioSamples) &&
                    channels(f.ch_layout) == result.channelLayout,
                "Audio format changed");
        require(f.best_effort_timestamp != AV_NOPTS_VALUE, "Audio timestamp is unavailable");
        const auto absolute = av_rescale_q(f.best_effort_timestamp, stream.time_base, {1, rate});
        const auto origin = cursor.origin;
        require((origin >= 0 || absolute <= std::numeric_limits<std::int64_t>::max() + origin) &&
                    (origin <= 0 || absolute >= std::numeric_limits<std::int64_t>::min() + origin),
                "Audio timestamp origin exceeds limits", Error::Oversized);
        const auto begin = absolute - origin;
        require(!cursor.previousEnd || begin >= *cursor.previousEnd,
                "Overlapping audio timestamps");
        require(begin <= std::numeric_limits<std::int64_t>::max() - f.nb_samples,
                "Audio timestamp exceeds limits", Error::Oversized);
        const auto end = begin + f.nb_samples;
        cursor.previousEnd = end;
        if (end <= first + static_cast<std::int64_t>(result.channels.front().size())) {
            av_frame_unref(input.frame);
            continue;
        }
        if (!input.resample) {
            require(swr_alloc_set_opts2(&input.resample, &f.ch_layout, AV_SAMPLE_FMT_FLTP, rate,
                                        &f.ch_layout, static_cast<AVSampleFormat>(f.format), rate,
                                        0, nullptr) >= 0 &&
                        swr_init(input.resample) >= 0,
                    "Audio conversion unavailable", Error::Unavailable);
        }
        std::vector<std::vector<float>> planes(
            result.channels.size(), std::vector<float>(static_cast<std::size_t>(f.nb_samples)));
        std::vector<std::uint8_t*> outputs;
        outputs.reserve(planes.size());
        for (auto& plane : planes)
            outputs.push_back(reinterpret_cast<std::uint8_t*>(plane.data()));
        const auto converted =
            swr_convert(input.resample, outputs.data(), f.nb_samples,
                        const_cast<const std::uint8_t**>(f.extended_data), f.nb_samples);
        require(converted == f.nb_samples, "Audio conversion changed sample count");
        av_frame_unref(input.frame);
        if (deliver(begin, planes))
            return complete();
    }
    throw Unavailable{Error::Unavailable, "Audio block is outside the stream duration"};
}
std::unique_ptr<Input> openInput(const CallRequest& request) {
    auto input = std::make_unique<Input>();
    if (request.snapshotBytes != 0) {
        auto opened = platform::WorkerSnapshotInput::open(request.snapshotBytes);
        auto* snapshot = std::get_if<std::unique_ptr<platform::WorkerSnapshotInput>>(&opened);
        require(snapshot != nullptr, "Immutable input capability is absent or invalid",
                Error::IdentityMismatch);
        input->snapshot = std::move(*snapshot);
        auto* buffer = static_cast<unsigned char*>(av_malloc(65536));
        require(buffer != nullptr, "Input buffer allocation failed", Error::Oversized);
        input->io =
            avio_alloc_context(buffer, 65536, 0, input.get(), snapshotRead, nullptr, snapshotSeek);
        if (!input->io)
            av_free(buffer);
        require(input->io != nullptr, "Input context allocation failed", Error::Oversized);
    }
    require(input->packet && input->frame && input->transfer, "Media allocation failed",
            Error::Oversized);
    input->format = avformat_alloc_context();
    require(input->format != nullptr, "Media context allocation failed", Error::Oversized);
    input->format->max_streams = Limits::entries;
    if (input->io) {
        input->format->pb = input->io;
        input->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", "mov,matroska,webm,mxf,mpegts,wav,mp3,aac", 0);
    av_dict_set(&options, "probesize", "8388608", 0);
    av_dict_set(&options, "analyzeduration", "5000000", 0);
    const auto opened = avformat_open_input(
        &input->format, input->io ? nullptr : request.source.c_str(), nullptr, &options);
    av_dict_free(&options);
    require(opened >= 0, "Malformed or unsupported media input");
    require(avformat_find_stream_info(input->format, nullptr) >= 0 &&
                input->format->nb_streams > 0 && input->format->nb_streams <= Limits::entries,
            "Invalid media streams");
    return input;
}
} // namespace
namespace {
// Opens `name` on `node` with a small NV12 VA-API frame pool of `width` x `height`. Success means
// the driver exposes the encode entry point, not merely the device.
bool vaapiEncoderOpensAt(const char* name, const std::string& node, const int width,
                         const int height) {
    const auto* codec = avcodec_find_encoder_by_name(name);
    if (codec == nullptr)
        return false;
    AVBufferRef* device = nullptr;
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node.c_str(), nullptr, 0) < 0)
        return false;
    AVBufferRef* frames = av_hwframe_ctx_alloc(device);
    AVCodecContext* context = avcodec_alloc_context3(codec);
    bool opened = false;
    if (frames != nullptr && context != nullptr) {
        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
        framesContext->format = AV_PIX_FMT_VAAPI;
        framesContext->sw_format = AV_PIX_FMT_NV12;
        framesContext->width = width;
        framesContext->height = height;
        framesContext->initial_pool_size = 2;
        if (av_hwframe_ctx_init(frames) >= 0) {
            context->width = width;
            context->height = height;
            context->time_base = {1, 24};
            context->framerate = {24, 1};
            context->pix_fmt = AV_PIX_FMT_VAAPI;
            context->hw_frames_ctx = av_buffer_ref(frames);
            opened = avcodec_open2(context, codec, nullptr) >= 0;
        }
    }
    avcodec_free_context(&context);
    av_buffer_unref(&frames);
    av_buffer_unref(&device);
    return opened;
}
// Issue #394: drivers reject pictures outside their encode limits (AMD's Mesa driver reports
// 128..4096), so a 64x64 probe called a working encoder unavailable. Probe at sizes every
// qualified encoder accepts: 256x256 first, then 1280x720 for a driver with a larger minimum.
bool vaapiEncoderOpens(const char* name, const std::string& node) {
    return vaapiEncoderOpensAt(name, node, 256, 256) || vaapiEncoderOpensAt(name, node, 1280, 720);
}
} // namespace
bool probeVaapiEncode() {
    std::vector<std::string> nodes;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", error))
        if (entry.path().filename().string().starts_with("renderD"))
            nodes.push_back(entry.path().string());
    std::ranges::sort(nodes);
    std::string text = "bloom-vaapi-probe-v1\n";
    for (const auto& node : nodes) {
        if (node.find_first_of(" \n") != std::string::npos)
            continue;
        text += node + (vaapiEncoderOpens("h264_vaapi", node) ? " 1" : " 0") +
                (vaapiEncoderOpens("hevc_vaapi", node) ? " 1" : " 0") + "\n";
    }
    return std::fwrite(text.data(), 1, text.size(), stdout) == text.size() &&
           std::fflush(stdout) == 0;
}
bool hardwareEncodeAvailable() {
    if (avcodec_find_encoder_by_name("h264_vaapi") == nullptr &&
        avcodec_find_encoder_by_name("hevc_vaapi") == nullptr)
        return false;
    // #369: any render node, not only the first; the encode request names the probed node.
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", error)) {
        if (!entry.path().filename().string().starts_with("renderD"))
            continue;
        AVBufferRef* device = nullptr;
        const auto status = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI,
                                                   entry.path().c_str(), nullptr, 0);
        av_buffer_unref(&device);
        if (status >= 0)
            return true;
    }
    return false;
}
} // namespace bloom::media::ffmpeg
#include "ffmpeg_provider_decode.ipp"
#include "ffmpeg_provider_encode.ipp"
#ifdef BLOOM_MEDIA_FIXTURE_GENERATOR
#include "ffmpeg_provider_fixtures.ipp"
#endif
