// Retained and one-shot decode entry points. Included by the sole FFmpeg-bearing translation unit.
namespace bloom::media::ffmpeg {
namespace {
struct Device final {
    AVBufferRef* context = nullptr;
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() { av_buffer_unref(&context); }
    // Opens the first discovered render node at or after `start`; returns the node index used.
    std::optional<std::size_t> open(const std::vector<std::string>& nodes, std::size_t start) {
        av_buffer_unref(&context);
        for (auto node = start; node < nodes.size(); ++node)
            if (av_hwdevice_ctx_create(&context, AV_HWDEVICE_TYPE_VAAPI, nodes[node].c_str(),
                                       nullptr, 0) >= 0)
                return node;
        return {};
    }
};
} // namespace
struct DecodeSession::State final {
    explicit State(bool hardwareValue) : hardware(hardwareValue) {}
    bool hardware = false;
    provider::Digest digest;
    std::uint64_t bytes = 0;
    std::unique_ptr<Input> input;
    VideoCursor cursor;
    std::optional<std::uint32_t> videoStream;
    std::optional<provider::DemuxIndex> demux;
    AudioCursor audio;
    // Hardware mode: discovered render nodes, the selected node and its parity proof.
    std::optional<std::vector<std::string>> nodes;
    std::size_t node = 0;
    Device device;
    bool parityProven = false;
    void reset() {
        input.reset();
        cursor = {};
        videoStream.reset();
        demux.reset();
        audio = {};
    }
};
DecodeSession::DecodeSession(bool hardware) : state_(std::make_unique<State>(hardware)) {}
DecodeSession::~DecodeSession() = default;
provider::Payload DecodeSession::call(const provider::CallRequest& request) {
    auto& state = *state_;
    try {
        av_log_set_level(AV_LOG_ERROR);
        const auto handshake = state.hardware ? provider::ffmpegHardwareDecodeHandshake()
                                              : provider::ffmpegHandshake();
        require(std::ranges::any_of(handshake.declarations,
                                    [&](const auto& declaration) {
                                        return declaration.capability == request.capability;
                                    }),
                "Unsupported FFmpeg capability", provider::Error::Unavailable);
        require(request.snapshotBytes != 0,
                "Retained decoder requires an immutable input capability",
                provider::Error::IdentityMismatch);
        if (state.input) {
            require(request.sourceDigest == state.digest && request.snapshotBytes == state.bytes,
                    "Retained decoder source generation changed", provider::Error::SourceChanged);
        } else {
            state.input = openInput(request);
            state.digest = request.sourceDigest;
            state.bytes = request.snapshotBytes;
        }
        if (request.capability.role == provider::Role::Probe)
            return probe(*state.input, state.digest, state.bytes);
        if (request.capability.role == provider::Role::DemuxIndex) {
            if (!state.demux) {
                // Index scanning must not disturb the retained video decoder's packet cursor.
                auto reader = openInput(request);
                state.demux = index(*reader);
            }
            return *state.demux;
        }
        if (request.capability.role == provider::Role::AudioDecode && !state.hardware) {
            require(request.stream < state.input->format->nb_streams, "Stream does not exist");
            require(request.capability.codec ==
                        avcodec_get_name(
                            state.input->format->streams[request.stream]->codecpar->codec_id),
                    "Stream codec does not match pinned capability",
                    provider::Error::IdentityMismatch);
            // A block starting where the previous one ended continues the same decode pass;
            // anything else reopens an independent reader and decodes from the start, which
            // keeps codec priming exact. The video cursor is never disturbed.
            if (!state.audio.input || state.audio.stream != request.stream ||
                state.audio.next != static_cast<std::int64_t>(request.frame))
                openAudio(state.audio, openInput(request), request.stream);
            return audio(state.audio, request);
        }
        require(request.capability.role == provider::Role::VideoDecode,
                "Retained decoder accepts video and audio only", provider::Error::Unavailable);
        require(request.stream < state.input->format->nb_streams, "Stream does not exist");
        require(
            request.capability.codec ==
                avcodec_get_name(state.input->format->streams[request.stream]->codecpar->codec_id),
            "Stream codec does not match pinned capability", provider::Error::IdentityMismatch);
        if (state.videoStream && request.stream != *state.videoStream) {
            state.input = openInput(request);
            state.cursor = {};
        }
        if (!state.hardware) {
            auto result = video(*state.input, request, nullptr, &state.cursor);
            state.videoStream = request.stream;
            return result;
        }
        // Hardware mode: walk the discovered render nodes until one decodes this stream on the
        // GPU. A node whose driver lacks the profile makes FFmpeg fall back to software frames;
        // that is a miss here, never a silently relabelled software frame.
        if (!state.nodes)
            state.nodes = renderNodes();
        while (true) {
            if (!state.device.context) {
                const auto opened = state.device.open(*state.nodes, state.node);
                require(opened.has_value(),
                        "No discovered VA-API render node decodes this stream; software decode "
                        "required",
                        provider::Error::Unavailable);
                state.node = *opened;
                state.parityProven = false;
            }
            try {
                auto result = video(*state.input, request, state.device.context, &state.cursor);
                state.videoStream = request.stream;
                if (!state.parityProven) {
                    // Each worker generation proves its first hardware frame against the software
                    // reference decoder on an independent reader of the same immutable input.
                    auto reference = openInput(request);
                    require(video(*reference, request, nullptr) == result,
                            "VA-API decode differs from the software reference; software decode "
                            "required",
                            provider::Error::IdentityMismatch);
                    state.parityProven = true;
                }
                return result;
            } catch (const HardwareMiss&) {
                av_buffer_unref(&state.device.context);
                ++state.node;
                state.input = openInput(request);
                state.cursor = {};
            }
        }
    } catch (const provider::Unavailable& error) {
        state.reset();
        return error;
    } catch (const std::bad_alloc&) {
        state.reset();
        return provider::Unavailable{provider::Error::Oversized, "Media allocation limit"};
    } catch (const std::filesystem::filesystem_error&) {
        state.reset();
        return provider::Unavailable{provider::Error::Io, "Media file unavailable"};
    }
}
provider::Payload call(const provider::CallRequest& request, bool hardware) {
    try {
        av_log_set_level(AV_LOG_ERROR);
        const auto h = provider::ffmpegHandshake(hardware);
        require(
            std::ranges::any_of(h.declarations,
                                [&](const auto& d) { return d.capability == request.capability; }),
            "Unsupported FFmpeg capability", provider::Error::Unavailable);
        const auto identity = request.snapshotBytes != 0
                                  ? std::pair{request.sourceDigest, request.snapshotBytes}
                                  : sourceIdentity(request.source);
        const auto& [digest, bytes] = identity;
        if (request.capability.role != provider::Role::Probe)
            require(digest == request.sourceDigest, "Media source content changed",
                    provider::Error::SourceChanged);
        auto input = openInput(request);
        provider::Payload result;
        if (request.capability.role == provider::Role::Probe)
            result = probe(*input, digest, bytes);
        else if (request.capability.role == provider::Role::DemuxIndex)
            result = index(*input);
        else {
            require(request.stream < input->format->nb_streams, "Stream does not exist");
            require(
                request.capability.codec ==
                    avcodec_get_name(input->format->streams[request.stream]->codecpar->codec_id),
                "Stream codec does not match pinned capability", provider::Error::IdentityMismatch);
            if (request.capability.role == provider::Role::VideoDecode) {
                Device device;
                if (hardware)
                    require(device.open(renderNodes(), 0).has_value(), "VA-API device unavailable",
                            provider::Error::Unavailable);
                try {
                    result = video(*input, request, device.context);
                } catch (const HardwareMiss&) {
                    throw provider::Unavailable{provider::Error::Unavailable,
                                                "Hardware decode not active"};
                }
            } else {
                // One-shot request: decode from the beginning to retain codec priming and exact
                // placement. The supervisor watchdog bounds long reads.
                AudioCursor cursor;
                openAudio(cursor, std::move(input), request.stream);
                result = audio(cursor, request);
                input = std::move(cursor.input);
            }
        }
        require(input->snapshot || sourceIdentity(request.source).first == digest,
                "Source changed during media read", provider::Error::SourceChanged);
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
