#include "flat_exr_test_support.hpp"
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>
#include <OpenColorIO/OpenColorIO.h>
#include <algorithm>
#include <array>
#include <bit>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/media/provider/openh264_runtime.hpp>
#include <bloom/media/video/session.hpp>
#include <bloom/output/flat_exr_export_write.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_analysis_digest.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <type_traits>

namespace {
using namespace bloom;
namespace support = bloom_output_flat_exr_test_support;
namespace OCIO = OCIO_NAMESPACE;
static_assert(!std::is_default_constructible_v<output::PreparedFlatExrOutputV1>);
static_assert(!std::is_default_constructible_v<output::PreparedOutputDisplayV1>);
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename T> T checked(media::provider::Result<T> value) {
    if (const auto* error = std::get_if<media::provider::Unavailable>(&value))
        throw std::runtime_error(error->detail);
    return std::get<T>(std::move(value));
}
[[maybe_unused]] void checked(const std::optional<media::provider::Unavailable>& error) {
    if (error)
        throw std::runtime_error(error->detail);
}
std::shared_ptr<const runtime::CompiledCompositionPlan>
planWithLook(const std::filesystem::path& lutPath) {
    using namespace runtime;
    using namespace document;
    const auto op = [](std::size_t index) { return OperationIndex::fromRaw(index); };
    AssetRecord asset;
    asset.id = AssetId::fromRaw(77);
    asset.kind = AssetKind::Lut;
    asset.name = "Double gain";
    asset.locator = {"file", "project-relative", "gain.cube", "file://" + lutPath.string()};
    asset.contentDigest = color::readLutFile(lutPath).digest;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(1),
                      {ParameterId::fromRaw(1), core::Color4d{0.08, 0.18, 0.3, 1}},
                      {ParameterId::fromRaw(2), 64.0},
                      {ParameterId::fromRaw(3), 64.0}});
    operations.emplace_back(
        CompiledImageEffect{NodeId::fromRaw(2), op(0),
                            FileTransformKernel{asset.id, 1, 0, "ACEScg", asset}, false, true});
    operations.emplace_back(CompiledLayerOutput{NodeId::fromRaw(3),
                                                LayerId::fromRaw(1),
                                                op(1),
                                                {ParameterId::fromRaw(10), Vec2d{32, 32}},
                                                {ParameterId::fromRaw(11), Vec2d{}},
                                                {ParameterId::fromRaw(12), Vec2d{1, 1}},
                                                {ParameterId::fromRaw(13), 0.0},
                                                {ParameterId::fromRaw(14), 1.0},
                                                ParameterId::fromRaw(15)});
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(4), {{LayerSlotId::fromRaw(1), LayerId::fromRaw(1), op(2)}}});
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(5), op(3)});
    const auto format = CompositionFormat::create(64, 64);
    if (!format)
        throw std::runtime_error("fixture format");
    return std::make_shared<const CompiledCompositionPlan>(CompiledCompositionPlanDefinition{
        Revision::fromRaw(1), ProjectId::fromRaw(1), CompositionId::fromRaw(1), *format,
        std::move(operations), op(4)});
}
std::shared_ptr<const output::OutputAnalysisAttemptV1>
attemptFor(const std::shared_ptr<const runtime::ProcessFrame>& frame,
           output::ExportResourceLedgerV1& ledger) {
    const auto identity = output::ProcessFrameSemanticIdentityV1Preparer{}.prepare(frame, {});
    check(identity.identity() != nullptr, "process identity prepared");
    const auto report = output::analyzeFlatExrRgba32fLinRec709SceneV1(
        {.process = {.readyIdentity = identity.identity(), .missingDescriptor = {}}});
    check(report.hasReport(), "source EXR analysis");
    auto attempt = output::buildOutputAnalysisAttemptV1(
        {.frame = frame,
         .processIdentity = identity.identity(),
         .report = report.report(),
         .target = {.targetKey = core::ArtifactTargetKey::fromRaw(1), .targetPath = "shot.exr"},
         .display = {}},
        ledger);
    check(attempt.hasAttempt(), "source EXR attempt");
    return attempt.attempt();
}
std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void testConfigBinding(const runtime::EvaluationColorIntent& intent) {
    auto original = support::roundTripFixture();
    original.identity.colorIntent = intent;
    const auto source = support::prepareSource(std::move(original));
    auto changed = support::roundTripFixture();
    changed.identity.colorIntent = intent;
    changed.identity.colorIntent.ocioConfigRevision = core::Sha256Digest::fromBytes({1});
    const auto substituted = support::prepareSource(std::move(changed));
    const auto report = output::analyzeTiffRgba16SrgbV1(
        {.process = {.readyIdentity = source.processIdentity, .missingDescriptor = {}}});
    check(report.hasReport() && report.report()->display(),
          "TIFF retains the source display processor");
    output::ExportResourceLedgerV1 ledger;
    auto inputs = output::OutputAnalysisAttemptBuildInputsV1{
        .frame = substituted.frame,
        .processIdentity = substituted.processIdentity,
        .report = report.report(),
        .target = {.targetKey = core::ArtifactTargetKey::fromRaw(1), .targetPath = "shot.tiff"},
        .display = {}};
    check(output::buildOutputAnalysisAttemptV1(inputs, ledger).error() ==
              output::OutputAnalysisAttemptErrorCodeV1::InvalidReport,
          "TIFF rejects substitution of the source config revision");
    inputs.processIdentity = source.processIdentity;
    check(output::buildOutputAnalysisAttemptV1(inputs, ledger).error() ==
              output::OutputAnalysisAttemptErrorCodeV1::InvalidIdentity,
          "attempt rejects substitution of its identity's source frame");
}
void exrOracle(const output::OutputAnalysisAttemptV1& source,
               output::ExportResourceLedgerV1& ledger, const std::filesystem::path& directory,
               const OCIO::ConstConfigRcPtr& config) {
    const auto prepared = output::prepareFlatExrAttemptV1(
        source,
        {.outputColorSpaceId = "ACES2065-1", .compression = output::FlatExrCompressionV1::Piz},
        ledger);
    check(prepared.hasAttempt(), "handoff analysis");
    const auto& attempt = *prepared.attempt();
    const auto& conversion = attempt.report()->exr();
    const std::array alphaInput{support::pixel(0.04F, 0.09F, 0.15F, 0.5F),
                                support::pixel(0, 0, 0, 0)};
    std::array<std::array<float, 4>, 2> alphaOutput{};
    check(conversion->apply(alphaInput, alphaOutput), "EXR alpha transform");
    std::array<float, 3> straight{0.08F, 0.18F, 0.3F};
    config->getProcessor("ACEScg", "ACES2065-1")
        ->getDefaultCPUProcessor()
        ->applyRGB(straight.data());
    for (std::size_t c = 0; c < 3; ++c)
        check(std::abs(alphaOutput[0][c] - straight[c] * 0.5F) < 1e-5F && alphaOutput[1][c] == 0,
              "unpremultiply-transform-premultiply and zero alpha oracle");
    check(alphaOutput[0][3] == 0.5F && alphaOutput[1][3] == 0, "output transform preserves alpha");
    auto wrongConfig = source.frame()->identity().colorIntent;
    wrongConfig.ocioConfigUri = "bloom://missing-config";
    check(!output::PreparedFlatExrOutputV1::prepare(wrongConfig,
                                                    {.outputColorSpaceId = "ACES2065-1"}),
          "unknown config URI cannot borrow a matching revision");
    const auto path = directory / "shot.1001.exr";
    const auto written = output::FlatExrExportWriterV1{}.run(attempt, path, {});
    check(written.status() == output::FlatExrExportWriteStatusV1::Written,
          "AP0 handoff export verified");
    Imf::InputFile file(path.string().c_str());
    check(file.header().compression() == Imf::PIZ_COMPRESSION, "PIZ header");
    const auto* id = file.header().findTypedAttribute<Imf::StringAttribute>("colorInteropID");
    check(id && id->value() == "ACES2065-1", "output colour id header");
    const auto& c = Imf::chromaticities(file.header());
    const std::array<float, 8> chroma{c.red.x,  c.red.y,  c.green.x, c.green.y,
                                      c.blue.x, c.blue.y, c.white.x, c.white.y};
    constexpr std::array<std::uint32_t, 8> expected{0x3f3c154dU, 0x3e87d567U, 0,
                                                    0x3f800000U, 0x38d1b717U, 0xbd9db22dU,
                                                    0x3ea4b33eU, 0x3eace315U};
    for (std::size_t i = 0; i < 8; ++i)
        check(std::bit_cast<std::uint32_t>(chroma[i]) == expected[i], "AP0 chromaticities oracle");
    std::vector<std::array<float, 4>> pixels(std::size_t{64} * 64U);
    Imf::FrameBuffer buffer;
    constexpr std::array names{"R", "G", "B", "A"};
    for (std::size_t channel = 0; channel < 4; ++channel)
        buffer.insert(names[channel], Imf::Slice::Make(Imf::FLOAT,
                                                       reinterpret_cast<char*>(pixels.data()) +
                                                           channel * sizeof(float),
                                                       file.header().dataWindow(),
                                                       sizeof(pixels[0]), 64U * sizeof(pixels[0])));
    file.setFrameBuffer(buffer);
    file.readPixels(0, 63);
    std::array<float, 3> oracle{0.08F, 0.18F, 0.3F};
    config->getProcessor("ACEScg", "ACES2065-1")->getDefaultCPUProcessor()->applyRGB(oracle.data());
    float delta = 0;
    for (const auto& pixel : pixels)
        for (std::size_t channel = 0; channel < 3; ++channel)
            delta = std::max(delta, std::abs(pixel[channel] - oracle[channel]));
    check(delta < 1e-5F, "handoff ignores the look and matches independent AP0 oracle");
    check(output::outputLookDescriptionV1(source.frame()->identity()) == "Look: OFF (handoff)",
          "handoff policy evidence");
    const auto record =
        output::computeOutputAnalysisDigestV1(*attempt.processIdentity(), attempt.report()->view());
    check(record.hasValue() && record.preimageByteCount() == 1669 &&
              attempt.processIdentity()->canonicalBytes().size() == 271,
          "handoff identity record sizes pinned");
    std::cout << "AP0 maximum delta=" << delta
              << " process record=" << attempt.processIdentity()->canonicalBytes().size()
              << " handoff analysis record=" << record.preimageByteCount() << '\n';
    // Explicit default options must preserve the original writer bytes, including compression.
    const auto defaultAttempt = output::prepareFlatExrAttemptV1(source, {}, ledger);
    check(defaultAttempt.hasAttempt(), "default options analysis");
    check(output::FlatExrExportWriterV1{}.run(source, directory / "legacy.exr", {}).status() ==
              output::FlatExrExportWriteStatusV1::Written,
          "legacy EXR");
    check(output::FlatExrExportWriterV1{}
                  .run(*defaultAttempt.attempt(), directory / "default.exr", {})
                  .status() == output::FlatExrExportWriteStatusV1::Written,
          "default EXR");
    check(bytes(directory / "legacy.exr") == bytes(directory / "default.exr") &&
              source.digest() == defaultAttempt.attempt()->digest(),
          "default EXR bytes and identity unchanged");
}
unsigned sample16(const media::provider::Bytes& bytes, std::size_t offset) {
    return std::to_integer<unsigned>(bytes[offset]) |
           (std::to_integer<unsigned>(bytes[offset + 1]) << 8U);
}
[[maybe_unused]] void reviewOracle(const runtime::ProcessFrame& frame,
                                   const std::filesystem::path& directory,
                                   const OCIO::ConstConfigRcPtr& config) {
    const auto display = output::PreparedOutputDisplayV1::prepare(
        frame.identity().colorIntent, "Rec.1886 Rec.709 - Display", "ACES 1.0 - SDR Video");
    check(display != nullptr, "review display pair prepared");
    auto prepared =
        checked(output::prepareMediaRgba16V1(frame.processImage(), {0, 1}, {}, display.get()));
    auto transform = OCIO::DisplayViewTransform::Create();
    transform->setSrc("ACEScg");
    transform->setDisplay("Rec.1886 Rec.709 - Display");
    transform->setView("ACES 1.0 - SDR Video");
    std::array<float, 3> oracle{0.16F, 0.36F, 0.6F};
    config->getProcessor(transform)->getDefaultCPUProcessor()->applyRGB(oracle.data());
    std::array<unsigned, 3> quantized{};
    for (std::size_t c = 0; c < 3; ++c) {
        quantized[c] = static_cast<unsigned>(
            std::floor(static_cast<double>(std::clamp(oracle[c], 0.0F, 1.0F)) * 65535.0 + 0.5));
        check(std::abs(static_cast<int>(sample16(prepared.planes[0].bytes, c * 2)) -
                       static_cast<int>(quantized[c])) <= 1,
              "review bakes 2x LUT and matches display oracle before encoding");
    }
    check(output::outputLookDescriptionV1(frame.identity()) ==
              "look: baked (1 look-tagged effects)",
          "review look evidence");
#ifdef BLOOM_COLOR5_OPENH264_RUNTIME
    // The Cisco OpenH264 binary is a consented runtime fetch, not a locked build input, so a
    // checkout or CI runner without it still runs the deterministic EXR oracle above but cannot
    // exercise the H.264 review encode.
    const auto openh264Runtime =
        OpenH264Runtime(std::filesystem::path(BLOOM_COLOR5_OPENH264_RUNTIME)).verify();
    if (openh264Runtime.installed) {
        using namespace media::provider;
        EncodeSessionOptionsV1 worker;
        worker.openh264Directory = BLOOM_COLOR5_OPENH264_RUNTIME;
        worker.openh264Version = OpenH264Runtime::version();
        worker.openh264Digest = OpenH264Runtime::libraryDigest();
        EncodeSettingsV1 settings;
        settings.container = "mov";
        settings.videoCodec = "h264";
        settings.profile = "high";
        settings.width = 64;
        settings.height = 64;
        settings.frames = 2;
        EncodeSessionV1 encoder(worker);
        checked(encoder.begin(settings));
        checked(encoder.video(prepared));
        prepared.pts = {1, 24};
        checked(encoder.video(std::move(prepared)));
        const auto qc = checked(encoder.finish());
        const auto path = directory / "review.mov";
        std::ofstream file(path, std::ios::binary);
        for (std::uint64_t offset = 0; offset < qc.bytes;) {
            const auto chunk = checked(encoder.read(offset));
            file.write(reinterpret_cast<const char*>(chunk.bytes.data()),
                       static_cast<std::streamsize>(chunk.bytes.size()));
            offset += chunk.bytes.size();
        }
        file.close();
        check(static_cast<bool>(file), "review movie saved");
        checked(encoder.close());
        media::video::VideoDecodeSession decoder(path);
        const auto probe = checked(decoder.probe());
        const auto stream = std::ranges::find_if(
            probe.streams, [](const auto& s) { return s.kind == MediaKind::Video; });
        check(stream != probe.streams.end(), "review video stream");
        const auto decoded = checked(decoder.frame(probe, stream->id, 0, 0, nullptr));
        check(decoded->format == PixelFormat::Yuv420p8 && decoded->colour.primaries == 1 &&
                  decoded->colour.transfer == 1 && decoded->colour.matrix == 1 &&
                  decoded->colour.range == 1,
              "worker decoded limited-range Rec.709 YUV420");
        unsigned maximum = 0;
        std::uint64_t total = 0;
        // Independently reconstruct display RGB from the worker's planar Rec.709 samples.
        for (std::size_t y = 0; y < 64; ++y)
            for (std::size_t x = 0; x < 64; ++x) {
                const auto channel = [&](std::size_t plane, std::size_t px, std::size_t py) {
                    return static_cast<double>(std::to_integer<unsigned>(
                        decoded->planes[plane].bytes[py * decoded->planes[plane].stride + px]));
                };
                const auto luma = (channel(0, x, y) - 16.0) / 219.0;
                const auto cb = (channel(1, x / 2, y / 2) - 128.0) / 224.0;
                const auto cr = (channel(2, x / 2, y / 2) - 128.0) / 224.0;
                const std::array rgb{luma + 1.5748 * cr,
                                     luma - 0.1873242729 * cb - 0.4681242729 * cr,
                                     luma + 1.8556 * cb};
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto value =
                        static_cast<int>(std::floor(std::clamp(rgb[c], 0.0, 1.0) * 65535.0 + 0.5));
                    const auto delta =
                        static_cast<unsigned>(std::abs(value - static_cast<int>(quantized[c])));
                    maximum = std::max(maximum, delta);
                    total += delta;
                }
            }
        const auto mean = (total + std::size_t{64} * 64U * 3U - 1) / (std::size_t{64} * 64U * 3U);
        check(maximum <= 22938 && mean <= 1967,
              "decoded review frame matches OCIO oracle within frozen tolerance");
        const auto analysis = checked(
            output::analyzeMediaOutputV1(output::OutputPresetV1::H264MovV1, settings, display, 1));
        check(analysis.implementationNote.find("look: baked (1 look-tagged effects)") !=
                      std::string::npos &&
                  analysis.implementationNote.find("Rec.1886 Rec.709 - Display") !=
                      std::string::npos,
              "review evidence binds actual display and look");
        std::cout << "review first frame OCIO error max=" << maximum << " mean=" << mean << '\n';
    }
