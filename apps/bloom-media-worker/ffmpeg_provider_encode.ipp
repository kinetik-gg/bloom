// Included only by ffmpeg_provider.cpp: all codec and container types stay in that TU.
namespace bloom::media::ffmpeg {
namespace {
struct PrivateMediaFile {
    std::FILE* file = nullptr;
    std::uint64_t size = 0, limit = 0;
    // `inherited` >= 0 is the host's staged artifact (ADR 0033 section 16): the container is
    // written straight into it, and the reopen check reads it back, so no spool copy exists.
    explicit PrivateMediaFile(std::uint64_t cap, const int inherited = -1) : limit(cap) {
#if defined(__unix__) || defined(__APPLE__)
        if (inherited >= 0) {
            require(::ftruncate(inherited, 0) == 0, "Cannot reset the staged artifact", Error::Io);
            const int own = ::dup(inherited);
            file = own >= 0 ? ::fdopen(own, "w+b") : nullptr;
            if (file == nullptr && own >= 0)
                ::close(own);
        } else
#endif
            file = std::tmpfile();
        require(file != nullptr, "Cannot create private media spool", Error::Io);
    }
    ~PrivateMediaFile() { std::fclose(file); }
    PrivateMediaFile(const PrivateMediaFile&) = delete;
    PrivateMediaFile& operator=(const PrivateMediaFile&) = delete;
    static int write(void* opaque, const std::uint8_t* data, int count) {
        auto& self = *static_cast<PrivateMediaFile*>(opaque);
        const auto position = std::ftell(self.file);
        if (position < 0 || count < 0 || static_cast<std::uint64_t>(position) > self.limit ||
            static_cast<std::uint64_t>(count) > self.limit - static_cast<std::uint64_t>(position))
            return AVERROR(ENOSPC);
        if (std::fwrite(data, 1, static_cast<std::size_t>(count), self.file) !=
            static_cast<std::size_t>(count))
            return AVERROR(EIO);
        self.size = std::max(self.size,
                             static_cast<std::uint64_t>(position) + static_cast<unsigned>(count));
        return count;
    }
    static int read(void* opaque, std::uint8_t* data, int count) {
        auto& self = *static_cast<PrivateMediaFile*>(opaque);
        const auto n = std::fread(data, 1, static_cast<std::size_t>(count), self.file);
        return n != 0 ? static_cast<int>(n) : (std::ferror(self.file) ? AVERROR(EIO) : AVERROR_EOF);
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        auto& self = *static_cast<PrivateMediaFile*>(opaque);
        if (whence == AVSEEK_SIZE)
            return static_cast<std::int64_t>(self.size);
        whence &= ~AVSEEK_FORCE;
        if (offset > std::numeric_limits<long>::max() ||
            offset < std::numeric_limits<long>::min() ||
            (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) ||
            std::fseek(self.file, static_cast<long>(offset), whence) != 0)
            return AVERROR(EIO);
        const auto position = std::ftell(self.file);
        return position < 0 || static_cast<std::uint64_t>(position) > self.limit ? AVERROR(EIO)
                                                                                 : position;
    }
    Digest hash() {
        require(std::fflush(file) == 0 && std::fseek(file, 0, SEEK_SET) == 0, "Spool close failed",
                Error::Io);
        core::Sha256Hasher hasher;
        std::array<std::byte, 65536> buffer{};
        std::uint64_t offset = 0;
        while (offset < size) {
            const auto count = std::min<std::uint64_t>(buffer.size(), size - offset);
            require(std::fread(buffer.data(), 1, count, file) == count &&
                        hasher.update(std::span(buffer).first(count)),
                    "Spool hash failed", Error::Io);
            offset += count;
        }
        return hasher.finalize();
    }
};
struct PrivateAvio {
    AVIOContext* context = nullptr;
    PrivateAvio(PrivateMediaFile& file, bool writing) {
        auto* buffer = static_cast<unsigned char*>(av_malloc(65536));
        require(buffer != nullptr, "Media I/O allocation", Error::Oversized);
        context = avio_alloc_context(
            buffer, 65536, writing ? 1 : 0, &file, writing ? nullptr : PrivateMediaFile::read,
            writing ? PrivateMediaFile::write : nullptr, PrivateMediaFile::seek);
        if (!context) {
            av_free(buffer);
            throw Unavailable{Error::Oversized, "Media I/O allocation"};
        }
    }
    ~PrivateAvio() {
        av_free(context->buffer);
        context->buffer = nullptr;
        avio_context_free(&context);
    }
    PrivateAvio(const PrivateAvio&) = delete;
    PrivateAvio& operator=(const PrivateAvio&) = delete;
};
struct EncodeTrack {
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* hardwareFrame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    AVBufferRef* hardwareFrames = nullptr;
    SwsContext* scale = nullptr;
    SwrContext* resample = nullptr;
    ~EncodeTrack() {
        sws_freeContext(scale);
        swr_free(&resample);
        av_frame_free(&frame);
        av_frame_free(&hardwareFrame);
        av_buffer_unref(&hardwareFrames);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
    }
};
int videoProfile(const EncodeSettingsV1& s) {
    if (s.videoCodec == "prores_ks") {
        constexpr std::array<std::string_view, 6> names{"proxy", "lt",   "422",
                                                        "hq",    "4444", "4444xq"};
        const auto* found = std::ranges::find(names, s.profile);
        require(found != names.end(), "ProRes profile unavailable", Error::Unavailable);
        return static_cast<int>(found - names.begin());
    }
    if (s.videoCodec == "dnxhd") {
        constexpr std::array<std::string_view, 6> names{"dnxhd",    "dnxhr_lb",  "dnxhr_sq",
                                                        "dnxhr_hq", "dnxhr_hqx", "dnxhr_444"};
        const auto* found = std::ranges::find(names, s.profile);
        require(found != names.end(), "DNx profile unavailable", Error::Unavailable);
        return static_cast<int>(found - names.begin());
    }
    if (s.videoCodec == "h264") {
        require(s.profile == "high", "H.264 profile unavailable", Error::Unavailable);
        return AV_PROFILE_H264_HIGH;
    }
    if (s.videoCodec == "hevc") {
        require(s.profile == "main" || s.profile == "main10", "HEVC profile unavailable",
                Error::Unavailable);
        return s.profile == "main10" ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
    }
    require(s.videoCodec == "tiff" && s.profile == "rgba16", "Video encoder unavailable",
            Error::Unavailable);
    return 0;
}
bool hardwareCodec(const EncodeSettingsV1& s) {
    return s.videoCodec == "h264" || s.videoCodec == "hevc";
}
// The software sample layout uploaded to a VA-API encoder.
AVPixelFormat hardwareSamples(const EncodeSettingsV1& s) {
    return s.profile == "main10" ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
}
// "<container>/<codec>" of a reopened artifact. MOV and MP4 share one demuxer; the major brand
// tells them apart ("qt  " for QuickTime).
std::string containerToken(const AVFormatContext& format, const AVCodecParameters& codec) {
    std::string container = format.iformat ? format.iformat->name : "";
    if (container.starts_with("mov")) {
        const auto* brand = av_dict_get(format.metadata, "major_brand", nullptr, 0);
        container = brand && std::string_view(brand->value) == "qt  " ? "mov" : "mp4";
    } else if (container.starts_with("mxf")) {
        container = "mxf";
    }
    return container + "/" + avcodec_get_name(codec.codec_id);
}
} // namespace
struct Encoder::State {
    EncodeSettingsV1 settings;
    PrivateMediaFile file;
    PrivateAvio io;
    AVFormatContext* format = nullptr;
    EncodeTrack videoTrack, audioTrack;
    std::uint64_t frameCount = 0, sampleCount = 0;
    bool finished = false;
    Bytes firstReference, lastReference;
    core::Sha256Hasher expectedPcm;
    std::unique_ptr<PrivateMediaFile> aacReference;
    std::vector<std::vector<float>> pendingAudio;
    std::uint64_t audioSubmitted = 0;
    bool hardware = false;
    State(EncodeSettingsV1 s, const bool useHardware, const int stagedOutput)
        : settings(std::move(s)), file(settings.byteLimit, stagedOutput), io(file, true),
          hardware(useHardware) {}
    ~State() { avformat_free_context(format); }
    void packets(EncodeTrack& track, AVFrame* frame) {
        AVFrame* submitted = frame;
        if (frame != nullptr && track.hardwareFrames != nullptr) {
            require(av_hwframe_get_buffer(track.hardwareFrames, track.hardwareFrame, 0) >= 0 &&
                        av_hwframe_transfer_data(track.hardwareFrame, frame, 0) >= 0,
                    "VA-API frame upload failed", Error::Unavailable);
            track.hardwareFrame->pts = frame->pts;
            submitted = track.hardwareFrame;
        }
        require(avcodec_send_frame(track.codec, submitted) >= 0, "Encoder rejected input");
        int status = 0;
        while ((status = avcodec_receive_packet(track.codec, track.packet)) >= 0) {
            if (settings.videoCodec == "tiff") {
                require(PrivateMediaFile::write(&file, track.packet->data, track.packet->size) ==
                            track.packet->size,
                        "TIFF product exceeds limit", Error::Oversized);
            } else {
                av_packet_rescale_ts(track.packet, track.codec->time_base, track.stream->time_base);
                track.packet->stream_index = track.stream->index;
                require(av_interleaved_write_frame(format, track.packet) >= 0,
                        "Mux failed or artifact exceeds limit", Error::Io);
            }
            av_packet_unref(track.packet);
        }
        require(status == AVERROR(EAGAIN) || status == AVERROR_EOF, "Encoder packet failure");
    }
    void openTrack(EncodeTrack& track, const std::string& name, bool video) {
        const auto* codec = avcodec_find_encoder_by_name(name.c_str());
        require(codec != nullptr, "Encoder is not in the intake", Error::Unavailable);
        track.codec = avcodec_alloc_context3(codec);
        require(track.codec && track.frame && track.packet, "Encoder allocation", Error::Oversized);
        auto& c = *track.codec;
        c.thread_count =
            video && settings.videoCodec != "tiff" ? static_cast<int>(settings.threads) : 1;
        c.flags |= AV_CODEC_FLAG_BITEXACT;
        if (format && (format->oformat->flags & AVFMT_GLOBALHEADER))
            c.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (video) {
            c.width = static_cast<int>(settings.width);
            c.height = static_cast<int>(settings.height);
            c.time_base = {static_cast<int>(settings.rate.denominator),
                           static_cast<int>(settings.rate.numerator)};
            c.framerate = av_inv_q(c.time_base);
            c.profile = videoProfile(settings);
            c.pix_fmt = settings.videoCodec == "tiff"         ? AV_PIX_FMT_RGBA64LE
                        : hardwareCodec(settings) && hardware ? AV_PIX_FMT_VAAPI
                        : settings.videoCodec == "h264"       ? AV_PIX_FMT_YUV420P
                                                              : AV_PIX_FMT_YUV422P10LE;
            if (settings.videoCodec == "prores_ks" && c.profile >= 4)
                c.pix_fmt = AV_PIX_FMT_YUVA444P10LE;
            if (settings.videoCodec == "dnxhd") {
                c.pix_fmt = c.profile == 5   ? AV_PIX_FMT_YUV444P10LE
                            : c.profile == 4 ? AV_PIX_FMT_YUV422P10LE
                                             : AV_PIX_FMT_YUV422P;
                if (c.profile == 0)
                    c.bit_rate = 120000000;
            }
            if (hardwareCodec(settings)) {
                c.bit_rate = settings.videoBitrateKbps != 0
                                 ? std::int64_t{settings.videoBitrateKbps} * 1000
                                 : (settings.videoCodec == "hevc" ? 20000000 : 30000000);
                c.rc_min_rate = c.bit_rate;
                c.rc_max_rate = c.bit_rate;
                c.gop_size = static_cast<int>(
                    std::max<std::int64_t>(1, settings.rate.numerator / settings.rate.denominator));
                c.max_b_frames = 0;
                if (hardware) {
                    const auto device = settings.vaapiDevice.empty()
                                            ? std::string("/dev/dri/renderD128")
                                            : settings.vaapiDevice;
                    require(av_hwdevice_ctx_create(&c.hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                                   device.c_str(), nullptr, 0) >= 0,
                            "VA-API device unavailable", Error::Unavailable);
                    track.hardwareFrames = av_hwframe_ctx_alloc(c.hw_device_ctx);
                    require(track.hardwareFrames, "VA-API frame context unavailable",
                            Error::Unavailable);
                    auto* frames = reinterpret_cast<AVHWFramesContext*>(track.hardwareFrames->data);
                    frames->format = AV_PIX_FMT_VAAPI;
                    frames->sw_format = hardwareSamples(settings);
                    frames->width = c.width;
                    frames->height = c.height;
                    frames->initial_pool_size = 4;
                    require(av_hwframe_ctx_init(track.hardwareFrames) >= 0,
                            "VA-API frame context failed", Error::Unavailable);
                    c.hw_frames_ctx = av_buffer_ref(track.hardwareFrames);
                }
            }
            // #369: tags from the output colour intent instead of a fixed BT.709.
            const auto tag = [](const std::int32_t code, const int unspecified) {
                return code < 0 ? unspecified : code;
            };
            c.color_primaries = static_cast<AVColorPrimaries>(
                tag(settings.colour.primaries, AVCOL_PRI_UNSPECIFIED));
            c.color_trc = static_cast<AVColorTransferCharacteristic>(
                tag(settings.colour.transfer, AVCOL_TRC_UNSPECIFIED));
            c.colorspace =
                static_cast<AVColorSpace>(tag(settings.colour.matrix, AVCOL_SPC_UNSPECIFIED));
            c.color_range = static_cast<AVColorRange>(tag(settings.colour.range, AVCOL_RANGE_MPEG));
            // Subsampled chroma is written co-sited left (the H.273 default every intaken codec
            // and Bloom's own reader assume) and declared, so a reader never has to guess.
            const auto* samples = av_pix_fmt_desc_get(
                c.pix_fmt == AV_PIX_FMT_VAAPI ? hardwareSamples(settings) : c.pix_fmt);
            require(samples != nullptr, "Output sample format unavailable", Error::Unavailable);
            c.chroma_sample_location = samples->log2_chroma_w != 0 || samples->log2_chroma_h != 0
                                           ? AVCHROMA_LOC_LEFT
                                           : AVCHROMA_LOC_UNSPECIFIED;
            c.sample_aspect_ratio = {1, 1};
        } else {
            require(name == "pcm_s16le" || name == "pcm_s24le" || name == "aac",
                    "Audio encoder unavailable", Error::Unavailable);
            c.sample_rate = static_cast<int>(settings.sampleRate);
            c.time_base = {1, c.sample_rate};
            av_channel_layout_default(&c.ch_layout, static_cast<int>(settings.channels));
            c.sample_fmt = name == "aac"         ? AV_SAMPLE_FMT_FLTP
                           : name == "pcm_s16le" ? AV_SAMPLE_FMT_S16
                                                 : AV_SAMPLE_FMT_S32;
            c.bit_rate = 192000;
        }
        AVDictionary* options = nullptr;
        if (settings.videoCodec == "dnxhd" && video)
            av_dict_set(&options, "profile", settings.profile.c_str(), 0);
        if (settings.videoCodec == "tiff" && video)
            av_dict_set(&options, "compression_algo", "raw", 0);
        const auto status = avcodec_open2(&c, codec, &options);
        av_dict_free(&options);
        require(status >= 0, "Encoder refused profile or stream layout", Error::Unavailable);
        if (format) {
            track.stream = avformat_new_stream(format, nullptr);
            require(track.stream, "Mux stream allocation", Error::Oversized);
            track.stream->time_base = c.time_base;
            if (video)
                track.stream->avg_frame_rate = c.framerate;
            require(avcodec_parameters_from_context(track.stream->codecpar, &c) >= 0,
                    "Encoder metadata failure");
        }
        if (video) {
            track.frame->format =
                track.hardwareFrames
                    ? reinterpret_cast<AVHWFramesContext*>(track.hardwareFrames->data)->sw_format
                    : c.pix_fmt;
            track.frame->width = c.width;
            track.frame->height = c.height;
            require(av_frame_get_buffer(track.frame, 32) >= 0, "Encode frame allocation",
                    Error::Oversized);
            // The conversion is exactly what the stream declares: its matrix, range and siting.
            track.scale = declaredConversion(
                c.width, c.height, AV_PIX_FMT_RGBA64LE,
                static_cast<AVPixelFormat>(track.frame->format),
                {c.colorspace, c.color_range, c.chroma_sample_location}, kOutputConversionFlags);
            require(track.scale, "Output pixel conversion unavailable", Error::Unavailable);
        } else {
            require(swr_alloc_set_opts2(&track.resample, &c.ch_layout, c.sample_fmt, c.sample_rate,
                                        &c.ch_layout, AV_SAMPLE_FMT_FLTP, c.sample_rate, 0,
                                        nullptr) >= 0 &&
                        swr_init(track.resample) >= 0,
                    "PCM conversion unavailable");
        }
    }
    void open() {
        require(valid(settings), "Invalid encode settings", Error::InvalidValue);
        // HEVC is hardware-only (ADR 0033): there is no software HEVC encoder in the intake.
        if ((settings.videoCodec == "hevc" && !hardware) || settings.videoCodec == "h265")
            throw Unavailable{Error::Unavailable, "codec.hevc.software-encoder-not-intaken"};
        if (settings.videoCodec == "tiff") {
            require(settings.container == "tiff" && settings.frames == 1 &&
                        settings.audioCodec.empty(),
                    "TIFF is a single image product", Error::Unavailable);
        } else {
            require(settings.container == "mov" || settings.container == "mp4" ||
                        settings.container == "mxf" || settings.container == "matroska" ||
                        settings.container == "wav",
                    "Muxer unavailable", Error::Unavailable);
            require(settings.container != "wav" || settings.videoCodec.empty(),
                    "WAV cannot carry video", Error::Unavailable);
            require(avformat_alloc_output_context2(&format, nullptr, settings.container.c_str(),
                                                   nullptr) >= 0 &&
                        format,
                    "Mux allocation failed");
            format->pb = io.context;
            format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_BITEXACT;
            format->max_interleave_delta = 1000000;
            if (settings.videoCodec == "prores_ks")
                av_dict_set(&format->metadata, "comment",
                            "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes "
                            "implementation; apple_authorized=false; delivery_qualified=false",
                            0);
            if (!settings.bwfDescription.empty())
                av_dict_set(&format->metadata, "description", settings.bwfDescription.c_str(), 0);
        }
        if (!settings.videoCodec.empty())
            openTrack(videoTrack,
                      settings.videoCodec == "h264"   ? (hardware ? "h264_vaapi" : "libopenh264")
                      : settings.videoCodec == "hevc" ? "hevc_vaapi"
                                                      : settings.videoCodec,
                      true);
        if (!settings.audioCodec.empty())
            openTrack(audioTrack, settings.audioCodec, false);
        if (settings.audioCodec == "aac") {
            aacReference = std::make_unique<PrivateMediaFile>(settings.byteLimit);
            pendingAudio.resize(settings.channels);
        }
        if (format) {
            AVDictionary* options = nullptr;
            if (settings.container == "wav") {
                av_dict_set(&options, "rf64", "auto", 0);
                if (!settings.bwfDescription.empty())
                    av_dict_set(&options, "write_bext", "1", 0);
            }
            const auto status = avformat_write_header(format, &options);
            av_dict_free(&options);
            require(status >= 0, "Muxer refused stream layout", Error::Unavailable);
        }
    }
    void push(const FrameProduct& product) {
        require(!finished && videoTrack.codec && valid(product) &&
                    product.format == PixelFormat::Rgba16 &&
                    product.planes[0].width == settings.width &&
                    product.planes[0].height == settings.height &&
                    product.planes[0].stride == settings.width * 8U &&
                    frameCount < settings.frames &&
                    product.pts ==
                        rational(static_cast<std::int64_t>(frameCount) * settings.rate.denominator,
                                 settings.rate.numerator),
                "Frame does not match frozen encode request", Error::IdentityMismatch);
        auto& track = videoTrack;
        require(av_frame_make_writable(track.frame) >= 0, "Encode frame allocation",
                Error::Oversized);
        const std::uint8_t* data[4]{
            reinterpret_cast<const std::uint8_t*>(product.planes[0].bytes.data()), nullptr, nullptr,
            nullptr};
        int strides[4]{static_cast<int>(product.planes[0].stride), 0, 0, 0};
        require(sws_scale(track.scale, data, strides, 0, track.codec->height, track.frame->data,
                          track.frame->linesize) == track.codec->height,
                "Output conversion failed");
        if (frameCount == 0)
            firstReference = product.planes[0].bytes;
        if (frameCount + 1 == settings.frames)
            lastReference = product.planes[0].bytes;
        track.frame->pts = static_cast<std::int64_t>(frameCount);
        packets(track, track.frame);
        ++frameCount;
    }
    void push(const AudioBlock& product) {
        require(!finished && audioTrack.codec && valid(product) &&
                    product.sampleRate == settings.sampleRate &&
                    product.channels.size() == settings.channels &&
                    product.channels[0].size() <= settings.audioSamples - sampleCount &&
                    product.pts ==
                        rational(static_cast<std::int64_t>(sampleCount), settings.sampleRate) &&
                    product.channelLayout == channels(audioTrack.codec->ch_layout),
                "Audio does not match frozen encode request", Error::IdentityMismatch);
        if (settings.audioCodec == "aac") {
            for (std::size_t i = 0; i < product.channels[0].size(); ++i) {
                for (std::size_t c = 0; c < settings.channels; ++c) {
                    const auto sample = product.channels[c][i];
                    require(PrivateMediaFile::write(aacReference.get(),
                                                    reinterpret_cast<const std::uint8_t*>(&sample),
                                                    sizeof(sample)) == sizeof(sample),
                            "AAC reference exceeds limit", Error::Oversized);
                    pendingAudio[c].push_back(sample);
                }
                if (pendingAudio[0].size() == 1024) {
                    encodeAudio(pendingAudio);
                    for (auto& channel : pendingAudio)
                        channel.clear();
                }
            }
        } else
            encodeAudio(product.channels);
        sampleCount += product.channels[0].size();
    }
    void encodeAudio(const std::vector<std::vector<float>>& planes) {
        auto& track = audioTrack;
        av_frame_unref(track.frame);
        auto& f = *track.frame;
        f.format = track.codec->sample_fmt;
        f.sample_rate = track.codec->sample_rate;
        require(av_channel_layout_copy(&f.ch_layout, &track.codec->ch_layout) >= 0,
                "Audio layout allocation");
        f.nb_samples = static_cast<int>(planes[0].size());
        f.pts = static_cast<std::int64_t>(audioSubmitted);
        require(av_frame_get_buffer(&f, 0) >= 0, "Audio allocation", Error::Oversized);
        std::vector<const std::uint8_t*> source;
        source.reserve(planes.size());
        for (const auto& plane : planes)
            source.push_back(reinterpret_cast<const std::uint8_t*>(plane.data()));
        require(swr_convert(track.resample, f.data, f.nb_samples, source.data(), f.nb_samples) ==
                    f.nb_samples,
                "Audio conversion changed duration");
        if (settings.audioCodec.starts_with("pcm_")) {
            // The s24 encoder discards the low eight bits of its signed 32-bit input.
            if (settings.audioCodec == "pcm_s24le") {
                auto* samples = reinterpret_cast<std::int32_t*>(f.data[0]);
                for (std::uint64_t i = 0; i < planes[0].size() * settings.channels; ++i)
                    samples[i] &= ~255;
            }
            require(expectedPcm.update(std::as_bytes(
                        std::span(f.data[0], planes[0].size() * settings.channels *
                                                 (settings.audioCodec == "pcm_s16le" ? 2U : 4U)))),
                    "PCM identity limit");
        }
        packets(track, &f);
        audioSubmitted += static_cast<unsigned>(f.nb_samples);
    }
    // Tolerance profile v2: the reference goes through the sample format and the sampling the
    // reopened stream declares, so 4:2:0/4:2:2 chroma decimation is not counted as codec error
    // while a wrong sample layout, matrix, range or siting still is.
    void compare(const AVFrame& decoded, const Bytes& reference, EncodeQcV1& qc,
                 const DeclaredSampling& declared) {
        require(decoded.width == static_cast<int>(settings.width) &&
                    decoded.height == static_cast<int>(settings.height),
                "Reopened dimensions differ");
        if (settings.videoCodec != "tiff") {
            // The codec's own subsampling, not whatever the stream claims: the decoded layout must
            // decimate chroma exactly as the encoder's input did.
            const auto* actual = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded.format));
            const auto* written =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(videoTrack.frame->format));
            require(actual && written && (actual->flags & AV_PIX_FMT_FLAG_RGB) == 0 &&
                        actual->log2_chroma_w == written->log2_chroma_w &&
                        actual->log2_chroma_h == written->log2_chroma_h,
                    "Reopened chroma sampling differs");
        }
        auto measure = measureReopen(decoded, std::span<const std::byte>(reference), declared);
        require(measure.failure.empty(), measure.failure.c_str());
        const bool alpha = settings.videoCodec == "tiff" ||
                           (settings.videoCodec == "prores_ks" && videoTrack.codec->profile >= 4);
        if (alpha)
            require(measure.alphaMaximum <= (settings.videoCodec == "tiff" ? 0U : 64U),
                    "Reopened alpha exceeds tolerance");
        qc.maximumError = std::max(qc.maximumError, measure.maximum);
        qc.meanError =
            std::max(qc.meanError,
                     static_cast<std::uint32_t>((measure.sum + measure.count - 1) / measure.count));
        const auto& rgba = measure.rgba;
        require(settings.videoCodec == "tiff" ? rgba == reference
                                              : qc.maximumError <= 22938 && qc.meanError <= 1967,
                ("Reopened pixels exceed tolerance profile (max=" +
                 std::to_string(qc.maximumError) + " mean=" + std::to_string(qc.meanError) + ")")
                    .c_str());
        const auto digest = digestBytes(rgba);
        if (qc.frames == 0)
            qc.firstFrame = digest;
        qc.lastFrame = digest;
    }
    EncodeQcV1 finish() {
        require(!finished && frameCount == settings.frames && sampleCount == settings.audioSamples,
                "Incomplete encode input", Error::InvalidValue);
        if (videoTrack.codec)
            packets(videoTrack, nullptr);
        if (!pendingAudio.empty() && !pendingAudio[0].empty())
            encodeAudio(pendingAudio);
        if (audioTrack.codec)
            packets(audioTrack, nullptr);
        if (format)
            require(av_write_trailer(format) >= 0, "Mux close failed", Error::Io);
        avio_flush(io.context);
        require(io.context->error >= 0, "Spool write failed", Error::Io);
        EncodeQcV1 qc;
        qc.bytes = file.size;
        qc.artifact = file.hash();
        require(qc.bytes > 0, "Empty encoded artifact");
        if (settings.videoCodec == "tiff") {
            Input input;
            const auto* codec = avcodec_find_decoder(AV_CODEC_ID_TIFF);
            require(codec, "TIFF reopen decoder unavailable", Error::Unavailable);
            input.codec = avcodec_alloc_context3(codec);
            require(input.codec && avcodec_open2(input.codec, codec, nullptr) >= 0,
                    "TIFF reopen failed");
            require(file.size <= Limits::productBytes &&
                        av_new_packet(input.packet, static_cast<int>(file.size)) >= 0 &&
                        std::fseek(file.file, 0, SEEK_SET) == 0,
                    "TIFF reopen allocation");
            require(std::fread(input.packet->data, 1, file.size, file.file) == file.size &&
                        avcodec_send_packet(input.codec, input.packet) >= 0 &&
                        avcodec_receive_frame(input.codec, input.frame) >= 0,
                    "TIFF reopen failed");
            compare(*input.frame, firstReference, qc, {});
            qc.frames = 1;
        } else {
            verifyVideo(qc);
            verifyAudio(qc);
        }
        qc.duration =
            settings.frames != 0
                ? rational(static_cast<std::int64_t>(settings.frames) * settings.rate.denominator,
                           settings.rate.numerator)
                : rational(static_cast<std::int64_t>(settings.audioSamples), settings.sampleRate);
        finished = true;
        return qc;
    }
    void openInput(Input& input, PrivateAvio& reader) {
        require(std::fseek(file.file, 0, SEEK_SET) == 0, "Cannot reopen spool", Error::Io);
        input.format = avformat_alloc_context();
        require(input.format, "Reopen allocation");
        input.format->pb = reader.context;
        input.format->flags |= AVFMT_FLAG_CUSTOM_IO;
        require(avformat_open_input(&input.format, nullptr, nullptr, nullptr) >= 0 &&
                    avformat_find_stream_info(input.format, nullptr) >= 0,
                "Reopen demux failed");
        require(input.format->nb_streams == (settings.videoCodec.empty() ? 0U : 1U) +
                                                (settings.audioCodec.empty() ? 0U : 1U),
                "Reopened stream layout differs");
    }
    void verifyVideo(EncodeQcV1& qc) {
        if (settings.videoCodec.empty())
            return;
        PrivateAvio reader(file, false);
        Input input;
        openInput(input, reader);
        const auto& stream = *input.format->streams[0];
        require(stream.codecpar->codec_id == videoTrack.codec->codec_id,
                "Reopened video codec differs");
        // #369: what the container actually declares, for the ColourTags and Container checks.
        const auto& parameters = *stream.codecpar;
        qc.colour = {parameters.color_primaries, parameters.color_trc, parameters.color_space,
                     parameters.color_range};
        qc.container = containerToken(*input.format, parameters);
        const DeclaredSampling declared{parameters.color_space, parameters.color_range,
                                        parameters.chroma_location};
        decoder(input, stream, nullptr);
        bool eof = false;
        while (next(input, 0, eof)) {
            require(
                qc.frames < settings.frames &&
                    timestamp(input.frame->best_effort_timestamp, stream.time_base) ==
                        rational(static_cast<std::int64_t>(qc.frames) * settings.rate.denominator,
                                 settings.rate.numerator),
                "Reopened video timing differs");
            if (qc.frames == 0 || qc.frames + 1 == settings.frames)
                compare(*input.frame, qc.frames == 0 ? firstReference : lastReference, qc,
                        declared);
            ++qc.frames;
            av_frame_unref(input.frame);
        }
        require(qc.frames == settings.frames, "Reopened frame count differs");
        require(
            stream.duration == AV_NOPTS_VALUE ||
                timestamp(stream.duration, stream.time_base) ==
                    rational(static_cast<std::int64_t>(settings.frames) * settings.rate.denominator,
                             settings.rate.numerator),
            "Reopened duration differs");
    }
    void verifyAudio(EncodeQcV1& qc) {
        if (settings.audioCodec.empty())
            return;
        PrivateAvio reader(file, false);
        Input input;
        openInput(input, reader);
        const unsigned index = settings.videoCodec.empty() ? 0U : 1U;
        const auto& stream = *input.format->streams[index];
        if (settings.videoCodec.empty())
            qc.container = containerToken(*input.format, *stream.codecpar);
        require(
            stream.codecpar->codec_id == audioTrack.codec->codec_id &&
                stream.codecpar->sample_rate == static_cast<int>(settings.sampleRate) &&
                (channels(stream.codecpar->ch_layout) == channels(audioTrack.codec->ch_layout) ||
                 ((settings.container == "mxf" ||
                   (settings.container == "wav" && settings.channels <= 2)) &&
                  stream.codecpar->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC &&
                  stream.codecpar->ch_layout.nb_channels ==
                      audioTrack.codec->ch_layout.nb_channels)),
            (std::string("Reopened audio layout differs: codec=") +
             avcodec_get_name(stream.codecpar->codec_id) +
             ";rate=" + std::to_string(stream.codecpar->sample_rate) +
             ";channels=" + std::to_string(stream.codecpar->ch_layout.nb_channels) +
             ";order=" + std::to_string(stream.codecpar->ch_layout.order))
                .c_str());
        decoder(input, stream, nullptr);
        bool eof = false;
        core::Sha256Hasher actual;
        if (aacReference)
            require(std::fflush(aacReference->file) == 0 &&
                        std::fseek(aacReference->file, 0, SEEK_SET) == 0,
                    "AAC reference reopen failed");
        double squaredError = 0;
        std::uint64_t comparedSamples = 0;
        while (next(input, index, eof)) {
            const auto& f = *input.frame;
            require(f.nb_samples > 0 && f.nb_samples <= static_cast<int>(Limits::audioSamples) &&
                        f.sample_rate == static_cast<int>(settings.sampleRate),
                    "Reopened audio format differs");
            const auto begin = av_rescale_q(f.best_effort_timestamp, stream.time_base,
                                            {1, static_cast<int>(settings.sampleRate)});
            require(begin == static_cast<std::int64_t>(qc.audioSamples),
                    "Reopened audio timing differs");
            const auto count =
                settings.audioCodec == "aac"
                    ? std::min<std::uint64_t>(static_cast<unsigned>(f.nb_samples),
                                              settings.audioSamples -
                                                  std::min(qc.audioSamples, settings.audioSamples))
                    : static_cast<unsigned>(f.nb_samples);
            if (settings.audioCodec == "aac") {
                require(f.format == AV_SAMPLE_FMT_FLTP, "AAC reopen sample format differs");
                for (std::size_t i = 0; i < count; ++i)
                    for (std::size_t c = 0; c < settings.channels; ++c) {
                        float reference = 0;
                        require(std::fread(&reference, sizeof(reference), 1, aacReference->file) ==
                                    1,
                                "AAC reference truncated");
                        const auto value = reinterpret_cast<const float*>(f.extended_data[c])[i];
                        const auto error =
                            static_cast<double>(value) - static_cast<double>(reference);
                        require(std::isfinite(value) && std::abs(error) <= 0.25,
                                (std::string("AAC sample exceeds tolerance at ") +
                                 std::to_string(qc.audioSamples + i) + ": actual=" +
                                 std::to_string(value) + "; expected=" + std::to_string(reference))
                                    .c_str());
                        require(actual.update(std::as_bytes(std::span(&value, 1))),
                                "AAC digest failed");
                        squaredError += error * error;
                        ++comparedSamples;
                    }
            }
            if (settings.audioCodec.starts_with("pcm_")) {
                const auto bytes = static_cast<std::size_t>(f.nb_samples) * settings.channels *
                                   (settings.audioCodec == "pcm_s16le" ? 2U : 4U);
                require(f.format == audioTrack.codec->sample_fmt &&
                            actual.update(std::as_bytes(std::span(f.data[0], bytes))),
                        "Reopened PCM representation differs");
            }
            qc.audioSamples += count;
            av_frame_unref(input.frame);
        }
        require(qc.audioSamples == settings.audioSamples, "Reopened audio sample count differs");
        if (settings.audioCodec == "aac") {
            require(comparedSamples > 0 &&
                        squaredError / static_cast<double>(comparedSamples) <= 0.0025,
                    "AAC RMS exceeds tolerance");
            require(stream.duration != AV_NOPTS_VALUE &&
                        timestamp(stream.duration, stream.time_base) ==
                            rational(static_cast<std::int64_t>(settings.audioSamples),
                                     settings.sampleRate),
                    "AAC logical duration differs");
        }
        qc.audio = actual.finalize();
        if (settings.audioCodec.starts_with("pcm_"))
            require(qc.audio == expectedPcm.finalize(), "Reopened PCM samples differ");
    }
};
Encoder::Encoder(const bool hardware, const int stagedOutput)
    : hardware_(hardware), stagedOutput_(stagedOutput) {}
