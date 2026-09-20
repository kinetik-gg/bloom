// Native acceptance for the production export builder injection: the real
// GpuProcessFrameEvaluator drives the PRODUCTION CpuGpuSceneBuilder with the resolved shared
// GpuSceneOcioContext and the configured GpuSceneMediaContext on a mixed media + CST + text graph,
// then performs ONE combined final readback that transfers the unchanged process payload plus the
// encoded DisplayRgba8 output (two payloads). The identity arm (no output command) transfers exactly
// one process payload.
//
// The OCIO context is qualified through the REAL GpuOcioContextResolver from this target's own
// BLOOM_GPU_TOOLS_* packaging (never a manually built preparer or an ambient path), which is the
// exact seam the desktop/CLI/MCP roots use. The process image is compared against the genuine
// unchanged CpuCompositionEvaluator frame at the strict 2e-6 absolute-or-relative gate with exact
// alpha. Requires the Vulkan backend and the pinned tools; skips cleanly otherwise.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_scene_executor_test_support.hpp"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace document = bloom::document;

using bloom::core::BlendMode;
using bloom::core::Color4d;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledImageSource;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledText;
using bloom::runtime::CompiledTextLayout;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CstKernel;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioContextRequest;
using bloom::runtime::GpuOcioContextResolver;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::GpuProcessFrameEvaluator;
using bloom::runtime::GpuProcessFrameEvaluatorOptions;
using bloom::runtime::GpuProcessFrameStatus;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::OperationIndex;

using namespace bloom::runtime::executor_test;
using namespace bloom::runtime::media_executor_test;

using namespace std::chrono_literals;

constexpr std::string_view kAcesWorking = "ACEScg";
constexpr std::string_view kAcesAlternate = "ACES2065-1";
constexpr std::string_view kAcesTextureInput = "sRGB - Texture";

class Checks final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAILED: " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] std::optional<bloom::core::Sha256Digest> parseDigest(const std::string_view text) {
    constexpr std::string_view prefix = "sha256:";
    if (text.size() != prefix.size() + bloom::core::kSha256HexCharacters ||
        text.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    return bloom::core::Sha256Digest::fromLowercaseHex(text.substr(prefix.size()));
}

[[nodiscard]] bloom::color::GpuShaderToolPackage packagedToolsDescriptor() {
    bloom::color::GpuShaderToolPackage package;
    package.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    package.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    package.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    package.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
#if defined(BLOOM_GPU_TOOLS_RELOCATED)
    package.relocated = BLOOM_GPU_TOOLS_RELOCATED != 0;
#endif
#if defined(BLOOM_GPU_TOOLS_BUNDLE_RELATIVE)
    package.bundleRelative = true;
#endif
#if defined(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256)
    package.glslangStagedDigest = parseDigest(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256);
#endif
#if defined(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256)
    package.spirvValStagedDigest = parseDigest(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256);
#endif
    return package;
}

[[nodiscard]] inline Imf::Chromaticities acesAp0Chromaticities() {
    return Imf::Chromaticities(Imath::V2f(0.7347F, 0.2653F), Imath::V2f(0.0F, 1.0F),
                               Imath::V2f(0.0001F, -0.0770F), Imath::V2f(0.32168F, 0.33767F));
}

struct ExrPixel final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

void writeExrRgbaWithChromaticities(const std::filesystem::path& path, const int width,
                                    const int height, const std::vector<ExrPixel>& pixels,
                                    const Imf::Chromaticities& chromaticities) {
    if (static_cast<int>(pixels.size()) != width * height) {
        throw std::logic_error("EXR fixture pixel count does not match its dimensions");
    }
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(width - 1, height - 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(chromaticities));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    std::vector<float> red(pixels.size()), green(pixels.size()), blue(pixels.size()),
        alpha(pixels.size());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        red[index] = pixels[index].red;
        green[index] = pixels[index].green;
        blue[index] = pixels[index].blue;
        alpha[index] = pixels[index].alpha;
    }
    Imf::FrameBuffer frameBuffer;
    const auto stride = static_cast<std::size_t>(width) * sizeof(float);
    frameBuffer.insert(
        "R", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(red.data()), sizeof(float), stride));
    frameBuffer.insert(
        "G", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(green.data()), sizeof(float), stride));
    frameBuffer.insert(
        "B", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(blue.data()), sizeof(float), stride));
    frameBuffer.insert(
        "A", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(alpha.data()), sizeof(float), stride));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
}

