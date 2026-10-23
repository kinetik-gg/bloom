#include "ffmpeg_provider.hpp"
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
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
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
    std::array<int, 5> scaleKey{};
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
            s.width = static_cast<std::uint32_t>(p.width);
            s.height = static_cast<std::uint32_t>(p.height);
            const auto rate = av_guess_frame_rate(input.format, input.format->streams[i], nullptr);
            require(rate.num > 0 && rate.den > 0, "Video rate unavailable", Error::Unavailable);
            s.rate = rational(rate.num, rate.den);
            const auto* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(p.format));
            s.pixelFormat = name ? name : "unknown";
            s.format =
                p.format == AV_PIX_FMT_YUV420P ? PixelFormat::Yuv420p8 : PixelFormat::Yuva444p16;
            s.colour = colour(p);
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
// Thrown when a VA-API decoder silently produced software frames for this device.
struct HardwareMiss final {};
// Bounded software decoder parallelism. Frame threads keep extra reference and output frames in
// flight, so the count is sized against half of the 2 GiB media-v1 worker address space as well
// as the machine. Intra-only codecs use slice threads: a random-access request then waits for one
// frame rather than for a pipeline of following frames.
void configureThreads(AVCodecContext& codec, const AVCodecParameters& p) {
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(p.format));
    const std::uint64_t sampleBytes = desc && desc->comp[0].depth > 8 ? 2U : 1U;
    const auto frameBytes = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::max(p.width, 1)) *
               static_cast<std::uint64_t>(std::max(p.height, 1)) * 3U * sampleBytes);
    constexpr std::uint64_t budget = 1ULL << 30U;
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
    const auto* codec = avcodec_find_decoder(stream.codecpar->codec_id);
    require(codec != nullptr, "Codec is not in the intake", Error::Unavailable);
    input.codec = avcodec_alloc_context3(codec);
    require(input.codec != nullptr, "Decoder allocation failed", Error::Oversized);
    require(avcodec_parameters_to_context(input.codec, stream.codecpar) >= 0,
            "Invalid codec parameters");
    input.codec->max_pixels = Limits::pixels;
    input.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
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
FrameProduct frame(Input& input, const AVStream& stream) {
    auto* decoded = input.frame;
    if (decoded->format == AV_PIX_FMT_VAAPI) {
        av_frame_unref(input.transfer);
        require(av_hwframe_transfer_data(input.transfer, decoded, 0) >= 0,
                "Hardware download failed");
        require(av_frame_copy_props(input.transfer, decoded) >= 0, "Hardware metadata copy failed");
        decoded = input.transfer;
    }
    extent(decoded->width, decoded->height);
    require((decoded->flags & AV_FRAME_FLAG_INTERLACED) == 0,
            "Interlaced video is not qualified for this preview path", Error::Unavailable);
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
    const bool direct = (format == AV_PIX_FMT_YUV420P && leftSited) || nv12;
    product.format = direct ? PixelFormat::Yuv420p8 : PixelFormat::Yuva444p16;
    std::array<std::uint8_t*, 4> data{};
    std::array<int, 4> strides{};
    for (unsigned i = 0; i < (direct ? 3U : 4U); ++i) {
        CpuPlane plane;
        plane.width = static_cast<std::uint32_t>(direct && i != 0 ? (decoded->width + 1) / 2
                                                                  : decoded->width);
        plane.height = static_cast<std::uint32_t>(direct && i != 0 ? (decoded->height + 1) / 2
                                                                   : decoded->height);
        plane.stride = plane.width * (direct ? 1U : 2U);
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
        const auto* desc = av_pix_fmt_desc_get(format);
        require(desc && (desc->flags & AV_PIX_FMT_FLAG_RGB) == 0,
                "RGB codec format is not qualified", Error::Unavailable);
        // Filtered chroma reconstruction honouring the frame's chroma siting. Source and
        // destination share coefficients and range, so no matrix or range change occurs. The
        // context is reused across a retained decoder's frames while the layout is unchanged.
        const int fullRange = product.colour.range == AVCOL_RANGE_JPEG ? 1 : 0;
        const std::array<int, 5> key{decoded->width, decoded->height, decoded->format, fullRange,
                                     static_cast<int>(siting)};
        if (!input.scale || input.scaleKey != key) {
            sws_freeContext(input.scale);
            input.scale = sws_alloc_context();
            require(input.scale != nullptr, "Pixel layout unavailable", Error::Oversized);
            int horizontal = 0, vertical = 0;
            require(av_chroma_location_enum_to_pos(&horizontal, &vertical, siting) >= 0,
                    "Chroma siting unavailable", Error::Unavailable);
            auto* options = static_cast<void*>(input.scale);
            require(av_opt_set_int(options, "srcw", decoded->width, 0) >= 0 &&
                        av_opt_set_int(options, "srch", decoded->height, 0) >= 0 &&
                        av_opt_set_int(options, "src_format", decoded->format, 0) >= 0 &&
                        av_opt_set_int(options, "dstw", decoded->width, 0) >= 0 &&
                        av_opt_set_int(options, "dsth", decoded->height, 0) >= 0 &&
                        av_opt_set_int(options, "dst_format", AV_PIX_FMT_YUVA444P16LE, 0) >= 0 &&
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
    bool eof = false;
};

// A forward request decodes on from the current position when no later random-access point
// precedes the target: seeking would restart from a keyframe at or before where the decoder
// already is, so continuing is never more work. Backward requests and requests past a later
// keyframe seek to the keyframe at or before the target.
bool continueForward(const VideoCursor& cursor, const std::int64_t target) {
    if (!cursor.position || *cursor.position >= target)
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
    if (!cursor.indexed) {
        decoder(input, stream, device);
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
        for (std::size_t i = 0; i < cursor.timestamps.size(); ++i) {
            const auto expected =
                av_rescale_q(static_cast<std::int64_t>(i), av_inv_q(rate), stream.time_base);
            require(expected >= 0 && start <= std::numeric_limits<std::int64_t>::max() - expected,
                    "Frame timestamp overflow");
            require(cursor.timestamps[i] == start + expected,
                    "Variable frame rate is not qualified for ordinal preview", Error::Unavailable);
        }
        cursor.indexed = true;
    }
    require(request.frame < cursor.timestamps.size(), "Frame is outside the video duration",
            Error::Unavailable);
    const auto target = cursor.timestamps[request.frame];
    av_frame_unref(input.frame);
    if (!continueForward(cursor, target)) {
        cursor.position.reset();
        require(av_seek_frame(input.format, static_cast<int>(request.stream),
                              seekTimestamp(cursor, target), AVSEEK_FLAG_BACKWARD) >= 0,
                "Cannot seek video", Error::Unavailable);
        avcodec_flush_buffers(input.codec);
        cursor.eof = false;
    }
    while (next(input, request.stream, cursor.eof)) {
        const auto pts = input.frame->best_effort_timestamp;
        require(pts != AV_NOPTS_VALUE, "Frame timestamp unavailable", Error::Unavailable);
        if (device && input.frame->format != AV_PIX_FMT_VAAPI)
            throw HardwareMiss{};
        if (pts == target) {
            auto product = frame(input, stream);
            cursor.position = pts;
            return product;
        }
        require(pts < target, "Exact frame timestamp unavailable (variable frame rate)",
                Error::Unavailable);
        cursor.position = pts;
        av_frame_unref(input.frame);
    }
    throw Unavailable{Error::Unavailable, "Frame is outside the video duration"};
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
bool hardwareEncodeAvailable() {
    const auto* codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (codec == nullptr)
        return false;
    AVBufferRef* device = nullptr;
    const auto status =
        av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr, 0);
    av_buffer_unref(&device);
    return status >= 0;
}
} // namespace bloom::media::ffmpeg
#include "ffmpeg_provider_decode.ipp"
#include "ffmpeg_provider_encode.ipp"
#ifdef BLOOM_MEDIA_FIXTURE_GENERATOR
#include "ffmpeg_provider_fixtures.ipp"
#endif
