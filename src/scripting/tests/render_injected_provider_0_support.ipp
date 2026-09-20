// Support fragment for render_injected_provider_test.cpp. Included inside that file's anonymous
// namespace after its includes and aliases. Holds the shared fixture vocabulary: the failure
// collector, a temp directory, an independent PNG decoder, the real packaged GPU provider option
// builder, the solid session builder, the per-frame production identity writer, the paired
// GPU/CPU PNG evidence builder, and the scripted proof publisher. No test is defined here.

// A skip for the proof CTest is exit 77; --require-device turns it into a failure.
enum class GpuProofOutcome : std::uint8_t { NotRequested, Skipped, Ran };

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 64 && path_.empty(); ++attempt) {
            auto candidate = base / ("bloom-render-injected-" + std::to_string(seed) + "-" +
                                     std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error) && !error) {
                path_ = std::move(candidate);
            }
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

// A from-scratch PNG reader (raw chunk parse + zlib inflate) that never calls into bloom::output's
// own PNG writer/verifier, so the scripted PNG parity proves the published artifact against a
// second, independent reading.
struct IndependentPngDecode final {
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool everyRowFilterZero = false;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] std::uint32_t readBigEndianU32(const std::vector<unsigned char>& bytes,
                                             const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] IndependentPngDecode independentlyDecodePng(const std::filesystem::path& path) {
    IndependentPngDecode result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return result;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
    static constexpr std::array<unsigned char, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 8 || !std::equal(kSignature.begin(), kSignature.end(), bytes.begin())) {
        return result;
    }
    std::vector<unsigned char> idatConcat;
    std::size_t offset = 8;
    bool sawIend = false;
    while (offset + 8 <= bytes.size()) {
        const auto length = readBigEndianU32(bytes, offset);
        const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(offset) + 4,
                               bytes.begin() + static_cast<std::ptrdiff_t>(offset) + 8);
        const auto dataOffset = offset + 8;
        if (dataOffset + length + 4 > bytes.size()) {
            return result;
        }
        const std::span<const unsigned char> data(bytes.data() + dataOffset, length);
        if (type == "IHDR") {
            if (length != 13) {
                return result;
            }
            result.width = readBigEndianU32(bytes, dataOffset);
            result.height = readBigEndianU32(bytes, dataOffset + 4);
        } else if (type == "IDAT") {
            idatConcat.insert(idatConcat.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            sawIend = true;
        }
        offset = dataOffset + length + 4;
        if (sawIend) {
            break;
        }
    }
    if (!sawIend || offset != bytes.size() || result.width == 0 || result.height == 0) {
        return result;
    }
    const auto expectedTotal =
        static_cast<std::size_t>(result.width) * result.height * 4 + result.height;
    std::vector<unsigned char> inflated(expectedTotal);
    z_stream inflateStream{};
    if (inflateInit(&inflateStream) != Z_OK) {
        return result;
    }
    inflateStream.next_in = idatConcat.data();
    inflateStream.avail_in = static_cast<uInt>(idatConcat.size());
    inflateStream.next_out = inflated.data();
    inflateStream.avail_out = static_cast<uInt>(inflated.size());
    const auto status = inflate(&inflateStream, Z_FINISH);
    inflateEnd(&inflateStream);
    if (status != Z_STREAM_END || inflateStream.avail_out != 0) {
        return result;
    }
    result.everyRowFilterZero = true;
    result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    const auto rowRgbaBytes = static_cast<std::size_t>(result.width) * 4;
    for (std::uint32_t row = 0; row < result.height; ++row) {
        const auto rowOffset = static_cast<std::size_t>(row) * (rowRgbaBytes + 1);
        if (inflated[rowOffset] != 0) {
            result.everyRowFilterZero = false;
        }
        const auto destinationOffset = static_cast<std::size_t>(row) * rowRgbaBytes;
        std::copy_n(inflated.begin() + static_cast<std::ptrdiff_t>(rowOffset) + 1, rowRgbaBytes,
                    result.rgba.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
    }
    result.ok = true;
    return result;
}

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions
optionsFor(const std::filesystem::path& loader) {
    runtime::GpuProcessFrameEvaluatorOptions options;
    // A deliberately absent loader keeps a real owner worker alive (because enabled is true) but
    // never publishes a device, so retirementComplete() is only true after we retire it.
    options.enabled = true;
    options.loaderPath = loader;
    // The genuine production packaged resolver: the same packaged GPU shader tools the desktop,
    // CLI, and MCP roots compose from their own target definitions. Without it the GPU output
    // display command cannot be prepared, and the scripted PNG route must not publish a proof.
    options.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
    return options;
}

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions disabledOptions() {
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false;
    return options;
}