[[nodiscard]] inline std::vector<ExrPixel> signedHdrPixels() {
    return {ExrPixel{1.5F, -0.25F, 0.5F, 1.0F},  ExrPixel{0.25F, 0.5F, 4.0F, 1.0F},
            ExrPixel{2.0F, 0.125F, 0.75F, 0.5F}, ExrPixel{0.5F, 2.5F, -0.5F, 1.0F},
            ExrPixel{0.0F, 0.0F, 0.0F, 0.0F},    ExrPixel{3.0F, 1.0F, 0.25F, 0.25F}};
}

[[nodiscard]] inline document::AssetRecord
imageAsset(const std::filesystem::path& path, const std::string& name, const std::uint64_t rawId) {
    const auto probe = bloom::media::probeImage(path);
    if (!probe.value.has_value()) {
        throw std::logic_error("EXR fixture could not be probed: " + probe.diagnostic);
    }
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(rawId);
    asset.kind = document::AssetKind::Image;
    asset.locator.kind = "file";
    asset.locator.portability = "project-relative";
    asset.locator.path = path.filename().string();
    asset.locator.relinkHint = "file:" + path.string();
    asset.contentDigest = probe.value->contentDigest;
    asset.interpretation.colorSpace = document::AssetColorSpace::Auto;
    asset.width = probe.value->width;
    asset.height = probe.value->height;
    asset.name = name;
    return asset;
}

struct Fixture final {
    std::filesystem::path directory;
    document::AssetRecord asset;
};

[[nodiscard]] Fixture makeAcesFixture() {
    Fixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_process_frame_ocio_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    const auto path = fixture.directory / "aces_ap0.exr";
    writeExrRgbaWithChromaticities(path, 3, 2, signedHdrPixels(), acesAp0Chromaticities());
    fixture.asset = imageAsset(path, "aces_ap0", 900);
    fixture.asset.interpretation.inputColorSpaceId = std::string{kAcesTextureInput};
    return fixture;
}