Encoder::~Encoder() = default;
provider::Payload Encoder::call(provider::MessageKind kind, const provider::Payload& payload) {
    try {
        av_log_set_level(AV_LOG_ERROR);
        if (kind == MessageKind::EncodeBegin) {
            require(!state_, "Encoder already initialized", Error::UnexpectedMessage);
            state_ = std::make_unique<State>(std::get<EncodeSettingsV1>(payload), hardware_,
                                             std::exchange(stagedOutput_, -1));
            state_->open();
        } else {
            require(state_ != nullptr, "Encoder is not initialized", Error::UnexpectedMessage);
            if (kind == MessageKind::Frame)
                state_->push(std::get<FrameProduct>(payload));
            else if (kind == MessageKind::Audio)
                state_->push(std::get<AudioBlock>(payload));
            else if (kind == MessageKind::EncodeFinish)
                return state_->finish();
            else if (kind == MessageKind::EncodeRead) {
                const auto offset = std::get<std::uint64_t>(payload);
                require(state_->finished && offset < state_->file.size,
                        "Invalid product chunk request", Error::InvalidValue);
                EncodedChunkV1 chunk;
                chunk.offset = offset;
                chunk.bytes.resize(
                    std::min<std::uint64_t>(kEncodeChunkBytes, state_->file.size - offset));
                require(PrivateMediaFile::seek(&state_->file, static_cast<std::int64_t>(offset),
                                               SEEK_SET) >= 0 &&
                            std::fread(chunk.bytes.data(), 1, chunk.bytes.size(),
                                       state_->file.file) == chunk.bytes.size(),
                        "Product chunk read failed", Error::Io);
                return chunk;
            } else
                throw Unavailable{Error::UnexpectedMessage, "Invalid encode operation"};
        }
        return std::monostate{};
    } catch (const Unavailable& error) {
        state_.reset();
        return error;
    } catch (const std::bad_alloc&) {
        state_.reset();
        return Unavailable{Error::Oversized, "Encode allocation limit"};
    }
}
} // namespace bloom::media::ffmpeg
