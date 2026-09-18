// Test-only synthetic media. Included by the sole FFmpeg-bearing translation unit.
namespace bloom::media::ffmpeg {
namespace {
struct Bits {
    std::vector<std::uint8_t> data;
    unsigned used = 0;
    void bit(unsigned v) {
        if (used == 0)
            data.push_back(0);
        data.back() |= static_cast<std::uint8_t>((v & 1U) << (7U - used));
        used = (used + 1U) % 8U;
    }
    void number(unsigned v, unsigned n) {
        for (unsigned i = n; i > 0; --i)
            bit(v >> (i - 1U));
    }
    void ue(unsigned v) {
        unsigned n = 0;
        for (auto value = v + 1; value > 1; value >>= 1U)
            ++n;
        for (unsigned i = 0; i < n; ++i)
            bit(0);
        number(v + 1U, n + 1U);
    }
    void align() {
        while (used != 0)
            bit(0);
    }
    std::vector<std::uint8_t> nal(std::uint8_t type) {
        bit(1);
        align();
        std::vector<std::uint8_t> output{0, 0, 0, 1, type};
        unsigned zeros = 0;
        for (auto v : data) {
            if (zeros == 2 && v <= 3) {
                output.push_back(3);
                zeros = 0;
            }
            output.push_back(v);
            zeros = v == 0 ? zeros + 1 : 0;
        }
        return output;
    }
};
std::vector<std::uint8_t> h264Header() {
    Bits s;
    s.number(66, 8);
    s.number(0, 8);
    s.number(10, 8);
    s.ue(0);
    s.ue(0);
    s.ue(2);
    s.ue(1);
    s.bit(0);
    s.ue(3);
    s.ue(2);
    s.bit(1);
    s.bit(1);
    s.bit(0);
    s.bit(1);
    // VUI: explicit limited-range Rec.709, with timing carried by the container.
    s.bit(0);
    s.bit(0);
    s.bit(1);
    s.number(5, 3);
    s.bit(0);
    s.bit(1);
    s.number(1, 8);
    s.number(1, 8);
    s.number(1, 8);
    s.bit(0);
    s.bit(0);
    s.bit(0);
    s.bit(0);
    s.bit(0);
    s.bit(0);
    auto output = s.nal(0x67);
    Bits p;
    p.ue(0);
    p.ue(0);
    p.bit(0);
    p.bit(0);
    p.ue(0);
    p.ue(0);
    p.ue(0);
    p.bit(0);
    p.number(0, 2);
    p.ue(0);
    p.ue(0);
    p.ue(0);
    p.bit(1);
    p.bit(0);
    p.bit(0);
    const auto pps = p.nal(0x68);
    output.insert(output.end(), pps.begin(), pps.end());
    return output;
}
unsigned luma(unsigned frame, unsigned x, unsigned y) {
    constexpr std::array<unsigned, 10> digits{0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                              0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    const auto digit = x < 32 ? frame / 10 : frame % 10;
    const auto local = x < 32 ? x - 16U : x - 34U;
    if (x >= 16 && x < 48 && y >= 10 && y < 36) {
        const auto py = y - 10;
        const auto mask = digits[digit];
        const bool horizontal = local >= 2 && local < 12;
        const bool left = local < 2, right = local >= 12 && local < 14;
        if (((mask & 1U) && horizontal && py < 2) || ((mask & 2U) && right && py >= 2 && py < 12) ||
            ((mask & 4U) && right && py >= 14 && py < 24) ||
            ((mask & 8U) && horizontal && py >= 24) ||
            ((mask & 16U) && left && py >= 14 && py < 24) ||
            ((mask & 32U) && left && py >= 2 && py < 12) ||
            ((mask & 64U) && horizontal && py >= 12 && py < 14))
            return 220;
    }
    return 16 + frame * 3;
}
std::vector<std::uint8_t> h264Frame(unsigned frame) {
    const bool key = frame % 12 == 0;
    Bits s;
    s.ue(0);
    s.ue(key ? 2 : 0);
    s.ue(0);
    s.number(frame % 12, 4);
    if (key) {
        s.ue(frame / 12);
        s.bit(0);
        s.bit(0);
    } else {
        s.bit(0);
        s.bit(0);
        s.bit(0);
    }
    s.ue(0);
    s.ue(1);
    for (unsigned mb = 0; mb < 12; ++mb) {
        if (!key)
            s.ue(0);
        s.ue(key ? 25 : 30);
        s.align();
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 16; ++x)
                s.number(luma(frame, (mb % 4) * 16 + x, (mb / 4) * 16 + y), 8);
        for (unsigned i = 0; i < 128; ++i)
            s.number(128, 8);
    }
    return s.nal(key ? 0x65 : 0x41);
}
struct Output {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    ~Output() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        if (format) {
            avio_closep(&format->pb);
            avformat_free_context(format);
        }
    }
};
void fixture(const std::filesystem::path& path, bool prores, bool alpha = false) {
    Output out;
    require(out.frame && out.packet, "Fixture allocation");
    require(avformat_alloc_output_context2(&out.format, nullptr, "mov", path.c_str()) >= 0,
            "Fixture mux allocation");
    auto* videoStream = avformat_new_stream(out.format, nullptr);
    require(videoStream, "Fixture video stream");
    videoStream->time_base = {1, 24};
    videoStream->avg_frame_rate = {24, 1};
    auto& vp = *videoStream->codecpar;
    vp.codec_type = AVMEDIA_TYPE_VIDEO;
    vp.codec_id = prores ? AV_CODEC_ID_PRORES : AV_CODEC_ID_H264;
    vp.width = 64;
    vp.height = 48;
    vp.color_primaries = AVCOL_PRI_BT709;
    vp.color_trc = AVCOL_TRC_BT709;
    vp.color_space = AVCOL_SPC_BT709;
    vp.color_range = AVCOL_RANGE_MPEG;
    av_dict_set(&videoStream->metadata, "timecode", "01:00:00:00", 0);
    if (prores) {
        const auto* encoder = avcodec_find_encoder_by_name("prores_ks");
        require(encoder, "Intake ProRes encoder missing");
        out.codec = avcodec_alloc_context3(encoder);
        require(out.codec, "Fixture encoder allocation");
        out.codec->width = 64;
        out.codec->height = 48;
        out.codec->pix_fmt = alpha ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV422P10LE;
        out.codec->time_base = {1, 24};
        out.codec->framerate = {24, 1};
        out.codec->profile = alpha ? 4 : 2;
        out.codec->thread_count = 1;
        out.codec->color_primaries = AVCOL_PRI_BT709;
        out.codec->color_trc = AVCOL_TRC_BT709;
        out.codec->colorspace = AVCOL_SPC_BT709;
        out.codec->color_range = AVCOL_RANGE_MPEG;
        out.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_BITEXACT;
        require(avcodec_open2(out.codec, encoder, nullptr) >= 0, "Fixture encoder open");
        require(avcodec_parameters_from_context(&vp, out.codec) >= 0, "Fixture encoder metadata");
        out.frame->width = 64;
        out.frame->height = 48;
        out.frame->format = out.codec->pix_fmt;
        require(av_frame_get_buffer(out.frame, 32) >= 0, "Fixture image buffer");
    } else {
        const auto header = h264Header();
        vp.extradata =
            static_cast<std::uint8_t*>(av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        require(vp.extradata, "Fixture codec header");
        std::memcpy(vp.extradata, header.data(), header.size());
        vp.extradata_size = static_cast<int>(header.size());
        vp.format = AV_PIX_FMT_YUV420P;
    }
    auto* audioStream = prores ? avformat_new_stream(out.format, nullptr) : nullptr;
    if (prores) {
        require(audioStream, "Fixture audio stream");
        audioStream->time_base = {1, 48000};
        auto& ap = *audioStream->codecpar;
        ap.codec_type = AVMEDIA_TYPE_AUDIO;
        ap.codec_id = AV_CODEC_ID_PCM_S16LE;
        ap.sample_rate = 48000;
        ap.bits_per_coded_sample = 16;
        ap.block_align = 2;
        av_channel_layout_default(&ap.ch_layout, 1);
    }
    require(avio_open(&out.format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0, "Fixture file open");
    AVDictionary* muxOptions = nullptr;
    if (!prores)
        av_dict_set(&muxOptions, "brand", "isom", 0);
    const auto headerStatus = avformat_write_header(out.format, &muxOptions);
    av_dict_free(&muxOptions);
    require(headerStatus >= 0, "Fixture mux header");
    for (unsigned n = 0; n < 48; ++n) {
        av_packet_unref(out.packet);
        if (prores) {
            require(av_frame_make_writable(out.frame) >= 0, "Fixture writable frame");
            for (unsigned plane = 0; plane < (alpha ? 4U : 3U); ++plane)
                for (unsigned y = 0; y < 48; ++y)
                    for (unsigned x = 0; x < (plane == 0 || alpha ? 64U : 32U); ++x) {
                        const auto value = plane == 0 ? luma(n, x, y) * 4 : 512U;
                        auto* pixel = out.frame->data[plane] +
                                      static_cast<std::ptrdiff_t>(y) * out.frame->linesize[plane] +
                                      static_cast<std::ptrdiff_t>(x) * 2;
                        pixel[0] = static_cast<std::uint8_t>(value & 255U);
                        pixel[1] = static_cast<std::uint8_t>(value >> 8U);
                    }
            out.frame->pts = n;
            require(avcodec_send_frame(out.codec, out.frame) >= 0 &&
                        avcodec_receive_packet(out.codec, out.packet) >= 0,
                    "Fixture ProRes encode");
        } else {
            const auto bytes = h264Frame(n);
            require(av_new_packet(out.packet, static_cast<int>(bytes.size())) >= 0,
                    "Fixture packet allocation");
            std::memcpy(out.packet->data, bytes.data(), bytes.size());
            out.packet->pts = n;
            out.packet->dts = n;
            out.packet->flags = n % 12 == 0 ? AV_PKT_FLAG_KEY : 0;
        }
        out.packet->duration = 1;
        out.packet->stream_index = videoStream->index;
        av_packet_rescale_ts(out.packet, {1, 24}, videoStream->time_base);
        require(av_interleaved_write_frame(out.format, out.packet) >= 0, "Fixture video mux");
        if (audioStream) {
            av_packet_unref(out.packet);
            require(av_new_packet(out.packet, 4000) >= 0, "Fixture audio packet");
            for (unsigned i = 0; i < 2000; ++i) {
                const auto value = static_cast<std::uint16_t>(
                    static_cast<std::int16_t>((static_cast<int>((n * 2000 + i) % 997) - 498) * 32));
                out.packet->data[static_cast<std::size_t>(i) * 2] =
                    static_cast<std::uint8_t>(value & 255U);
                out.packet->data[static_cast<std::size_t>(i) * 2 + 1] =
                    static_cast<std::uint8_t>(value >> 8U);
            }
            out.packet->pts = static_cast<std::int64_t>(n) * 2000;
            out.packet->dts = out.packet->pts;
            out.packet->duration = 2000;
            out.packet->stream_index = audioStream->index;
            out.packet->flags = AV_PKT_FLAG_KEY;
            require(av_interleaved_write_frame(out.format, out.packet) >= 0, "Fixture audio mux");
        }
    }
    require(av_write_trailer(out.format) >= 0, "Fixture mux trailer");
}
} // namespace
bool generateFixtures(const std::string& directory) {
    try {
        const std::filesystem::path path(directory);
        std::filesystem::create_directories(path);
        fixture(path / "numbered-h264.mp4", false);
        fixture(path / "numbered-prores.mov", true);
        fixture(path / "alpha-prores.mov", true, true);
        std::ofstream provenance(path / "provenance.txt");
        provenance
            << "MEDIA-3 generated-read-v1\nFFmpeg 8.1.2 qualified intake mux/ProRes encode\n"
               "H.264 baseline I_PCM macroblocks, GOP 12; ProRes 422; 64x48; 48 frames at 24/1\n"
               "Luma background 16+3*N; seven-segment frame numbers; Rec.709 limited\n"
               "Additional ProRes 4444 alpha fixture uses 512/1023 opacity\n"
               "PCM mono 48000 Hz: sample[n]=((n%997)-498)/1024\n"
               "Original synthetic content; generated at test time; no external media\n";
        const auto specification =
            provider::ffmpegHandshake().declarations.front().evidence.fixtures.toLowercaseHex();
        provenance << "Generator SHA-256: "
                   << std::string(specification.begin(), specification.end()) << '\n';
        for (const auto* name : {"numbered-h264.mp4", "numbered-prores.mov", "alpha-prores.mov"}) {
            const auto identity = sourceIdentity((path / name).string()).first.toLowercaseHex();
            provenance << name << " SHA-256: " << std::string(identity.begin(), identity.end())
                       << '\n';
        }
        return provenance.good();
    } catch (...) {
        return false;
    }
}
} // namespace bloom::media::ffmpeg