// A real session whose composition is inside the currently prepared-GPU subset (a translated solid
// layer), so Render::run genuinely evaluates on the device when one is available.
[[nodiscard]] std::unique_ptr<scripting::Session> buildSolidSession(Expectations& expectations) {
    const auto format = document::CompositionFormat::create(32, 32);
    expectations.expect(format.has_value(), "native render: the composition format is valid");
    if (!format.has_value()) {
        return nullptr;
    }
    auto created = scripting::Session::createNew("Native GPU Render", "Main",
                                                 core::RationalTime::fromInteger(2), *format);
    expectations.expect(static_cast<bool>(created), "native render: the session is created");
    if (!created) {
        return nullptr;
    }
    auto session = std::move(created).takeSession();
    const auto compositionId = session->snapshot().project().compositions().front().id();
    commands::Transaction addSolid("Add a solid", session->snapshot().revision());
    addSolid.emplace<commands::AddSolidLayer>(
        compositionId, "Moving", core::Color4d{0.25, 0.5, 0.75, 1.0}, document::Vec2d{16.0, 16.0});
    auto result = session->execute(std::move(addSolid));
    expectations.expect(result.succeeded(), "native render: the solid layer is added");
    if (!result.succeeded()) {
        return nullptr;
    }
    return session;
}

// The per-frame production identity material the scripting facade can genuinely observe: the real
// compiled plan identity plus the exact frame request and preset. Render::run compiles the same
// plan, so this is the production plan/request identity, not a placeholder.
struct FrameIdentityFields final {
    std::string_view routeId;
    std::uint64_t projectId = 0;
    std::uint64_t compositionId = 0;
    std::uint64_t sourceRevision = 0;
    std::uint64_t outputIndex = 0;
    std::uint64_t operationCount = 0;
    std::uint32_t planSemantics = 0;
    std::uint32_t animationSamplingSemantics = 0;
    std::uint64_t frameIndex = 0;
    std::int64_t timeNumerator = 0;
    std::int64_t timeDenominator = 1;
    std::uint64_t preset = 0;
    std::string_view provider;
};

[[nodiscard]] std::string frameIdentityHex(const FrameIdentityFields& fields) {
    routeproof::CanonicalWriter writer;
    writer.text("bloom.gpu.route.export-frame-identity.v1");
    writer.text(fields.routeId);
    writer.u64(fields.projectId);
    writer.u64(fields.compositionId);
    writer.u64(fields.sourceRevision);
    writer.u64(fields.outputIndex);
    writer.u64(fields.operationCount);
    writer.u32(fields.planSemantics);
    writer.u32(fields.animationSamplingSemantics);
    writer.u64(fields.frameIndex);
    writer.i64(fields.timeNumerator);
    writer.i64(fields.timeDenominator);
    writer.u64(fields.preset);
    writer.text(fields.provider);
    return routeproof::sha256Hex(writer.bytes());
}

