// PNG preset group (issue #111). Split verbatim from frame_export_publication_tests.cpp.

// -------------------------------------------------------------------------------------------
// PNG preset (issue #111)
// -------------------------------------------------------------------------------------------

struct IndependentPngDecode final {
    bool ok = false;
    std::vector<std::string> chunkTypesInOrder;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t colorType = 0;
    std::uint8_t interlaceMethod = 0;
    std::uint8_t srgbIntent = 0;
    bool everyCrcValid = false;
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

    result.everyCrcValid = true;
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
        auto crc = crc32_z(0L, reinterpret_cast<const Bytef*>(type.data()), 4);
        if (!data.empty()) {
            crc = crc32_z(crc, reinterpret_cast<const Bytef*>(data.data()), data.size());
        }
        const auto crcOffset = dataOffset + length;
        if (static_cast<std::uint32_t>(crc) != readBigEndianU32(bytes, crcOffset)) {
            result.everyCrcValid = false;
        }
        result.chunkTypesInOrder.push_back(type);
        if (type == "IHDR") {
            if (length != 13) {
                return result;
            }
            result.width = readBigEndianU32(bytes, dataOffset);
            result.height = readBigEndianU32(bytes, dataOffset + 4);
            result.bitDepth = data[8];
            result.colorType = data[9];
            result.interlaceMethod = data[12];
        } else if (type == "sRGB") {
            if (length != 1) {
                return result;
            }
            result.srgbIntent = data[0];
        } else if (type == "IDAT") {
            idatConcat.insert(idatConcat.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            sawIend = true;
        }
        offset = crcOffset + 4;
        if (sawIend) {
            break;
        }
    }
    if (!sawIend || offset != bytes.size()) {
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

// Independently re-derives the prepared straight-RGBA8 stream from the attempt's retained process
// frame, through a FRESHLY built qualified display processor (never the one the export retained),
// so G1's reopen verifier can be run against the published file with inputs the export path did
// not hand it.
[[nodiscard]] bool
independentlyVerifyPng(Expectations& expectations,
                       const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
                       const std::filesystem::path& target,
                       const bloom::core::Sha256Digest& expectedSemanticDigest) {
    const auto built = runtime::buildBloomNeutralQualifiedDisplayProcessor();
    expectations.expect(built.succeeded(),
                        "independent PNG verify: a fresh qualified processor builds");
    if (!built.succeeded()) {
        return false;
    }
    const runtime::CpuQualifiedDisplayPreparer preparer(*built.handle());
    const auto prepared = preparer.prepare(
        attempt->frame(),
        {.aggregatePixelStorageByteLimit = output::kOutputExportPreparedPngBytesMaximumV1,
         .chunkPixelCount = runtime::kDefaultQualifiedDisplayChunkPixelCount,
         .viewAdjust = {},
         .displayName = {},
         .viewName = {},
         .showLook = true},
        {});
    expectations.expect(prepared.status() == runtime::QualifiedDisplayPreparationStatus::Prepared &&
                            prepared.frame() != nullptr,
                        "independent PNG verify: the display frame re-derives");
    if (prepared.status() != runtime::QualifiedDisplayPreparationStatus::Prepared ||
        prepared.frame() == nullptr) {
        return false;
    }
    const auto& buffer = prepared.frame()->buffer();
    const output::PngRgba8SrgbPreparedStreamV1 stream{.dimensions = buffer.displayWindow().extent(),
                                                      .pixels = buffer.pixels()};
    const auto& display = attempt->display();
    expectations.expect(display.isPresent(),
                        "independent PNG verify: the attempt retains its display products");
    bloom::core::Sha256Digest revision;
    if (display.expectedOcioRevision.has_value()) {
        revision = *display.expectedOcioRevision;
    } else {
        return false;
    }
    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto verifyResult = verifier.verify(target, stream, attempt->processIdentity(),
                                              attempt->report(), revision, display.identity, {});
    expectations.expect(verifyResult.status() == output::PngVerifyStatusV1::Verified,
                        "independent PNG verify: the published file reopen-verifies");
    if (verifyResult.status() != output::PngVerifyStatusV1::Verified) {
        return false;
    }
    expectations.expect(verifyResult.digest() == expectedSemanticDigest,
                        "independent PNG verify: the independently issued kind-1 digest equals the "
                        "digest the export surfaced");
    return true;
}

void testEndToEndPngExportPublished(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png end to end: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "published.png";
    auto attempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), target, output::OutputPresetV1::PngRgba8SrgbV1);
    if (attempt == nullptr) {
        return;
    }
    expectations.expect(attempt->preset() == output::OutputPresetV1::PngRgba8SrgbV1 &&
                            attempt->display().isPresent(),
                        "png end to end: the attempt is a PNG attempt retaining its qualified "
                        "display-processor handle, identity, and expected OCIO revision");
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "png end to end: approval succeeds");
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "png end to end: the export job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "png end to end: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    expectations.expect(static_cast<bool>(*result) && result->publication() != nullptr &&
                            result->publication()->outcome ==
                                platform::StagedArtifactPublicationOutcome::Published,
                        "png end to end: the export publishes");
    expectations.expect(std::filesystem::exists(target),
                        "png end to end: the target file now exists");
    const auto semanticDigest = result->semanticDigest();
    expectations.expect(semanticDigest.has_value() && result->artifactDigest().has_value(),
                        "png end to end: both the semantic and artifact digests are surfaced");
    if (!semanticDigest.has_value()) {
        return;
    }

    const auto decoded = independentlyDecodePng(target);
    expectations.expect(decoded.ok && decoded.everyCrcValid && decoded.everyRowFilterZero,
                        "png end to end: the published file independently decodes with valid CRCs "
                        "and zero row filters");
    expectations.expect(decoded.chunkTypesInOrder ==
                            std::vector<std::string>{"IHDR", "sRGB", "IDAT", "IEND"},
                        "png end to end: the published file carries exactly the closed chunk "
                        "profile");
    expectations.expect(decoded.width == 2 && decoded.height == 2 && decoded.bitDepth == 8 &&
                            decoded.colorType == 6 && decoded.interlaceMethod == 0 &&
                            decoded.srgbIntent == 0,
                        "png end to end: the independent decode sees the required IHDR/sRGB "
                        "fields");

    static_cast<void>(independentlyVerifyPng(
        expectations, attempt, target,
        *semanticDigest)); // NOLINT(bugprone-unchecked-optional-access) -- guarded above.
}

