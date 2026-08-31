#pragma once

// Shared fixture/harness support for the PNG writer and reopen-verifier test binaries (issue
// #107). Mirrors flat_exr_test_support.hpp's golden idiom for obtaining a real
// `ProcessFrameSemanticIdentityV1` (evaluate a trivial one-pixel plan through the actual
// `CpuCompositionEvaluator`, then move-assign an arbitrary fixture identity/image into the
// published frame) and adds the two PNG-only fixtures flat OpenEXR never needed: a real
// `DisplayProcessorIdentityV1` (built through the color module's own public
// validate/write/adopt sequence -- the same one display_processor_identity_tests.cpp's
// `goldenInput()` idiom exercises) and the caller-supplied `PngRgba8SrgbPreparedStreamV1` RGBA8
// byte fixture itself (this preset's encoder input surface, independent of the process frame's
// own RGBA32F content -- see png_output_adapter.hpp's own design-decision-3 rationale).

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>

// Used only by independentlyDecodePng() below -- a from-scratch reader (raw chunk parse + zlib
// inflate) that never calls into src/output's own png_output_adapter.cpp/png_reopen_verifier.cpp,
// so a round-trip test can "prove the encoder against a second reading" (design decision's own
// wording) rather than exercising the verifier's internals a second time.
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom_output_png_test_support {

namespace color = bloom::color;
namespace core = bloom::core;
namespace document = bloom::document;
namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

constexpr auto kProjectId = document::ProjectId::fromRaw(0x11121314151617ULL);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0x21222324252627ULL);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0x31323334353637ULL);
constexpr auto kInputNodeId = document::NodeId::fromRaw(0x41424344454647ULL);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0x51525354555657ULL);
constexpr auto kSourceRevision = document::Revision::fromRaw(0x61626364656667ULL);
constexpr auto kShellLayerNodeId = document::NodeId::fromRaw(0x71);
constexpr auto kShellLayerId = document::LayerId::fromRaw(0x72);
constexpr auto kShellStackNodeId = document::NodeId::fromRaw(0x73);
constexpr auto kShellSlotId = document::LayerSlotId::fromRaw(0x74);
constexpr auto kShellPositionParameterId = document::ParameterId::fromRaw(0x75);
constexpr auto kShellOpacityParameterId = document::ParameterId::fromRaw(0x76);

// The revision fixture bytes both `expectedOcioRevision` and the built `DisplayProcessorIdentityV1`
// embed -- the doc's own cross-check ("the separate expected OCIO revision must equal ... the
// revision embedded in the canonical DisplayProcessorIdentity").
constexpr core::Sha256Digest::Bytes kRevisionBytes{
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
};

[[nodiscard]] inline render::ImageWindow window(const std::int64_t originX,
                                                const std::int64_t originY,
                                                const std::uint32_t width,
                                                const std::uint32_t height) {
    const auto value = render::ImageWindow::create(originX, originY, width, height);
    if (!value) {
        std::abort();
    }
    return *value.value();
}

[[nodiscard]] inline render::Rgba32f pixel(const float red, const float green, const float blue,
                                           const float alpha) {
    const auto value = render::Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!value) {
        std::abort();
    }
    return *value.value();
}

// The process frame's own pixel content is never observed by the PNG verifier (design decision 3:
// the prepared RGBA8 stream, not the process frame, is what gets compared bit-exact) -- it only
// needs a valid image whose data/display windows equal (0,0,width,height) and whose pixel aspect
// is square, matching PNG's own required source-descriptor shape
// (docs/architecture/frame-output.md, "PNG Preset Version 1": "origin (0, 0), identical data and
// display windows, and square pixel aspect").
[[nodiscard]] inline render::Rgba32fImage pngShapedImage(const std::uint32_t width,
                                                         const std::uint32_t height) {
    const auto dataWindow = window(0, 0, width, height);
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        dataWindow, dataWindow, core::PixelAspectRatio::square());
    if (!descriptor) {
        std::abort();
    }
    auto builder = render::Rgba32fImageBuilder::create(
        *descriptor.value(), descriptor.value()->layout().pixelStorageBytes);
    if (!builder) {
        std::abort();
    }
    const auto fill = pixel(0.25F, 0.5F, 0.75F, 1.0F);
    for (std::uint32_t rowIndex = 0; rowIndex < height; ++rowIndex) {
        auto row = builder.value()->row(static_cast<std::int64_t>(rowIndex));
        if (!row) {
            std::abort();
        }
        std::ranges::fill(*row.value(), fill);
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        std::abort();
    }
    return std::move(*frozen.value());
}

