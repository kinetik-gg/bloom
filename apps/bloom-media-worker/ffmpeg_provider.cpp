#include "ffmpeg_provider.hpp"
#include <algorithm>
#include <array>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
namespace bloom::media::ffmpeg {
namespace {
using namespace provider;
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
    SwrContext* resample = nullptr;
    ~Input() {
        swr_free(&resample);
        sws_freeContext(scale);
        av_frame_free(&transfer);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
};
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
AVPixelFormat hardwareFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (const auto* p = formats; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    return AV_PIX_FMT_NONE;
}
void decoder(Input& input, const AVStream& stream, bool hardware) {
    const auto* codec = avcodec_find_decoder(stream.codecpar->codec_id);
    require(codec != nullptr, "Codec is not in the intake", Error::Unavailable);
    input.codec = avcodec_alloc_context3(codec);
    require(input.codec != nullptr, "Decoder allocation failed", Error::Oversized);
    require(avcodec_parameters_to_context(input.codec, stream.codecpar) >= 0,
            "Invalid codec parameters");
    input.codec->thread_count = 1;
    input.codec->max_pixels = Limits::pixels;
    input.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    if (hardware) {
        require(av_hwdevice_ctx_create(&input.codec->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                       "/dev/dri/renderD128", nullptr, 0) >= 0,
                "VA-API device unavailable", Error::Unavailable);
        input.codec->get_format = hardwareFormat;
    }
    require(avcodec_open2(input.codec, codec, nullptr) >= 0, "Decoder refused stream",
            Error::Unavailable);
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
FrameProduct frame(Input& input, const AVStream& stream, bool hardware) {
    auto* decoded = input.frame;
    if (hardware) {
        require(decoded->format == AV_PIX_FMT_VAAPI, "Hardware decode not active",
                Error::Unavailable);
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
    const bool direct = format == AV_PIX_FMT_YUV420P;
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
    if (direct) {
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
        input.scale = sws_getContext(decoded->width, decoded->height, format, decoded->width,
                                     decoded->height, AV_PIX_FMT_YUVA444P16LE,
                                     SWS_POINT | SWS_BITEXACT, nullptr, nullptr, nullptr);
        require(input.scale != nullptr, "Pixel layout unavailable", Error::Unavailable);
        const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        const int fullRange = product.colour.range == AVCOL_RANGE_JPEG ? 1 : 0;
        require(sws_setColorspaceDetails(input.scale, coefficients, fullRange, coefficients,
                                         fullRange, 0, 1 << 16, 1 << 16) >= 0,
                "Pixel range preservation unavailable", Error::Unavailable);
        require(sws_scale(input.scale, decoded->data, decoded->linesize, 0, decoded->height,
                          data.data(), strides.data()) == decoded->height,
                "Pixel layout conversion failed");
    }
    for (auto& plane : product.planes)
        plane.digest = digestBytes(plane.bytes);
    require(valid(product), "Invalid video product");
    return product;
}
FrameProduct video(Input& input, const CallRequest& request, bool hardware) {
    const auto& stream = *input.format->streams[request.stream];
    require(stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO, "Requested stream is not video");
    extent(stream.codecpar->width, stream.codecpar->height);
    decoder(input, stream, hardware);
    const auto rate =
        av_guess_frame_rate(input.format, input.format->streams[request.stream], nullptr);
    require(rate.num > 0 && rate.den > 0 && request.frame < Limits::indexEntries,
            "Frame index exceeds limits", Error::Oversized);
    const auto start = stream.start_time == AV_NOPTS_VALUE ? 0 : stream.start_time;
    // Qualify the packet presentation cadence before using a CFR ordinal. Without
    // this check a VFR stream could coincidentally contain the requested timestamp
    // while assigning it a different frame number. The admitted codecs/demuxers
    // produce one access unit per packet; missing or duplicate PTS are unavailable.
    std::vector<std::int64_t> timestamps;
    int read = 0;
    while ((read = av_read_frame(input.format, input.packet)) >= 0) {
        if (input.packet->stream_index == static_cast<int>(request.stream)) {
            require(input.packet->pts != AV_NOPTS_VALUE, "Video packet timestamp unavailable",
                    Error::Unavailable);
            if (input.packet->pts >= start) {
                require(timestamps.size() < Limits::indexEntries, "Frame index exceeds limits",
                        Error::Oversized);
                timestamps.push_back(input.packet->pts);
            }
        }
        av_packet_unref(input.packet);
    }
    require(read == AVERROR_EOF, "Malformed video packets");
    std::ranges::sort(timestamps);
    require(request.frame < timestamps.size(), "Frame is outside the video duration",
            Error::Unavailable);
    for (std::size_t i = 0; i < timestamps.size(); ++i) {
        const auto expected =
            av_rescale_q(static_cast<std::int64_t>(i), av_inv_q(rate), stream.time_base);
        require(expected >= 0 && start <= std::numeric_limits<std::int64_t>::max() - expected,
                "Frame timestamp overflow");
        require(timestamps[i] == start + expected,
                "Variable frame rate is not qualified for ordinal preview", Error::Unavailable);
    }
    const auto offset =
        av_rescale_q(static_cast<std::int64_t>(request.frame), av_inv_q(rate), stream.time_base);
    require(offset >= 0 && start <= std::numeric_limits<std::int64_t>::max() - offset,
            "Frame timestamp overflow");
    const auto target = start + offset;
    require(av_seek_frame(input.format, static_cast<int>(request.stream), target,
                          AVSEEK_FLAG_BACKWARD) >= 0,
            "Cannot seek video", Error::Unavailable);
    avcodec_flush_buffers(input.codec);
    bool eof = false;
    while (next(input, request.stream, eof)) {
        const auto pts = input.frame->best_effort_timestamp;
        require(pts != AV_NOPTS_VALUE, "Frame timestamp unavailable", Error::Unavailable);
        if (pts == target)
            return frame(input, stream, hardware);
        require(pts < target, "Exact frame timestamp unavailable (variable frame rate)",
                Error::Unavailable);
        av_frame_unref(input.frame);
    }
    throw Unavailable{Error::Unavailable, "Frame is outside the video duration"};
}
AudioBlock audio(Input& input, const CallRequest& request) {
    const auto& stream = *input.format->streams[request.stream];
    require(stream.codecpar->codec_type == AVMEDIA_TYPE_AUDIO, "Requested stream is not audio");
    const auto rate = stream.codecpar->sample_rate;
    require(rate > 0 && rate <= static_cast<int>(Limits::sampleRate) && request.samples > 0 &&
                request.samples <= Limits::audioSamples &&
                request.frame <=
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                        request.samples,
            "Audio request exceeds limits", Error::Oversized);
    decoder(input, stream, false);
    AudioBlock result;
    result.pts = rational(static_cast<std::int64_t>(request.frame), rate);
    result.sampleRate = static_cast<std::uint32_t>(rate);
    result.channelLayout = channels(stream.codecpar->ch_layout);
    result.channels.resize(result.channelLayout.size());
    // Decode from the beginning to retain codec priming and exact sample placement.
    // The supervisor watchdog bounds long seeks; no approximate audio is published.
    bool eof = false;
    const auto first = static_cast<std::int64_t>(request.frame);
    // Both node outputs use the first video presentation as their local origin.
    // Container timestamps can start well after zero (for example MPEG-TS).
    std::int64_t origin = 0;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        const auto& candidate = *input.format->streams[i];
        if (candidate.codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (candidate.start_time != AV_NOPTS_VALUE)
                origin = av_rescale_q(candidate.start_time, candidate.time_base, {1, rate});
            break;
        }
    }
    std::optional<std::int64_t> previousEnd;
    while (next(input, request.stream, eof)) {
        const auto& f = *input.frame;
        require(f.sample_rate == rate && f.nb_samples > 0 &&
                    f.nb_samples <= static_cast<int>(Limits::audioSamples) &&
                    channels(f.ch_layout) == result.channelLayout,
                "Audio format changed");
        require(f.best_effort_timestamp != AV_NOPTS_VALUE, "Audio timestamp is unavailable");
        const auto absolute = av_rescale_q(f.best_effort_timestamp, stream.time_base, {1, rate});
        require((origin >= 0 || absolute <= std::numeric_limits<std::int64_t>::max() + origin) &&
                    (origin <= 0 || absolute >= std::numeric_limits<std::int64_t>::min() + origin),
                "Audio timestamp origin exceeds limits", Error::Oversized);
        const auto begin = absolute - origin;
        require(!previousEnd || begin >= *previousEnd, "Overlapping audio timestamps");
        require(begin <= std::numeric_limits<std::int64_t>::max() - f.nb_samples,
                "Audio timestamp exceeds limits", Error::Oversized);
        const auto end = begin + f.nb_samples;
        previousEnd = end;
        if (end <= first) {
            av_frame_unref(input.frame);
            continue;
        }
        require(begin <= first + static_cast<std::int64_t>(result.channels.front().size()),
                "Audio timestamp gap");
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
        const auto skip = static_cast<std::size_t>(std::max<std::int64_t>(0, first - begin));
        const auto count = std::min(static_cast<std::size_t>(converted) - skip,
                                    request.samples - result.channels.front().size());
        for (std::size_t c = 0; c < planes.size(); ++c)
            result.channels[c].insert(
                result.channels[c].end(), planes[c].begin() + static_cast<std::ptrdiff_t>(skip),
                planes[c].begin() + static_cast<std::ptrdiff_t>(skip + count));
        if (result.channels.front().size() == request.samples) {
            require(valid(result), "Invalid audio product");
            return result;
        }
        av_frame_unref(input.frame);
    }
    throw Unavailable{Error::Unavailable, "Audio block is outside the stream duration"};
}
} // namespace
provider::Payload call(const provider::CallRequest& request, bool hardware) {
    try {
        av_log_set_level(AV_LOG_ERROR);
        const auto h = provider::ffmpegHandshake(hardware);
        require(
            std::ranges::any_of(h.declarations,
                                [&](const auto& d) { return d.capability == request.capability; }),
            "Unsupported FFmpeg capability", provider::Error::Unavailable);
        const auto [digest, bytes] = sourceIdentity(request.source);
        if (request.capability.role != provider::Role::Probe)
            require(digest == request.sourceDigest, "Media source content changed",
                    provider::Error::SourceChanged);
        Input input;
        require(input.packet && input.frame && input.transfer, "Media allocation failed",
                provider::Error::Oversized);
        input.format = avformat_alloc_context();
        require(input.format != nullptr, "Media context allocation failed",
                provider::Error::Oversized);
        input.format->max_streams = provider::Limits::entries;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        av_dict_set(&options, "format_whitelist", "mov,matroska,webm,mxf,mpegts,wav,mp3,aac", 0);
        av_dict_set(&options, "probesize", "8388608", 0);
        av_dict_set(&options, "analyzeduration", "5000000", 0);
        const auto opened =
            avformat_open_input(&input.format, request.source.c_str(), nullptr, &options);
        av_dict_free(&options);
        require(opened >= 0, "Malformed or unsupported media input");
        require(avformat_find_stream_info(input.format, nullptr) >= 0 &&
                    input.format->nb_streams > 0 &&
                    input.format->nb_streams <= provider::Limits::entries,
                "Invalid media streams");
        provider::Payload result;
        if (request.capability.role == provider::Role::Probe)
            result = probe(input, digest, bytes);
        else if (request.capability.role == provider::Role::DemuxIndex)
            result = index(input);
        else {
            require(request.stream < input.format->nb_streams, "Stream does not exist");
            require(request.capability.codec ==
                        avcodec_get_name(input.format->streams[request.stream]->codecpar->codec_id),
                    "Stream codec does not match pinned capability",
                    provider::Error::IdentityMismatch);
            result = request.capability.role == provider::Role::VideoDecode
                         ? provider::Payload(video(input, request, hardware))
                         : provider::Payload(audio(input, request));
        }
        require(sourceIdentity(request.source).first == digest, "Source changed during media read",
                provider::Error::SourceChanged);
        return result;
    } catch (const provider::Unavailable& error) {
        return error;
    } catch (const std::bad_alloc&) {
        return provider::Unavailable{provider::Error::Oversized, "Media allocation limit"};
    } catch (const std::filesystem::filesystem_error&) {
        return provider::Unavailable{provider::Error::Io, "Media file unavailable"};
    }
}
} // namespace bloom::media::ffmpeg
#ifdef BLOOM_MEDIA_FIXTURE_GENERATOR
#include "ffmpeg_provider_fixtures.ipp"
#endif
