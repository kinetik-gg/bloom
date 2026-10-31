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
// H.273 colour description written to both the H.264 VUI and the container. Absent means no
// video_signal_type in the VUI and unspecified container tags (an untagged stream).
struct FixtureColour final {
    unsigned primaries = 1, transfer = 1, matrix = 1;
    bool present = true;
};
std::vector<std::uint8_t> h264Header(const FixtureColour& colour = {}) {
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
    // VUI: explicit limited-range colour description (or none), with timing carried by the
    // container.
    s.bit(0);
    s.bit(0);
    s.bit(colour.present ? 1U : 0U);
    if (colour.present) {
        s.number(5, 3);
        s.bit(0);
        s.bit(1);
        s.number(colour.primaries, 8);
        s.number(colour.transfer, 8);
        s.number(colour.matrix, 8);
    }
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
// Issue #381: twelve uniform colour patches, one per 16x16 macroblock of the 64x48 picture, as a
// camera codes them: R'G'B' (8-bit full scale) to limited-range Y'CbCr with the clip's matrix.
// 75% bars, near black, mid grey, a skin and a sky tone, and full white.
using Patches = std::array<std::array<unsigned, 3>, 12>;
Patches colourPatches(const double kr, const double kb) {
    constexpr std::array<std::array<double, 3>, 12> rgb{{{191, 191, 191},
                                                         {191, 191, 0},
                                                         {0, 191, 191},
                                                         {0, 191, 0},
                                                         {191, 0, 191},
                                                         {191, 0, 0},
                                                         {0, 0, 191},
                                                         {16, 16, 16},
                                                         {128, 128, 128},
                                                         {224, 172, 140},
                                                         {100, 150, 210},
                                                         {255, 255, 255}}};
    Patches result{};
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        const auto r = rgb[i][0] / 255.0, g = rgb[i][1] / 255.0, b = rgb[i][2] / 255.0;
        const auto y = kr * r + (1.0 - kr - kb) * g + kb * b;
        const auto cb = (b - y) / (2.0 * (1.0 - kb)), cr = (r - y) / (2.0 * (1.0 - kr));
        result[i] = {static_cast<unsigned>(std::lround(16.0 + 219.0 * y)),
                     static_cast<unsigned>(std::lround(128.0 + 224.0 * cb)),
                     static_cast<unsigned>(std::lround(128.0 + 224.0 * cr))};
    }
    return result;
}
std::vector<std::uint8_t> h264Frame(unsigned frame, bool forceKey = false,
                                    const Patches* patches = nullptr) {
    const bool key = forceKey || frame % 12 == 0;
    Bits s;
    s.ue(0);
    s.ue(key ? 2 : 0);
    s.ue(0);
    s.number(key ? 0 : frame % 12, 4);
    if (key) {
        s.ue(forceKey ? frame % 2 : frame / 12);
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
        if (patches != nullptr) {
            // I_PCM: 256 luma samples, then the 8x8 Cb and the 8x8 Cr block.
            const auto& patch = (*patches)[mb];
            for (unsigned i = 0; i < 256; ++i)
                s.number(patch[0], 8);
            for (unsigned i = 0; i < 128; ++i)
                s.number(i < 64 ? patch[1] : patch[2], 8);
            continue;
        }
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 16; ++x)
                // The pattern repeats every 48 frames (the long fixture keeps 8-bit samples).
                s.number(luma(frame % 48U, (mb % 4) * 16 + x, (mb / 4) * 16 + y), 8);
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
void fixture(const std::filesystem::path& path, bool prores, bool alpha = false,
             const FixtureColour& colour = {}, unsigned frames = 48,
             const Patches* patches = nullptr) {
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
    vp.color_primaries =
        colour.present ? static_cast<AVColorPrimaries>(colour.primaries) : AVCOL_PRI_UNSPECIFIED;
    vp.color_trc = colour.present ? static_cast<AVColorTransferCharacteristic>(colour.transfer)
                                  : AVCOL_TRC_UNSPECIFIED;
    vp.color_space =
        colour.present ? static_cast<AVColorSpace>(colour.matrix) : AVCOL_SPC_UNSPECIFIED;
    vp.color_range = colour.present ? AVCOL_RANGE_MPEG : AVCOL_RANGE_UNSPECIFIED;
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
        const auto header = h264Header(colour);
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
    for (unsigned n = 0; n < frames; ++n) {
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
            const auto bytes = h264Frame(n, false, patches);
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
// Issue #367 timing/metadata fixtures, 64x48 with the numbered luma pattern, and a 1280x720
// ProRes stream with no colour tags (the HD case of the untagged-colour convention). ChromaEdge
// (issue #393) is a ProRes 422 HQ picture of flat grey luma whose Cr steps from 768 to 384 at row
// 24, on an 8x8 block boundary so each block is flat and decodes to one value per side.
enum class Special { Vfr, Rotated, Interlaced, Rgb, Anamorphic, UntaggedHd, EightK, ChromaEdge };
void special(const std::filesystem::path& path, const Special kind) {
    // The 8K UHD fixture (issue #352) is a tagged Rec.709 ProRes 422 stream laid out like the HD
    // one; it exceeds the former 16,777,216-pixel provider limit.
    const bool eightK = kind == Special::EightK;
    const bool hd = kind == Special::UntaggedHd || eightK;
    const bool untagged = kind == Special::UntaggedHd;
    const unsigned width = eightK ? 7680U : hd ? 1280U : 64U;
    const unsigned height = eightK ? 4320U : hd ? 720U : 48U;
    Output out;
    require(out.frame && out.packet, "Fixture allocation");
    require(avformat_alloc_output_context2(&out.format, nullptr, "mov", path.c_str()) >= 0,
            "Fixture mux allocation");
    auto* stream = avformat_new_stream(out.format, nullptr);
    require(stream, "Fixture video stream");
    stream->time_base = {1, 24};
    stream->avg_frame_rate = {24, 1};
    auto& vp = *stream->codecpar;
    vp.codec_type = AVMEDIA_TYPE_VIDEO;
    vp.width = static_cast<int>(width);
    vp.height = static_cast<int>(height);
    vp.color_primaries = untagged ? AVCOL_PRI_UNSPECIFIED : AVCOL_PRI_BT709;
    vp.color_trc = untagged ? AVCOL_TRC_UNSPECIFIED : AVCOL_TRC_BT709;
    vp.color_space = kind == Special::Rgb ? AVCOL_SPC_RGB
                     : untagged           ? AVCOL_SPC_UNSPECIFIED
                                          : AVCOL_SPC_BT709;
    vp.color_range = kind == Special::Rgb ? AVCOL_RANGE_JPEG
                     : untagged           ? AVCOL_RANGE_UNSPECIFIED
                                          : AVCOL_RANGE_MPEG;
    const bool chromaEdge = kind == Special::ChromaEdge;
    const bool encoded = kind == Special::Interlaced || kind == Special::Rgb || hd || chromaEdge;
    if (encoded) {
        const auto* encoder =
            avcodec_find_encoder_by_name(kind == Special::Rgb ? "tiff" : "prores_ks");
        require(encoder, "Intake fixture encoder missing");
        out.codec = avcodec_alloc_context3(encoder);
        require(out.codec, "Fixture encoder allocation");
        out.codec->width = static_cast<int>(width);
        out.codec->height = static_cast<int>(height);
        out.codec->pix_fmt = kind == Special::Rgb ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_YUV422P10LE;
        out.codec->time_base = {1, 24};
        out.codec->framerate = {24, 1};
        out.codec->thread_count = 1;
        out.codec->color_primaries = vp.color_primaries;
        out.codec->color_trc = vp.color_trc;
        out.codec->colorspace = vp.color_space;
        out.codec->color_range = vp.color_range;
        out.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_BITEXACT;
        if (hd)
            out.codec->profile = 2;
        if (chromaEdge)
            out.codec->profile = 3;
        if (kind == Special::Interlaced) {
            out.codec->profile = 2;
            out.codec->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
            out.codec->field_order = AV_FIELD_TT;
        }
        require(avcodec_open2(out.codec, encoder, nullptr) >= 0, "Fixture encoder open");
        require(avcodec_parameters_from_context(&vp, out.codec) >= 0, "Fixture encoder metadata");
        out.frame->width = static_cast<int>(width);
        out.frame->height = static_cast<int>(height);
        out.frame->format = out.codec->pix_fmt;
        require(av_frame_get_buffer(out.frame, 32) >= 0, "Fixture image buffer");
    } else {
        vp.codec_id = AV_CODEC_ID_H264;
        const auto header = h264Header();
        vp.extradata =
            static_cast<std::uint8_t*>(av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        require(vp.extradata, "Fixture codec header");
        std::memcpy(vp.extradata, header.data(), header.size());
        vp.extradata_size = static_cast<int>(header.size());
        vp.format = AV_PIX_FMT_YUV420P;
        if (kind == Special::Anamorphic) {
            // 2:1 sample aspect ratio: 64x48 coded pixels display as 128x48.
            stream->sample_aspect_ratio = {2, 1};
            vp.sample_aspect_ratio = {2, 1};
        }
        if (kind == Special::Rotated) {
            // Display matrix: rotate the coded picture 90 degrees clockwise for display.
            auto* side =
                av_packet_side_data_new(&vp.coded_side_data, &vp.nb_coded_side_data,
                                        AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t), 0);
            require(side, "Fixture display matrix");
            av_display_rotation_set(reinterpret_cast<std::int32_t*>(side->data), -90.0);
        }
    }
    require(avio_open(&out.format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0, "Fixture file open");
    AVDictionary* muxOptions = nullptr;
    if (!encoded)
        av_dict_set(&muxOptions, "brand", "isom", 0);
    const auto headerStatus = avformat_write_header(out.format, &muxOptions);
    av_dict_free(&muxOptions);
    require(headerStatus >= 0, "Fixture mux header");
    // Variable cadence: frames at 0,1,2,4,5,8,9,10,14,15,16,20 (24/1 timebase), each one frame
    // number n showing its own presentation slot; every frame is an IDR so any cadence decodes.
    constexpr std::array<unsigned, 12> vfr{0, 1, 2, 4, 5, 8, 9, 10, 14, 15, 16, 20};
    for (unsigned n = 0; n < (eightK ? 2U : 12U); ++n) {
        av_packet_unref(out.packet);
        const auto pts = kind == Special::Vfr ? vfr[n] : n;
        if (encoded) {
            require(av_frame_make_writable(out.frame) >= 0, "Fixture writable frame");
            for (unsigned y = 0; y < height; ++y) {
                auto* row =
                    out.frame->data[0] + static_cast<std::ptrdiff_t>(y) * out.frame->linesize[0];
                for (unsigned x = 0; x < width; ++x) {
                    if (kind == Special::Rgb) {
                        // R carries the numbered luma pattern, G a horizontal ramp, B constant.
                        const std::array<unsigned, 3> rgb{luma(n, x, y) * 256U, x * 1024U, 32768U};
                        for (unsigned c = 0; c < 3; ++c) {
                            row[x * 6 + c * 2] = static_cast<std::uint8_t>(rgb[c] & 255U);
                            row[x * 6 + c * 2 + 1] = static_cast<std::uint8_t>(rgb[c] >> 8U);
                        }
                    } else if (chromaEdge) {
                        row[x * 2] = 0;
                        row[x * 2 + 1] = 2;
                    } else if (hd) {
                        // The numbered pattern tiled across the HD picture.
                        const auto value = luma(n, x % 64U, y % 48U) * 4U;
                        row[x * 2] = static_cast<std::uint8_t>(value & 255U);
                        row[x * 2 + 1] = static_cast<std::uint8_t>(value >> 8U);
                    } else {
                        // Interlaced: the top field carries frame n; the bottom field is a
                        // later instant that flickers between 200 and 40 (field-rate motion).
                        const auto value = (y % 2 == 0 ? luma(n, x, y) : n % 2 ? 40U : 200U) * 4U;
                        row[x * 2] = static_cast<std::uint8_t>(value & 255U);
                        row[x * 2 + 1] = static_cast<std::uint8_t>(value >> 8U);
                    }
                }
            }
            // Neutral 10-bit chroma (512) for the 4:2:2 ProRes fixtures; the chroma-edge fixture
            // steps Cr from 768 (rows 0-23) to 384 (rows 24-47).
            if (kind == Special::Interlaced || hd || chromaEdge)
                for (unsigned plane = 1; plane < 3; ++plane)
                    for (unsigned y = 0; y < height; ++y)
                        for (unsigned x = 0; x < width / 2U; ++x) {
                            auto* pixel =
                                out.frame->data[plane] +
                                static_cast<std::ptrdiff_t>(y) * out.frame->linesize[plane] +
                                static_cast<std::ptrdiff_t>(x) * 2;
                            const unsigned value =
                                chromaEdge && plane == 2 ? (y < 24U ? 768U : 384U) : 512U;
                            pixel[0] = static_cast<std::uint8_t>(value & 255U);
                            pixel[1] = static_cast<std::uint8_t>(value >> 8U);
                        }
            out.frame->flags = kind == Special::Interlaced
                                   ? AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST
                                   : 0;
            out.frame->pts = pts;
            require(avcodec_send_frame(out.codec, out.frame) >= 0 &&
                        avcodec_receive_packet(out.codec, out.packet) >= 0,
                    "Fixture special encode");
        } else {
            const auto bytes = h264Frame(pts, true);
            require(av_new_packet(out.packet, static_cast<int>(bytes.size())) >= 0,
                    "Fixture packet allocation");
            std::memcpy(out.packet->data, bytes.data(), bytes.size());
            out.packet->flags = AV_PKT_FLAG_KEY;
        }
        out.packet->pts = pts;
        out.packet->dts = pts;
        out.packet->duration = 1;
        out.packet->stream_index = stream->index;
        av_packet_rescale_ts(out.packet, {1, 24}, stream->time_base);
        require(av_interleaved_write_frame(out.format, out.packet) >= 0, "Fixture special mux");
    }
    require(av_write_trailer(out.format) >= 0, "Fixture mux trailer");
}
// Issue #398: a WebM (Matroska) file carrying a VP8 stream, a WebM codec this build does not decode.
// The packets are opaque bytes: the probe must refuse the stream by name before any decode. (VP9
// cannot be muxed here: Matroska inserts the vp9_superframe filter, which the build omits.)
void undecodableWebm(const std::filesystem::path& path) {
    Output out;
    require(avformat_alloc_output_context2(&out.format, nullptr, "matroska", path.c_str()) >= 0,
            "Fixture mux allocation");
    auto* stream = avformat_new_stream(out.format, nullptr);
    require(stream, "Fixture video stream");
    stream->time_base = {1, 1000};
    stream->avg_frame_rate = {24, 1};
    auto& vp = *stream->codecpar;
    vp.codec_type = AVMEDIA_TYPE_VIDEO;
    vp.codec_id = AV_CODEC_ID_VP8;
    vp.width = 64;
    vp.height = 48;
    vp.format = AV_PIX_FMT_YUV420P;
    require(avio_open(&out.format->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0, "Fixture file open");
    require(avformat_write_header(out.format, nullptr) >= 0, "Fixture mux header");
    for (int n = 0; n < 2; ++n) {
        require(av_new_packet(out.packet, 16) >= 0, "Fixture packet allocation");
        std::memset(out.packet->data, 0x80 + n, 16);
        out.packet->pts = out.packet->dts = av_rescale_q(n, {1, 24}, stream->time_base);
        out.packet->duration = av_rescale_q(1, {1, 24}, stream->time_base);
        out.packet->flags = AV_PKT_FLAG_KEY;
        out.packet->stream_index = stream->index;
        require(av_interleaved_write_frame(out.format, out.packet) >= 0, "Fixture webm mux");
    }
    require(av_write_trailer(out.format) >= 0, "Fixture mux trailer");
}
// Issue #381: the player-equivalent decode of a clip's first frame, written as packed RGB24. FFmpeg
// decodes the stream and swscale converts limited-range Y'CbCr with `matrix` to full-range R'G'B'
// with no transfer change, exactly what `-vf scale=in_color_matrix=...:in_range=tv:out_range=full`
// does for a player: the R'G'B' goes to the display as it is.
void playerReference(const std::filesystem::path& clip, const std::filesystem::path& output,
                     const int matrix) {
    AVFormatContext* format = nullptr;
    require(avformat_open_input(&format, clip.c_str(), nullptr, nullptr) >= 0, "Reference open");
    const std::unique_ptr<AVFormatContext*, void (*)(AVFormatContext**)> formatOwner(
        &format, avformat_close_input);
    require(avformat_find_stream_info(format, nullptr) >= 0, "Reference stream info");
    const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(index >= 0, "Reference video stream");
    const auto* parameters = format->streams[index]->codecpar;
    const auto* decoder = avcodec_find_decoder(parameters->codec_id);
    require(decoder, "Reference decoder");
    AVCodecContext* codec = avcodec_alloc_context3(decoder);
    const std::unique_ptr<AVCodecContext*, void (*)(AVCodecContext**)> codecOwner(
        &codec, avcodec_free_context);
    require(codec && avcodec_parameters_to_context(codec, parameters) >= 0 &&
                avcodec_open2(codec, decoder, nullptr) >= 0,
            "Reference decoder open");
    AVPacket* packet = av_packet_alloc();
    const std::unique_ptr<AVPacket*, void (*)(AVPacket**)> packetOwner(&packet, av_packet_free);
    AVFrame* frame = av_frame_alloc();
    const std::unique_ptr<AVFrame*, void (*)(AVFrame**)> frameOwner(&frame, av_frame_free);
    require(packet && frame, "Reference allocation");
    bool decoded = false;
    while (!decoded && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index && avcodec_send_packet(codec, packet) >= 0)
            decoded = avcodec_receive_frame(codec, frame) >= 0;
        av_packet_unref(packet);
    }
    if (!decoded && avcodec_send_packet(codec, nullptr) >= 0)
        decoded = avcodec_receive_frame(codec, frame) >= 0;
    require(decoded, "Reference decode");
    auto* scale = sws_getContext(frame->width, frame->height,
                                 static_cast<AVPixelFormat>(frame->format), frame->width,
                                 frame->height, AV_PIX_FMT_RGB24,
                                 SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr,
                                 nullptr, nullptr);
    const std::unique_ptr<SwsContext, void (*)(SwsContext*)> scaleOwner(scale, sws_freeContext);
    require(scale != nullptr, "Reference scaler");
    require(sws_setColorspaceDetails(scale, sws_getCoefficients(matrix), 0,
                                     sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16,
                                     1 << 16) >= 0,
            "Reference color details");
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(frame->width) *
                                  static_cast<std::size_t>(frame->height) * 3U);
    std::array<std::uint8_t*, 4> planes{rgb.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{frame->width * 3, 0, 0, 0};
    require(sws_scale(scale, frame->data, frame->linesize, 0, frame->height, planes.data(),
                      strides.data()) == frame->height,
            "Reference conversion");
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    require(file.good(), "Reference write");
}
} // namespace
bool generateFixtures(const std::string& directory) {
    try {
        const std::filesystem::path path(directory);
        std::filesystem::create_directories(path);
        fixture(path / "numbered-h264.mp4", false);
        fixture(path / "numbered-prores.mov", true);
        fixture(path / "alpha-prores.mov", true, true);
        // Issue #365 interpretation fixtures: untagged, Rec.2100 PQ and HLG (BT.2020 matrix).
        fixture(path / "untagged-h264.mp4", false, false, {1, 1, 1, false}, 12);
        fixture(path / "pq2020-h264.mp4", false, false, {9, 16, 9, true}, 12);
        fixture(path / "hlg2020-h264.mp4", false, false, {9, 18, 9, true}, 12);
        special(path / "vfr-h264.mp4", Special::Vfr);
        special(path / "rotated-h264.mp4", Special::Rotated);
        special(path / "interlaced-prores.mov", Special::Interlaced);
        special(path / "rgb-tiff.mov", Special::Rgb);
        special(path / "anamorphic-h264.mp4", Special::Anamorphic);
        special(path / "untagged-hd-prores.mov", Special::UntaggedHd);
        special(path / "uhd8k-prores.mov", Special::EightK);
        special(path / "chroma-edge-422.mov", Special::ChromaEdge);
        undecodableWebm(path / "vp8-unavailable.webm");
        // Issue #352: a 1,200-frame (50 s) constant-cadence H.264 stream for the lazy index.
        fixture(path / "long-h264.mp4", false, false, {}, 1200);
        // Issue #381: colour patches tagged BT.709 and untagged (64x48 is SD, so the size
        // convention assumes Rec.601), each with its player-equivalent FFmpeg RGB24 decode.
        const auto bt709 = colourPatches(0.2126, 0.0722), bt601 = colourPatches(0.299, 0.114);
        fixture(path / "bars709-h264.mp4", false, false, {1, 1, 1, true}, 2, &bt709);
        fixture(path / "bars-untagged-h264.mp4", false, false, {1, 1, 1, false}, 2, &bt601);
        playerReference(path / "bars709-h264.mp4", path / "bars709-h264.rgb24", SWS_CS_ITU709);
        playerReference(path / "bars-untagged-h264.mp4", path / "bars-untagged-h264.rgb24",
                        SWS_CS_ITU601);
        std::ofstream provenance(path / "provenance.txt");
        provenance
            << "MEDIA-3 generated-read-v1\nFFmpeg 8.1.2 qualified intake mux/ProRes encode\n"
               "H.264 baseline I_PCM macroblocks, GOP 12; ProRes 422; 64x48; 48 frames at 24/1\n"
               "Luma background 16+3*N; seven-segment frame numbers; Rec.709 limited\n"
               "Additional ProRes 4444 alpha fixture uses 512/1023 opacity\n"
               "12-frame H.264 color fixtures: untagged, BT.2020/PQ, BT.2020/HLG\n"
               "12-frame timing fixtures: VFR and 90-degree display-matrix H.264 (all IDR),\n"
               "top-field-first interlaced ProRes 422, RGB48 TIFF in MOV, 2:1 SAR H.264\n"
               "12-frame untagged 1280x720 ProRes 422 (tiled numbered pattern)\n"
               "1200-frame H.264 (pattern repeats every 48 frames) for lazy indexing\n"
               "2-frame 7680x4320 Rec.709 ProRes 422 (tiled numbered pattern)\n"
               "12-frame ProRes 422 HQ, flat luma, Cr 768/384 split at row 24 (siting)\n"
               "2-packet WebM (Matroska) VP8 stream of opaque bytes (undecodable codec)\n"
               "2-frame H.264 color patches (BT.709 tagged, untagged BT.601) with their\n"
               "FFmpeg swscale RGB24 decodes (limited Y'CbCr to full-range R'G'B')\n"
               "PCM mono 48000 Hz: sample[n]=((n%997)-498)/1024\n"
               "Original synthetic content; generated at test time; no external media\n";
        const auto specification =
            provider::ffmpegHandshake().declarations.front().evidence.fixtures.toLowercaseHex();
        provenance << "Generator SHA-256: "
                   << std::string(specification.begin(), specification.end()) << '\n';
        for (const auto* name :
             {"numbered-h264.mp4", "numbered-prores.mov", "alpha-prores.mov", "untagged-h264.mp4",
              "pq2020-h264.mp4", "hlg2020-h264.mp4", "vfr-h264.mp4", "rotated-h264.mp4",
              "interlaced-prores.mov", "rgb-tiff.mov", "anamorphic-h264.mp4",
              "untagged-hd-prores.mov", "long-h264.mp4", "uhd8k-prores.mov",
              "chroma-edge-422.mov", "vp8-unavailable.webm", "bars709-h264.mp4",
              "bars-untagged-h264.mp4", "bars709-h264.rgb24", "bars-untagged-h264.rgb24"}) {
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