#else
    (void)directory;
#endif
}
} // namespace
int main() {
    try {
        support::ScratchDirectory scratch("color5-deliverables");
        const auto path = scratch.file("gain.cube");
        {
            std::ofstream lut(path);
            lut << "LUT_3D_SIZE 2\n";
            for (int b = 0; b < 2; ++b)
                for (int g = 0; g < 2; ++g)
                    for (int r = 0; r < 2; ++r)
                        lut << r * 2 << ' ' << g * 2 << ' ' << b * 2 << '\n';
        }
        const auto plan = planWithLook(path);
        const auto revision = color::ocioBuiltInContentRevision(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri);
        if (!revision)
            throw std::runtime_error("ACES config revision");
        runtime::EvaluationRequest request{
            {}, plan->output(), runtime::CompositionFormatResolution{}};
        request.colorIntent = {"ACEScg", *revision, color::kAcesCgV1ConfigUri};
        request.pixelStorageByteLimit = std::size_t{1024} * 1024U;
        request.bypassLookNodes = true;
        runtime::CpuCompositionEvaluator evaluator;
        const auto handoff = evaluator.evaluate(plan, request, {});
        request.bypassLookNodes = false;
        const auto review = evaluator.evaluate(plan, request, {});
        check(handoff.frame() && handoff.diagnostics().empty(),
              "handoff evaluates with external look bypassed");
#ifdef BLOOM_COLOR5_OPENH264_RUNTIME
        check(review.frame() && review.diagnostics().empty(),
              "review look evaluates without diagnostics");
        check(std::abs(review.frame()->processImage().pixels()[0].red() -
                       2.0F * handoff.frame()->processImage().pixels()[0].red()) < 1e-5F,
              "look policy changes source pixels by 2x");
#else
        check(!review.diagnostics().empty(),
              "unsupported external LUT helper reports its fallback");
#endif
        output::ExportResourceLedgerV1 ledger;
        const auto attempt = attemptFor(handoff.frame(), ledger);
        const auto config = OCIO::Config::CreateFromBuiltinConfig(
            std::string(color::kAcesCgV1BuiltinConfigName).c_str());
        testConfigBinding(request.colorIntent);
        exrOracle(*attempt, ledger, path.parent_path(), config);
#ifdef BLOOM_COLOR5_OPENH264_RUNTIME
        reviewOracle(*review.frame(), path.parent_path(), config);
#endif
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