// solid -> CST round trip -> layer A, text -> layer B (Multiply), image source -> layer C, all
// merged bottom-to-top, then composition output. Mirrors the production working-space fixture.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
mixedPlan(const CompositionFormat compositionFormat, const document::AssetRecord& asset,
          const Color4d solidColor, const std::uint64_t idBase) {
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6),  ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8),  ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    const LayerIds idsC{ParameterId::fromRaw(idBase + 12), ParameterId::fromRaw(idBase + 13),
                        ParameterId::fromRaw(idBase + 14), ParameterId::fromRaw(idBase + 15),
                        ParameterId::fromRaw(idBase + 16), ParameterId::fromRaw(idBase + 17)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 20),
                                          {ParameterId::fromRaw(idBase + 21), solidColor},
                                          {ParameterId::fromRaw(idBase + 22), 8.0},
                                          {ParameterId::fromRaw(idBase + 23), 8.0}});
    operations.emplace_back(CompiledImageEffect{
        NodeId::fromRaw(idBase + 24), OperationIndex::fromRaw(0),
        CstKernel{std::string{kAcesWorking}, std::string{kAcesAlternate}}, false, false});
    operations.emplace_back(CompiledImageEffect{
        NodeId::fromRaw(idBase + 25), OperationIndex::fromRaw(1),
        CstKernel{std::string{kAcesAlternate}, std::string{kAcesWorking}}, false, false});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 26), LayerId::fromRaw(idBase + 30),
                                        OperationIndex::fromRaw(2), idsA,
                                        LayerValues{.position = {3.0, 2.0}, .opacity = 0.9}));
    operations.emplace_back(
        CompiledText{NodeId::fromRaw(idBase + 40),
                     ParameterId::fromRaw(idBase + 41),
                     "ACES",
                     {ParameterId::fromRaw(idBase + 42), 9.0},
                     {ParameterId::fromRaw(idBase + 43), Color4d{0.9, -0.1, 0.4, 1.0}},
                     CompiledTextLayout{ParameterId::fromRaw(idBase + 44),
                                        0,
                                        {ParameterId::fromRaw(idBase + 45), 1.0},
                                        {ParameterId::fromRaw(idBase + 46), 0.0}}});
    operations.emplace_back(layerOutput(
        NodeId::fromRaw(idBase + 47), LayerId::fromRaw(idBase + 31), OperationIndex::fromRaw(4),
        idsB,
        LayerValues{.position = {5.0, 4.0}, .opacity = 0.8, .blendMode = BlendMode::Multiply}));
    operations.emplace_back(
        CompiledImageSource{NodeId::fromRaw(idBase + 50), asset, 0, 0, 0, std::string{}, false});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 51), LayerId::fromRaw(idBase + 32),
                                        OperationIndex::fromRaw(6), idsC,
                                        LayerValues{.position = {6.0, 5.0}, .opacity = 1.0}));
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 60),
        std::vector<CompiledMergeInput>{
            CompiledMergeInput{LayerSlotId::fromRaw(idBase + 61), LayerId::fromRaw(idBase + 30),
                               OperationIndex::fromRaw(3)},
            CompiledMergeInput{LayerSlotId::fromRaw(idBase + 62), LayerId::fromRaw(idBase + 31),
                               OperationIndex::fromRaw(5)},
            CompiledMergeInput{LayerSlotId::fromRaw(idBase + 63), LayerId::fromRaw(idBase + 32),
                               OperationIndex::fromRaw(7)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 70), OperationIndex::fromRaw(8)});
    return publish(CompiledCompositionPlanDefinition{
        document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(9)});
}

// A 6000x4000-friendly plan: a real EXR image-source layer and a translucent solid layer merged
// bottom-to-top, then composition output. No OCIO effect/text, so the CPU reference gate stays fast
// while still exercising real media decode at the full composition size.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
mediaSolidPlan(const CompositionFormat compositionFormat, const document::AssetRecord& asset,
               const Color4d solidColor, const std::uint64_t idBase) {
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6),  ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8),  ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledImageSource{NodeId::fromRaw(idBase + 10), asset, 0, 0, 0, std::string{}, false});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 11), LayerId::fromRaw(idBase + 20),
                                        OperationIndex::fromRaw(0), idsA,
                                        LayerValues{.position = {0.0, 0.0}, .opacity = 1.0}));
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 12),
                                          {ParameterId::fromRaw(idBase + 13), solidColor},
                                          {ParameterId::fromRaw(idBase + 14), 32.0},
                                          {ParameterId::fromRaw(idBase + 15), 32.0}});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 16), LayerId::fromRaw(idBase + 21),
                                        OperationIndex::fromRaw(2), idsB,
                                        LayerValues{.position = {1.0, 1.0}, .opacity = 0.5}));
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 30),
        std::vector<CompiledMergeInput>{
            CompiledMergeInput{LayerSlotId::fromRaw(idBase + 31), LayerId::fromRaw(idBase + 20),
                               OperationIndex::fromRaw(1)},
            CompiledMergeInput{LayerSlotId::fromRaw(idBase + 32), LayerId::fromRaw(idBase + 21),
                               OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 40), OperationIndex::fromRaw(4)});
    return publish(CompiledCompositionPlanDefinition{
        document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(5)});
}

[[nodiscard]] EvaluationRequest acesRequest(const CompiledCompositionPlan& plan) {
    auto request = requestFor(plan);
    request.colorIntent = EvaluationColorIntent{
        .workingColorSpaceId = kAcesWorking,
        .ocioConfigRevision = {},
        .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri,
    };
    return request;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options.valid) {
        std::cerr << "invalid arguments\n";
        return 2;
    }
#if !defined(BLOOM_GPU_TOOLS_AVAILABLE) || !BLOOM_GPU_TOOLS_AVAILABLE
    std::cout << "SKIP: packaged GPU shader tools unavailable\n";
    return 77;