// The actual paired GPU/CPU comparison of one independently decoded PNG frame: RGB within one 8-bit
// code, alpha exactly. The evidence digest is bound to the exact decoded bytes.
[[nodiscard]] routeproof::FrameEvidence comparePngEvidence(const IndependentPngDecode& gpu,
                                                           const IndependentPngDecode& cpu,
                                                           std::string identityHex) {
    routeproof::FrameEvidence evidence;
    evidence.identityHex = std::move(identityHex);
    evidence.comparedPixels = cpu.rgba.size();
    evidence.alphaExact = true;
    std::uint64_t maxIntegerDelta = 0;
    std::uint64_t mismatches = 0;
    if (gpu.ok && cpu.ok && gpu.rgba.size() == cpu.rgba.size()) {
        for (std::size_t index = 0; index < cpu.rgba.size(); ++index) {
            const auto difference = static_cast<std::uint64_t>(
                std::abs(static_cast<int>(cpu.rgba[index]) - static_cast<int>(gpu.rgba[index])));
            maxIntegerDelta = std::max(maxIntegerDelta, difference);
            const bool alpha = index % 4U == 3U;
            if (alpha) {
                if (difference != 0) {
                    evidence.alphaExact = false;
                    ++mismatches;
                }
            } else if (difference > 1) {
                ++mismatches;
            }
        }
    }
    evidence.maxIntegerDelta = maxIntegerDelta;
    evidence.mismatchedPixels = mismatches;
    evidence.cpuDecodedDigest = routeproof::sha256Pixels(std::span<const std::uint8_t>(cpu.rgba));
    evidence.gpuDecodedDigest = routeproof::sha256Pixels(std::span<const std::uint8_t>(gpu.rgba));
    return evidence;
}

// Publishes one scripted PNG route proof from the exact aggregate the run reported. The route
// requires the genuine GPU output-colour arm: exactly one final readback submission per verified
// frame carrying TWO payloads each (process-analysis + encoded display) with real encoded bytes. A
// CPU output-colour omission (one payload, no encoded bytes) or a CPU fallback (all zero) can never
// satisfy this, so it can never write a green proof. The writer then rejects a zero-dispatch /
// no-frame / over-policy proof independently.
void publishScriptedProof(Expectations& expectations, const std::filesystem::path& proofDirectory,
                          const std::string_view routeId,
                          const bloom::runtime::GpuRouteHarnessKind harness,
                          const scripting::RenderResult& result,
                          const std::vector<std::string>& identityHex,
                          const std::vector<routeproof::FrameEvidence>& evidence) {
    if (proofDirectory.empty()) {
        return;
    }
    const auto frames = static_cast<std::uint64_t>(identityHex.size());
    const bool displayArm = frames != 0 && result.gpuEvaluatedFrames == frames &&
                            result.gpuReadbackSubmissions == frames &&
                            result.gpuTransferredPayloads == frames * 2U &&
                            result.gpuProcessPayloadBytes > 0 && result.gpuEncodedPayloadBytes > 0;
    expectations.expect(displayArm,
                        "scripted proof: the route requires the genuine GPU display-colour arm "
                        "(one submission and two payloads per frame, with encoded bytes)");
    if (!displayArm) {
        return;
    }
    routeproof::ExportProofCounters counters;
    counters.deviceOwnershipEpoch = result.gpuDeviceOwnershipEpoch;
    counters.nativeDispatches = result.gpuNativeDispatches;
    counters.verifiedFrames = frames;
    // The ACTUAL combined-readback counters the per-frame attempts reported. Never derived from the
    // submission count or the frame dimensions.
    counters.readbackSubmissions = result.gpuReadbackSubmissions;
    counters.payloads = result.gpuTransferredPayloads;
    counters.transferredBytes = result.gpuProcessPayloadBytes + result.gpuEncodedPayloadBytes;
    std::string nonce;
    if (!routeproof::readProofNonce(proofDirectory, nonce)) {
        expectations.expect(false, "scripted proof: a fresh run nonce is required");
        return;
    }
    const auto processDigest = routeproof::orderedIdentityDigest(identityHex);
    const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidence);
    const auto written = routeproof::publishExportProof(
        proofDirectory, nonce, routeId, harness, counters, processDigest, capturedEvidenceDigest);
    if (!written.written) {
        expectations.expect(false, std::string{"scripted proof rejected: "} + written.detail);
        return;
    }
    std::cout << "PASS(route-proof) " << routeId << " frames=" << counters.verifiedFrames
              << " dispatches=" << counters.nativeDispatches
              << " readbacks=" << counters.readbackSubmissions << " payloads=" << counters.payloads
              << " bytes=" << counters.transferredBytes << '\n';
}