void testPngSemanticDigestStableAcrossTwoRuns(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png digest stability: fixture is available")) {
        return;
    }
    std::array<std::optional<bloom::core::Sha256Digest>, 2> digests;
    for (int index = 0; index < 2; ++index) {
        const auto target = fixture.path() / ("stable-" + std::to_string(index) + ".png");
        auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(),
                                              fixture.artifacts(), fixture.ledger(), target,
                                              output::OutputPresetV1::PngRgba8SrgbV1);
        if (attempt == nullptr) {
            return;
        }
        auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                                   requireDigest(attempt, expectations));
        if (!approval) {
            expectations.expect(false, "png digest stability: approval succeeds");
            return;
        }
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        auto* runValue = require(run, expectations, "png digest stability: the job is submitted");
        if (runValue == nullptr) {
            return;
        }
        auto resultOpt = awaitExportRun(*runValue);
        const auto* result = require(resultOpt, expectations,
                                     "png digest stability: the job reaches a terminal state");
        if (result == nullptr) {
            return;
        }
        digests.at(static_cast<std::size_t>(index)) = result->semanticDigest();
    }
    expectations.expect(digests[0].has_value() && digests[1].has_value() &&
                            digests[0] == digests[1],
                        "png digest stability: the kind-1 semantic identity digest is identical "
                        "across two full publications of the identical fixture");
}

void testBothPresetsExportFromTheSameFixture(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "both presets: fixture is available")) {
        return;
    }
    const auto exrTarget = fixture.path() / "both.exr";
    const auto pngTarget = fixture.path() / "both.png";

    auto exrAttempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                             fixture.ledger(), exrTarget);
    auto pngAttempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), pngTarget, output::OutputPresetV1::PngRgba8SrgbV1);
    if (exrAttempt == nullptr || pngAttempt == nullptr) {
        return;
    }
    expectations.expect(exrAttempt->preset() ==
                                output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 &&
                            exrAttempt->display().isAbsent(),
                        "both presets: the EXR attempt retains no display product at all");
    expectations.expect(pngAttempt->preset() == output::OutputPresetV1::PngRgba8SrgbV1 &&
                            pngAttempt->display().isPresent(),
                        "both presets: the PNG attempt retains its display products");
    expectations.expect(*exrAttempt->digest() != *pngAttempt->digest(), // NOLINT
                        "both presets: the two presets' approval digests differ over the same "
                        "frame");

    std::array<std::optional<bloom::core::Sha256Digest>, 2> semantic;
    const std::array<std::shared_ptr<const output::OutputAnalysisAttemptV1>, 2> attempts{
        exrAttempt, pngAttempt};
    for (std::size_t index = 0; index < attempts.size(); ++index) {
        auto approval = host::approveFrameExportV1(fixture.coordinator(), attempts.at(index),
                                                   requireDigest(attempts.at(index), expectations));
        if (!approval) {
            expectations.expect(false, "both presets: approval succeeds");
            return;
        }
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        auto* runValue = require(run, expectations, "both presets: the job is submitted");
        if (runValue == nullptr) {
            return;
        }
        auto resultOpt = awaitExportRun(*runValue);
        const auto* result =
            require(resultOpt, expectations, "both presets: the job reaches a terminal state");
        if (result == nullptr) {
            return;
        }
        expectations.expect(static_cast<bool>(*result) && result->publication() != nullptr &&
                                result->publication()->outcome ==
                                    platform::StagedArtifactPublicationOutcome::Published,
                            "both presets: the export publishes");
        semantic.at(index) = result->semanticDigest();
    }
    expectations.expect(std::filesystem::exists(exrTarget) && std::filesystem::exists(pngTarget),
                        "both presets: both target files exist");
    expectations.expect(semantic[0].has_value() && semantic[1].has_value() &&
                            semantic[0] != semantic[1],
                        "both presets: the two presets' output semantic identities differ");

    // EXR magic (0x76 0x2f 0x31 0x01) vs the PNG signature -- proof each preset actually wrote its
    // own container, not the other one under a different extension.
    const auto readMagic = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        std::array<char, 4> magic{};
        stream.read(magic.data(), 4);
        return magic;
    };
    const auto exrMagic = readMagic(exrTarget);
    const auto pngMagic = readMagic(pngTarget);
    expectations.expect(static_cast<unsigned char>(exrMagic[0]) == 0x76U &&
                            static_cast<unsigned char>(exrMagic[1]) == 0x2FU,
                        "both presets: the .exr target really is an OpenEXR file");
    expectations.expect(static_cast<unsigned char>(pngMagic[0]) == 137U && pngMagic[1] == 'P' &&
                            pngMagic[2] == 'N' && pngMagic[3] == 'G',
                        "both presets: the .png target really is a PNG file");
}