// Composition Output requires a layer-stack input, not a bare solid (mirrors
// flat_exr_test_support.hpp's shellPlan()). This plan's own pixels are never observed: publish()
// immediately overwrites the evaluated frame's image/identity with a fixture.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan> shellPlan() {
    const auto format = document::CompositionFormat::create(1, 1);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kInputNodeId, kColorParameterId, {0.0, 0.0, 0.0, 0.0}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kShellLayerNodeId, kShellLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{kShellPositionParameterId, document::Vec2d{0.5, 0.5}},
        runtime::CompiledScalarParameter{kShellOpacityParameterId, 1.0}});
    operations.emplace_back(runtime::CompiledLayerStack{
        kShellStackNodeId, {{kShellSlotId, kShellLayerId, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNodeId, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{.sourceRevision = kSourceRevision,
                                                   .projectId = kProjectId,
                                                   .compositionId = kCompositionId,
                                                   .format = *format,
                                                   .operations = std::move(operations),
                                                   .output = runtime::OperationIndex::fromRaw(3)});
}

// A minimal plan used only to give ProcessFrameIdentity an opaque `.plan` handle whose
// CompositionFormat declares the exact width/height a fixture's display window claims (mirrors
// flat_exr_test_support.hpp's identityPlan()). This plan is never evaluated.
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
identityPlan(const std::uint32_t width, const std::uint32_t height) {
    const auto format = document::CompositionFormat::create(width, height);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{kInputNodeId, kColorParameterId, {}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNodeId, runtime::OperationIndex::fromRaw(0)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{.sourceRevision = kSourceRevision,
                                                   .projectId = kProjectId,
                                                   .compositionId = kCompositionId,
                                                   .format = *format,
                                                   .operations = std::move(operations),
                                                   .output = runtime::OperationIndex::fromRaw(1)});
}

[[nodiscard]] inline std::shared_ptr<const runtime::ProcessFrame> evaluateShell() {
    const auto compiledPlan = shellPlan();
    const runtime::EvaluationRequest request{.time = core::RationalTime::fromInteger(0),
                                             .output = compiledPlan->output(),
                                             .resolution = runtime::CompositionFormatResolution{},
                                             .quality = runtime::EvaluationQuality::Reference,
                                             .colorIntent =
                                                 runtime::EvaluationColorIntent::LinearRec709Scene,
                                             .pixelStorageByteLimit = 4096};
    const runtime::CpuCompositionEvaluator evaluator;
    const auto result = evaluator.evaluate(compiledPlan, request, {});
    if (result.status() != runtime::EvaluationStatus::Evaluated || result.frame() == nullptr) {
        for (const auto& diagnostic : result.diagnostics()) {
            std::cerr << "evaluateShell diagnostic: "
                      << runtime::evaluationDiagnosticCodeId(diagnostic.code)
                      << " summary=" << diagnostic.summary << " detail=" << diagnostic.detail
                      << '\n';
        }
        std::abort();
    }
    return result.frame();
}

struct Fixture final {
    runtime::ProcessFrameIdentity identity;
    render::Rgba32fImage processImage;
};

// Publishes `fixture` as the retained image/identity of a real (privately-constructed)
// `ProcessFrame` (mirrors flat_exr_test_support.hpp's publish()).
[[nodiscard]] inline std::shared_ptr<const runtime::ProcessFrame> publish(Fixture fixture) {
    auto published = std::const_pointer_cast<runtime::ProcessFrame>(evaluateShell());
    auto& identity = const_cast<runtime::ProcessFrameIdentity&>(published->identity());
    auto& processImage = const_cast<render::Rgba32fImage&>(published->processImage());
    identity = std::move(fixture.identity);
    processImage = std::move(fixture.processImage);
    return published;
}

[[nodiscard]] inline runtime::ProcessFrameIdentity
frameIdentity(const std::shared_ptr<const runtime::CompiledCompositionPlan>& compiledPlan) {
    const auto time = core::RationalTime::create(1, 2);
    if (!time) {
        std::abort();
    }
    return {.plan = compiledPlan,
            .time = *time,
            .output = compiledPlan->output(),
            .resolution = runtime::CompositionFormatResolution{},
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
            .provider = runtime::EvaluationProvider::CpuReference,
            .evaluatorSemanticsVersion = 1,
            .animationSamplingSemanticsVersion = 1,
            .imagePrimitiveSemanticsVersion = 1};
}

[[nodiscard]] inline Fixture pngShapedFixture(const std::uint32_t width,
                                              const std::uint32_t height) {
    return {frameIdentity(identityPlan(width, height)), pngShapedImage(width, height)};
}

// A real, validated `DisplayProcessorIdentityV1`, built through the color module's own public
// validate/write/adopt sequence (the same one display_processor_identity_tests.cpp's
// `goldenInput()`/`writeDisplayProcessorIdentityV1()`/`adoptDisplayProcessorIdentityV1()` idiom
// exercises). Its embedded revision is exactly kRevisionBytes.
[[nodiscard]] inline color::DisplayProcessorIdentityV1 fixtureDisplayIdentity() {
    static constexpr std::array<std::string_view, 0> kNoLooks{};
    const color::DisplayProcessorIdentityV1InputView input{
        .expectedOcioRevision = core::Sha256Digest::fromBytes(kRevisionBytes),
        .contextVariables = {},
        .sourceColorSpaceId = color::kDisplayProcessorIdentitySourceColorSpaceId,
        .displayName = "sRGB",
        .viewName = "Standard",
        .lookMode = color::DisplayProcessorLookModeV1::Bypass,
        .lookNames = kNoLooks,
        .outputColorSpaceId = color::kDisplayProcessorIdentityOutputColorSpaceId,
        .qualityId = color::kDisplayProcessorIdentityQualityId,
        .semanticsProfileId = color::kDisplayProcessorIdentitySemanticsProfileId,
        .packingId = color::kDisplayProcessorIdentityPackingId,
    };
    const auto validation = color::validateDisplayProcessorIdentityV1(input);
    if (!validation) {
        std::abort();
    }
    std::vector<std::byte> storage(validation.requiredByteCount());
    const auto written = color::writeDisplayProcessorIdentityV1(input, storage);
    if (!written) {
        std::abort();
    }
    auto adopted = color::adoptDisplayProcessorIdentityV1(std::move(storage));
    auto identity = std::move(adopted).takeIdentity();
    if (!identity) {
        std::abort();
    }
    return std::move(*identity);
}

// Both the process-identity/report pair (the exact public inputs the reopen verifier's
// processIdentity/report parameters require) and the PNG-only expectedOcioRevision/
// displayProcessorIdentity pair the kind-1 identity seam additionally requires (design decision
// 4). Mirrors flat_exr_test_support.hpp's PreparedSource, extended with the two PNG-only fields.
struct PreparedSource final {
    std::shared_ptr<const runtime::ProcessFrame> frame;
    std::shared_ptr<const output::ProcessFrameSemanticIdentityV1> processIdentity;
    std::shared_ptr<const output::OutputAnalysisReportV1> report;
    core::Sha256Digest expectedOcioRevision;
    std::shared_ptr<const color::DisplayProcessorIdentityV1> displayProcessorIdentity;
};

// Runs the already-implemented pre-approval pipeline this slice consumes but does not
// reimplement: prepare the frame-bound `ProcessFrameSemanticIdentityV1` and analyze it with the
// existing PNG analyzer -- the exact public input shape the reopen verifier requires.
[[nodiscard]] inline PreparedSource prepareSource(Fixture fixture) {
    auto frame = publish(std::move(fixture));

    const output::ProcessFrameSemanticIdentityV1Preparer identityPreparer;
    const auto identityResult = identityPreparer.prepare(frame, {});
    if (identityResult.status() !=
            output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
        identityResult.identity() == nullptr) {
        std::abort();
    }

    const auto expectedRevision = core::Sha256Digest::fromBytes(kRevisionBytes);
    const auto analyzed = output::analyzePngRgba8SrgbV1(
        {.process = {.state = output::OutputAnalysisProcessSourceStateV1::Ready,
                     .readyIdentity = identityResult.identity(),
                     .missingDescriptor = std::nullopt},
         .expectedOcioRevision = expectedRevision,
         .colorResolution = output::PngRgba8SrgbColorResolutionStateV1::Ready});
    if (!analyzed.hasReport()) {
        std::abort();
    }

    auto displayIdentity =
        std::make_shared<const color::DisplayProcessorIdentityV1>(fixtureDisplayIdentity());

    return {frame, identityResult.identity(), analyzed.report(), expectedRevision,
            std::move(displayIdentity)};
}

// RAII per-process scratch directory under the platform temp root, isolating this test binary's
// staged artifacts from other parallel ctest processes. Best-effort cleanup on destruction.
class ScratchDirectory final {
  public:
    explicit ScratchDirectory(const std::string& label) {
        static std::atomic<std::uint64_t> counter{0};
        const auto suffix =
            std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(label))) + "-" +
            std::to_string(counter.fetch_add(1));
        std::error_code errorCode;
        path_ =
            std::filesystem::temp_directory_path(errorCode) / ("bloom-output-png-test-" + suffix);
        if (errorCode) {
            std::abort();
        }
        std::filesystem::create_directories(path_, errorCode);
        if (errorCode) {
            std::abort();
        }
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    ~ScratchDirectory() {
        std::error_code errorCode;
        std::filesystem::remove_all(path_, errorCode);
    }

    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

  private:
    std::filesystem::path path_;
};

// The small "Expectations" idiom every src/output test file already uses, duplicated here so both
// PNG test binaries share one copy instead of two more (same rationale as
// flat_exr_test_support.hpp's own copy).
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

// Minimal raw byte-level access to a valid staged PNG's chunk stream, used by the adversarial
// fixtures to craft corrupted variants of a real staged file. Mirrors flat_exr_test_support.hpp's
// ExrHeaderBytes idiom (per F1's precedent, cited by design decision "Adversarial byte surgery").
// A chunk record is length(4 BE) + type(4 ASCII) + data(length bytes) + crc(4 BE), starting right
// after the 8-byte signature.
class PngChunkBytes final {
  public:
    explicit PngChunkBytes(const std::filesystem::path& path) : path_(path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            std::abort();
        }
        bytes_.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        if (bytes_.size() < 8) {
            std::abort();
        }
    }

    // Byte offset of one chunk occurrence's data (after its length/type fields) and that chunk's
    // declared data length. `occurrence` selects among repeated types (e.g. the second IDAT).
    // Aborts if not present.
    [[nodiscard]] std::pair<std::size_t, std::size_t>
    dataRange(const std::string& type, const std::size_t occurrence = 0) const {
        std::size_t offset = 8;
        std::size_t seen = 0;
        while (offset + 8 <= bytes_.size()) {
            const auto length = readU32(offset);
            const std::string chunkType(bytes_.begin() + static_cast<std::ptrdiff_t>(offset) + 4,
                                        bytes_.begin() + static_cast<std::ptrdiff_t>(offset) + 8);
            const auto dataOffset = offset + 8;
            if (chunkType == type) {
                if (seen == occurrence) {
                    return {dataOffset, length};
                }
                ++seen;
            }
            offset = dataOffset + length + 4; // data + CRC
            if (offset > bytes_.size()) {
                break;
            }
        }
        std::abort();
    }

    // Patches a single byte with NO CRC fix-up -- deliberately breaks that chunk's CRC, for the
    // "corrupted CRC" adversarial fixture itself.
    void patchByteBreakingCrc(const std::size_t offset, const unsigned char value) {
        bytes_.at(offset) = static_cast<char>(value);
    }

    // Patches one byte inside a chunk's data and recomputes/rewrites that exact chunk's trailing
    // CRC-32, so the surrounding chunk stays CRC-valid and the verifier reaches the field-value
    // check the caller actually wants to exercise (IHDR field perturbation, sRGB intent, a
    // non-zero row filter byte) rather than tripping over ChunkCrcMismatch first.
    void patchDataByteWithValidCrc(const std::string& type, const std::size_t occurrence,
                                   const std::size_t offsetWithinData, const unsigned char value) {
        const auto [dataOffset, length] = dataRange(type, occurrence);
        bytes_.at(dataOffset + offsetWithinData) = static_cast<char>(value);
        const auto typeOffset = dataOffset - 4;
        auto crc = crc32Seed();
        for (std::size_t index = 0; index < 4; ++index) {
            crc = crc32Step(crc, static_cast<unsigned char>(bytes_.at(typeOffset + index)));
        }
        for (std::size_t index = 0; index < length; ++index) {
            crc = crc32Step(crc, static_cast<unsigned char>(bytes_.at(dataOffset + index)));
        }
        crc ^= 0xFFFFFFFFU;
        std::vector<unsigned char> crcBytes;
        appendU32(crcBytes, crc);
        for (std::size_t index = 0; index < 4; ++index) {
            bytes_.at(dataOffset + length + index) = static_cast<char>(crcBytes[index]);
        }
    }

    [[nodiscard]] unsigned char byteAt(const std::size_t offset) const {
        return static_cast<unsigned char>(bytes_.at(offset));
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    void truncateTo(const std::size_t length) { bytes_.resize(std::min(length, bytes_.size())); }

    void appendBytes(const std::span<const unsigned char> extra) {
        bytes_.insert(bytes_.end(), extra.begin(), extra.end());
    }

    // Inserts a complete, well-formed chunk record (length + type + data, CRC recomputed here so
    // callers never have to hand-compute it) immediately before the given byte offset.
    void insertChunk(const std::size_t beforeOffset, const std::string& type,
                     const std::vector<unsigned char>& data) {
        std::vector<unsigned char> record;
        record.reserve(12 + data.size());
        appendU32(record, static_cast<std::uint32_t>(data.size()));
        for (const char character : type) {
            record.push_back(static_cast<unsigned char>(character));
        }
        record.insert(record.end(), data.begin(), data.end());
        auto crc = crc32Seed();
        for (const char character : type) {
            crc = crc32Step(crc, static_cast<unsigned char>(character));
        }
        for (const auto byteValue : data) {
            crc = crc32Step(crc, byteValue);
        }
        crc ^= 0xFFFFFFFFU;
        appendU32(record, crc);
        bytes_.insert(bytes_.begin() + static_cast<std::ptrdiff_t>(beforeOffset), record.begin(),
                      record.end());
    }

    // Offset immediately after the IEND chunk's CRC (i.e. the true end of a well-formed file);
    // used both to append trailing bytes and as the natural "insert before this" point for a new
    // chunk placed right after the last existing one.
    [[nodiscard]] std::size_t endOffset() const noexcept { return bytes_.size(); }

    void save() const {
        std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
        if (!stream) {
            std::abort();
        }
        stream.write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
    }

  private:
    [[nodiscard]] std::uint32_t readU32(const std::size_t offset) const {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_.at(offset))) << 24U) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_.at(offset + 1)))
                << 16U) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_.at(offset + 2)))
                << 8U) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_.at(offset + 3)));
    }

    static void appendU32(std::vector<unsigned char>& out, const std::uint32_t value) {
        out.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
        out.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
        out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<unsigned char>(value & 0xFFU));
    }

    // A tiny standalone CRC-32 (ISO 3309 / ITU-T V.42, the exact PNG/zlib polynomial, pre-inverted
    // seed with a final XOR the two callers above apply) so this test support header can compute a
    // valid CRC for a patched/inserted adversarial chunk without depending on zlib itself.
    [[nodiscard]] static std::uint32_t crc32Seed() noexcept { return 0xFFFFFFFFU; }
    [[nodiscard]] static std::uint32_t crc32Step(std::uint32_t crc,
                                                 const unsigned char byteValue) noexcept {
        crc ^= byteValue;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U)));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
        return crc;
    }

    std::filesystem::path path_;
    std::vector<char> bytes_;
};