#else
    Checks expectations;
    const auto fixture = makeAcesFixture();
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        std::cout << "SKIP: the ACES built-in is unavailable\n";
        return 77;
    }

    // Qualify the packaged tools through the production resolver exactly as the roots do.
    GpuOcioContextResolver resolver{GpuOcioContextRequest{
        std::filesystem::path{BLOOM_GPU_OCIO_CONTEXT_TEST_EXECUTABLE}, packagedToolsDescriptor(),
        {}, {}}};
    const auto resolved = resolver.resolve();
    expectations.expect(resolved.hasValue(), "the packaged tools resolve to a shared context");
    if (!resolved.hasValue()) {
        std::cerr << "resolver diagnostic: " << resolved.diagnostic << '\n';
        return 1;
    }
    const auto ocioContext = resolved.context;

    CpuCompositionEvaluator cpuEvaluator;
    cpuEvaluator.setAssetBaseDirectory(fixture.directory);

    GpuProcessFrameEvaluatorOptions evaluatorOptions;
    evaluatorOptions.enabled = true;
    evaluatorOptions.loaderPath = options.loader_path;
    // Deliberately leave requestByteBudget/readbackByteBudget at their DEFAULTS: the production
    // host-availability-derived budget, not an injected test limit. This is the "default factory"
    // budget path the desktop/CLI/MCP roots take.
    evaluatorOptions.ocioContext = ocioContext;
    evaluatorOptions.mediaContext = GpuSceneMediaContext::fromEvaluator(cpuEvaluator);
    expectations.expect(
        evaluatorOptions.requestByteBudget ==
                bloom::runtime::defaultGpuProcessFrameByteBudget() &&
            evaluatorOptions.readbackByteBudget ==
                bloom::runtime::defaultGpuProcessFrameByteBudget() &&
            evaluatorOptions.requestByteBudget > 0,
        "the default factory budget is the host-availability-derived budget");
    // Pure policy: a conservative quarter of available, no fixed floor or ceiling; unknown
    // availability is a small typed fallback.
    expectations.expect(
        bloom::runtime::gpuProcessFrameByteBudgetForAvailable(
            std::size_t{128} * 1024U * 1024U) == std::size_t{32} * 1024U * 1024U &&
            bloom::runtime::gpuProcessFrameByteBudgetForAvailable(
                std::size_t{16} * 1024U * 1024U * 1024U) ==
                std::size_t{4} * 1024U * 1024U * 1024U &&
            bloom::runtime::gpuProcessFrameByteBudgetForAvailable(
                std::size_t{128} * 1024U * 1024U * 1024U) ==
                std::size_t{32} * 1024U * 1024U * 1024U &&
            bloom::runtime::gpuProcessFrameByteBudgetForAvailable(std::nullopt) ==
                std::size_t{512} * 1024U * 1024U,
        "the pure budget policy scales with availability with no fixed floor or ceiling");
    auto evaluator = GpuProcessFrameEvaluator::create(evaluatorOptions);
    expectations.expect(evaluator != nullptr, "the evaluator is constructed");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        if (options.require_device) {
            std::cerr << "FAIL: required device unavailable: "
                      << (evaluator != nullptr ? evaluator->availabilityDiagnostic().message
                                               : std::string{"no evaluator"})
                      << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available\n";
        return 77;
    }

    const auto plan = mixedPlan(format(8, 8), fixture.asset, Color4d{-0.2, 1.6, 0.35, 0.5}, 5000);
    const auto request = acesRequest(*plan);

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto oracle = cpuEvaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(oracle.frame() != nullptr, "the CPU oracle evaluates the mixed scene");
    if (oracle.frame() == nullptr) {
        evaluator->beginShutdown();
        return 1;
    }
    const auto* descriptor = oracle.frame()->processImage().descriptor();
    expectations.expect(descriptor != nullptr, "the CPU oracle carries a process descriptor");
    if (descriptor == nullptr) {
        evaluator->beginShutdown();
        return 1;
    }
    const auto geometry =
        GpuOcioCommandGeometry{descriptor->dataWindow().extent().width(),
                               descriptor->dataWindow().extent().height()};

    // Prepare the display command from the SAME shared preparer the evaluator uses.
    auto configResolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *revision, std::string{kAcesWorking});
    expectations.expect(configResolution.ready(), "the ACES display config resolves");
    if (!configResolution.ready()) {
        evaluator->beginShutdown();
        return 1;
    }
    auto config = std::move(configResolution).takeResolved();
    expectations.expect(config.has_value(), "the ACES display config is taken");
    if (!config.has_value()) {
        evaluator->beginShutdown();
        return 1;
    }
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = std::string(config->displayName());
    spec.view = std::string(config->viewName());
    const auto prepared =
        ocioContext->preparer->prepare(*config, spec, geometry, ocioContext->compileOptions);
    expectations.expect(prepared.hasValue(), "the shared preparer compiles the display command");
    if (!prepared.hasValue()) {
        evaluator->beginShutdown();
        return 1;
    }

    // Display arm: the ONE combined readback transfers the process payload plus the encoded output.
    const auto display = evaluator->evaluate(plan, request, {}, {}, prepared.command);
    if (display.status != GpuProcessFrameStatus::Evaluated) {
        std::cerr << "display-arm status=" << static_cast<int>(display.status)
                  << " code=" << static_cast<int>(display.diagnostic.code)
                  << " message=" << display.diagnostic.message << '\n';
    }
    expectations.expect(display.status == GpuProcessFrameStatus::Evaluated &&
                            display.frame != nullptr,
                        "the mixed media+CST+text scene evaluates on the GPU");
    expectations.expect(display.encodedArm == bloom::runtime::GpuOutputColorArm::DisplayRgba8 &&
                            !display.encodedDisplayRgba8.empty(),
                        "the display arm transfers a straight RGBA8 payload");
    constexpr std::uint64_t kProcessBytes = 8ULL * 8ULL * sizeof(float) * 4U;
    constexpr std::uint64_t kEncodedBytes = 8ULL * 8ULL * sizeof(std::uint8_t) * 4U;
    expectations.expect(display.outputColorCounters.readbackSubmissions == 1 &&
                            display.outputColorCounters.transferredPayloads == 2 &&
                            display.outputColorCounters.processPayloadBytes == kProcessBytes &&
                            display.outputColorCounters.encodedPayloadBytes == kEncodedBytes,
                        "the display arm makes one submission carrying two exactly-accounted "
                        "payloads");
    expectations.expect(display.counters.nativeDispatches > 0 && display.counters.uploads > 0,
                        "the media upload and OCIO/CST work dispatch natively");
    if (display.frame != nullptr) {
        expectations.expect(
            pixelsClose(display.frame->processImage().pixels(),
                        oracle.frame()->processImage().pixels()),
            "the GPU process payload matches the CPU oracle at the strict 2e-6 gate");
    }

    // Strict display parity for the NON-DEFAULT ACES config: the GPU DisplayRgba8 payload must
    // match the unchanged CPU qualified display processor within one code with exact alpha.
    auto cpuDisplayBuild = bloom::color::buildBloomNeutralCpuDisplayProcessor(*config);
    auto cpuDisplayHandle = std::move(cpuDisplayBuild).takeHandle();
    expectations.expect(cpuDisplayHandle.has_value(),
                        "the ACES CPU display processor builds for the oracle");
    if (cpuDisplayHandle.has_value() &&
        display.encodedArm == bloom::runtime::GpuOutputColorArm::DisplayRgba8 &&
        display.frame != nullptr) {
        const bloom::runtime::CpuQualifiedDisplayPreparer displayPreparer(*cpuDisplayHandle);
        const auto cpuDisplay = displayPreparer.prepare(
            display.frame,
            {.aggregatePixelStorageByteLimit = std::size_t{1} << 30U,
             .chunkPixelCount = bloom::runtime::kDefaultQualifiedDisplayChunkPixelCount,
             .viewAdjust = {},
             .displayName = {},
             .viewName = {},
             .showLook = true},
            {});
        expectations.expect(
            cpuDisplay.status() ==
                    bloom::runtime::QualifiedDisplayPreparationStatus::Prepared &&
                cpuDisplay.frame() != nullptr,
            "the ACES CPU display frame prepares");
        if (cpuDisplay.frame() != nullptr) {
            const auto cpuPixels = cpuDisplay.frame()->buffer().pixels();
            expectations.expect(cpuPixels.size() == display.encodedDisplayRgba8.size(),
                                "the ACES display pixel counts match");
            if (cpuPixels.size() == display.encodedDisplayRgba8.size()) {
                bool withinOneCode = true;
                for (std::size_t index = 0; index < cpuPixels.size(); ++index) {
                    const auto& cpuPixel = cpuPixels[index];
                    const auto& gpuPixel = display.encodedDisplayRgba8[index];
                    const auto close = [](const std::uint8_t left, const std::uint8_t right) {
                        return left == right ||
                               (left > right ? left - right : right - left) <= 1;
                    };
                    if (!close(cpuPixel.red, gpuPixel.red) ||
                        !close(cpuPixel.green, gpuPixel.green) ||
                        !close(cpuPixel.blue, gpuPixel.blue) ||
                        cpuPixel.alpha != gpuPixel.alpha) {
                        withinOneCode = false;
                        break;
                    }
                }
                expectations.expect(withinOneCode,
                                    "the ACES GPU display payload matches the CPU display within "
                                    "one code with exact alpha");
            }
        }
    }

    // Identity arm: exactly one process payload, no colour work.
    const auto identity = evaluator->evaluate(plan, request);
    expectations.expect(identity.status == GpuProcessFrameStatus::Evaluated &&
                            identity.frame != nullptr,
                        "the identity arm still evaluates the mixed scene");
    expectations.expect(identity.encodedArm == bloom::runtime::GpuOutputColorArm::None &&
                            identity.encodedDisplayRgba8.empty() &&
                            identity.outputColorCounters.readbackSubmissions == 1 &&
                            identity.outputColorCounters.transferredPayloads == 1 &&
                            identity.outputColorCounters.processPayloadBytes == kProcessBytes &&
                            identity.outputColorCounters.encodedPayloadBytes == 0,
                        "the identity arm transfers exactly one exactly-accounted process payload");

    // The large-composition gate with the DEFAULT export budget and a real media source: a
    // 6000x4000 composition must evaluate (not be refused by a fixed 1 GiB gate) whenever the host
    // genuinely has the memory. The render layer's per-operation image cap is fixed and smaller than
    // one 6000x4000 RGBA32F image, so this exercises the honest CPU reference path with the same
    // host-derived budget the production roots inject.
    {
        constexpr std::uint32_t kLargeWidth = 6000;
        constexpr std::uint32_t kLargeHeight = 4000;
        const auto largePlan = mediaSolidPlan(format(kLargeWidth, kLargeHeight), fixture.asset,
                                              Color4d{0.3, 0.4, 0.5, 0.5}, 7000);
        auto largeRequest = acesRequest(*largePlan);
        largeRequest.pixelStorageByteLimit = bloom::runtime::defaultGpuProcessFrameByteBudget();
        const auto largeFrame = cpuEvaluator.evaluate(largePlan, largeRequest, {});
        expectations.expect(largeFrame.status() == bloom::runtime::EvaluationStatus::Evaluated &&
                                largeFrame.frame() != nullptr,
                            "a 6000x4000 real-source export evaluates under the default "
                            "host-derived budget");
        if (largeFrame.frame() != nullptr &&
            largeFrame.frame()->processImage().descriptor() != nullptr) {
            const auto extent =
                largeFrame.frame()->processImage().descriptor()->dataWindow().extent();
            expectations.expect(extent.width() == kLargeWidth && extent.height() == kLargeHeight,
                                "the 6000x4000 process image keeps its exact geometry");
        }
    }

    evaluator->beginShutdown();
    if (expectations.failures() != 0) {
        std::cerr << "process-frame OCIO expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: mixed media+CST+text GPU export with resolved OCIO context\n";
    return 0;
#endif
}