void testPngPreparedBytesLimitExceededIsTyped(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png prepared limit: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "prepared-limit.png";
    auto attempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), target, output::OutputPresetV1::PngRgba8SrgbV1);
    if (attempt == nullptr) {
        return;
    }
    // A LOWERED prepared-bytes ceiling ("a request may lower but not raise them"): the 2x2 fixture
    // needs 16 prepared bytes, so 8 is over-limit. The closed 256 MiB ceiling itself can never be
    // exceeded through the production pipeline (the closed 2^26 pixel-count limit caps prepared
    // bytes at exactly 256 MiB), so this seam is the only way to reach the rejection -- see
    // output_limits.hpp's own reachability note.
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations),
                                               {.totalDeadline = std::chrono::hours(24),
                                                .noProgressInterval = std::chrono::seconds(120),
                                                .preparedPngByteLimit = 8});
    expectations.expect(static_cast<bool>(approval), "png prepared limit: approval succeeds");
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "png prepared limit: the job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "png prepared limit: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    const auto* failure = result->failure();
    expectations.expect(failure != nullptr &&
                            failure->payloadAs<host::FrameExportPreparedBytesExceededV1>() !=
                                nullptr &&
                            failure->stage() == host::FrameExportPublicationStageV1::ColorPreparing,
                        "png prepared limit: an over-limit prepared stream is a typed "
                        "FrameExportPreparedBytesExceededV1 failure at ColorPreparing");
    expectations.expect(!std::filesystem::exists(target),
                        "png prepared limit: nothing is published");
}

void testPngColorPreparingCancellationPublishesNothing(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png color cancellation: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "colour-cancelled.png";
    auto attempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), target, output::OutputPresetV1::PngRgba8SrgbV1);
    if (attempt == nullptr) {
        return;
    }
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "png color cancellation: approval succeeds");
    if (!approval) {
        return;
    }

    auto shared = std::make_shared<std::optional<host::FrameExportPublicationResultV1>>();
    auto sharedRequest =
        std::shared_ptr<host::FrameExportRequestV1>(std::move(approval).takeRequest());
    auto scratchDir = fixture.path();
    auto reachedColorPreparing = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    auto submission = fixture.scheduler().submit<void>(
        runtime::TaskRequest("export-publication-color-cancel-test",
                             runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                .id = runtime::TaskOwnerId::fromRaw(4)},
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [&artifacts = fixture.artifacts(), sharedRequest, shared, scratchDir, reachedColorPreparing,
         released](runtime::TaskContext& context) {
            // Blocks at the one ColorPreparing report emitted immediately before C2's chunked
            // apply loop, giving the outer thread a deterministic point to request cancellation --
            // no sleep, no race against a background worker.
            output::OutputExportProgressCallbackV1 progress =
                [reachedColorPreparing, released](const output::OutputExportProgressV1& update) {
                    if (update.stage != output::OutputExportStageV1::ColorPreparing ||
                        update.completed != 0 || update.total != 0) {
                        return;
                    }
                    reachedColorPreparing->store(true, std::memory_order_release);
                    while (!released->load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                };
            auto result = host::executeExportPublication(
                context, artifacts, std::move(*sharedRequest), scratchDir, {}, std::move(progress));
            shared->emplace(std::move(result));
            return runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted(), "png color cancellation: the job is submitted");
    if (!submission.accepted()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!reachedColorPreparing->load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(reachedColorPreparing->load(std::memory_order_acquire),
                        "png color cancellation: the job reaches the ColorPreparing rendezvous");
    submission.handle.cancel();
    released->store(true, std::memory_order_release);

    ExportRun run{std::move(submission.handle), shared};
    auto resultOpt = awaitExportRun(run);
    const auto* result = require(resultOpt, expectations,
                                 "png color cancellation: the job reaches a terminal state");
    if (result != nullptr) {
        expectations.expect(!*result && result->publication() == nullptr,
                            "png color cancellation: a cancelled job never reaches publish()");
    }
    expectations.expect(!std::filesystem::exists(target),
                        "png color cancellation: cancellation during ColorPreparing publishes "
                        "nothing");
}