// The result of a from-scratch, independent read of a staged PNG: chunk order, IHDR/sRGB field
// values, whether every CRC-32 matched, whether every scanline's filter byte was 0, and the
// decoded RGBA8 sample bytes -- filter bytes stripped, in increasing row Y then X order. `ok` is
// false for any structural defect (bad signature/CRC/zlib stream/size mismatch); check it before
// trusting any other field.
struct IndependentPngDecode final {
    bool ok = false;
    std::vector<std::string> chunkTypesInOrder;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t colorType = 0;
    std::uint8_t compressionMethod = 0;
    std::uint8_t filterMethod = 0;
    std::uint8_t interlaceMethod = 0;
    std::uint8_t srgbIntent = 0;
    bool everyCrcValid = false;
    bool everyRowFilterZero = false;
    std::vector<std::uint8_t> rgba;
};

// Reads `path` as a plain PNG byte stream and inflates its concatenated IDAT payload via zlib
// directly -- no dependency on bloom::output::PngRgba8SrgbWriterV1 or
// bloom::output::PngRgba8SrgbReopenVerifierV1. Used by the round-trip test's "independent decode
// cross-check" (proving the encoder against a second reading) and by the chunk-conformance test.
[[nodiscard]] inline IndependentPngDecode
independentlyDecodePng(const std::filesystem::path& path) {
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
        const std::uint32_t length = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                                     (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
                                     (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
                                     static_cast<std::uint32_t>(bytes[offset + 3]);
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
        const std::uint32_t storedCrc = (static_cast<std::uint32_t>(bytes[crcOffset]) << 24U) |
                                        (static_cast<std::uint32_t>(bytes[crcOffset + 1]) << 16U) |
                                        (static_cast<std::uint32_t>(bytes[crcOffset + 2]) << 8U) |
                                        static_cast<std::uint32_t>(bytes[crcOffset + 3]);
        if (static_cast<std::uint32_t>(crc) != storedCrc) {
            result.everyCrcValid = false;
        }
        result.chunkTypesInOrder.push_back(type);
        if (type == "IHDR") {
            if (length != 13) {
                return result;
            }
            result.width = (static_cast<std::uint32_t>(data[0]) << 24U) |
                           (static_cast<std::uint32_t>(data[1]) << 16U) |
                           (static_cast<std::uint32_t>(data[2]) << 8U) |
                           static_cast<std::uint32_t>(data[3]);
            result.height = (static_cast<std::uint32_t>(data[4]) << 24U) |
                            (static_cast<std::uint32_t>(data[5]) << 16U) |
                            (static_cast<std::uint32_t>(data[6]) << 8U) |
                            static_cast<std::uint32_t>(data[7]);
            result.bitDepth = data[8];
            result.colorType = data[9];
            result.compressionMethod = data[10];
            result.filterMethod = data[11];
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
    z_stream stream2{};
    if (inflateInit(&stream2) != Z_OK) {
        return result;
    }
    stream2.next_in = idatConcat.data();
    stream2.avail_in = static_cast<uInt>(idatConcat.size());
    stream2.next_out = inflated.data();
    stream2.avail_out = static_cast<uInt>(inflated.size());
    const auto ret = inflate(&stream2, Z_FINISH);
    inflateEnd(&stream2);
    if (ret != Z_STREAM_END || stream2.avail_out != 0) {
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
        std::memcpy(result.rgba.data() + static_cast<std::size_t>(row) * rowRgbaBytes,
                    inflated.data() + rowOffset + 1, rowRgbaBytes);
    }
    result.ok = true;
    return result;
}

} // namespace bloom_output_png_test_support
