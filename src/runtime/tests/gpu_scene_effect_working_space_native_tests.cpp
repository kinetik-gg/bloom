// Real native acceptance for arbitrary resolved scene-linear working spaces (ACEScg) through the
// PRODUCTION CpuGpuSceneBuilder -> GpuSceneExecutor path.
//
// The graph mixes every working-space-relevant branch in one composition: an authored negative/HDR
// translucent Solid, a Text layer, a real AP0 (ACES2065-1 tagged) OpenEXR media source, a CST
// round-trip (ACEScg -> ACES2065-1 -> ACEScg), and a non-Normal Blend on one layer. The result is
// compared against the genuine, unchanged CpuCompositionEvaluator frame at the strict 2e-6
// absolute-or-relative RGB gate with exact alpha and the CPU process descriptor (data/display
// window, PAR).
//
// Also asserted: a changed working space keeps the decoded raw upload cached and changes the OCIO
// transform identity (colour identity invalidation), and a changed solid colour changes the solid
// command key without touching the media upload.
//
// Requires the Vulkan backend and the pinned tools; skips cleanly otherwise.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_scene_preparation_test_support.hpp"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace document = bloom::document;

using bloom::core::BlendMode;
using bloom::core::Color4d;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
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
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::CstKernel;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::GpuSceneSolidCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::OperationIndex;

using bloom::runtime::media_executor_test::descriptorMatchesScene;
using bloom::runtime::media_executor_test::kCacheBudget;
using bloom::runtime::media_executor_test::kReadbackBudget;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::runScene;

constexpr std::string_view kAcesWorking = "ACEScg";
constexpr std::string_view kAcesAlternate = "ACES2065-1";
constexpr std::string_view kAcesTextureInput = "sRGB - Texture";

// ACES AP0 primaries with the ACES white point, exactly what the EXR reader recognizes as the
// ACES2065-1 interpretation.
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

inline void writeExrRgbaWithChromaticities(const std::filesystem::path& path, const int width,
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

// A 3x2 signed/HDR RGBA EXR: an odd source smaller than the composition, negative channels, values
// above 1, and a fully transparent pixel, written premultiplied.
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
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_working_space_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    const auto path = fixture.directory / "aces_ap0.exr";
    writeExrRgbaWithChromaticities(path, 3, 2, signedHdrPixels(), acesAp0Chromaticities());
    fixture.asset = imageAsset(path, "aces_ap0", 900);
    // An explicit texture input keeps the input->working transform non-identity for every working
    // space, so a changed working space genuinely changes the OCIO effect identity.
    fixture.asset.interpretation.inputColorSpaceId = std::string{kAcesTextureInput};
    return fixture;
}

// solid -> CST round trip -> layer A, text -> layer B (Multiply), image source -> layer C, all
// merged bottom-to-top, then composition output.
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

[[nodiscard]] EvaluationRequest acesRequest(const CompiledCompositionPlan& plan,
                                            const std::string_view working) {
    auto request = requestFor(plan);
    request.colorIntent = EvaluationColorIntent{
        .workingColorSpaceId = working,
        .ocioConfigRevision = {},
        .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri,
    };
    return request;
}

[[nodiscard]] const GpuSceneUploadCommand*
firstUpload(const bloom::runtime::PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
            return upload;
        }
    }
    return nullptr;
}

[[nodiscard]] std::size_t countSolidCommands(const bloom::runtime::PreparedGpuScene& scene) {
    std::size_t count = 0;
    for (const auto& command : scene.commands()) {
        count += std::holds_alternative<GpuSceneSolidCommand>(command) ? 1U : 0U;
    }
    return count;
}

// The first CST effect command, to prove a changed working space changes colour identity.
[[nodiscard]] const GpuSceneOcioEffectCommand*
firstEffect(const bloom::runtime::PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
            if (ocio->program != nullptr &&
                ocio->program->program().semanticsId == "bloom.color.ocio-process-cst.v1") {
                return ocio;
            }
        }
    }
    return nullptr;
}

// The media leaf's input->working OCIO command, identified by its raw-upload input key.
[[nodiscard]] const GpuSceneOcioEffectCommand*
mediaEffect(const bloom::runtime::PreparedGpuScene& scene) {
    const auto* upload = firstUpload(scene);
    if (upload == nullptr) {
        return nullptr;
    }
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
            if (ocio->inputKey == upload->semanticKey) {
                return ocio;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] std::string solidCommandKey(const bloom::runtime::PreparedGpuScene& scene) {
    const auto index = scene.commandForOperation()[0];
    if (index == bloom::runtime::kInvalidGpuSceneCommand) {
        return {};
    }
    return std::visit(
        [](const auto& command) -> std::string {
            using T = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<T, GpuSceneSolidCommand>) {
                return command.semanticKey;
            }
            return {};
        },
        scene.commands()[index]);
}

void testMixedAces(Expectations& expectations, GpuDevice& device,
                   const CpuCompositionEvaluator& evaluator, const Fixture& fixture,
                   const GpuSceneOcioContext& ocioContext) {
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    auto executor = GpuSceneExecutor::create(device, *cache.cache, GpuSceneExecutorBudgets{});
    expectations.expect(cache.hasValue() && executor.hasValue(),
                        "mixed ACES: cache and executor host");
    if (!cache || !executor) {
        return;
    }
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan = mixedPlan(format(8, 8), fixture.asset, Color4d{-0.2, 1.6, 0.35, 0.5}, 5000);
    const auto request = acesRequest(*plan, kAcesWorking);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "mixed ACES: the mixed scene prepares");
    if (!prepared) {
        std::cerr << "mixed ACES prepare diagnostic: " << prepared.diagnostic.message << '\n';
        return;
    }
    expectations.expect(firstUpload(*prepared.scene) != nullptr,
                        "mixed ACES: the media branch emits a raw upload");
    expectations.expect(countSolidCommands(*prepared.scene) >= 1,
                        "mixed ACES: the solid branch emits a solid command under ACES working");
    expectations.expect(firstEffect(*prepared.scene) != nullptr,
                        "mixed ACES: the CST branch emits a real OCIO CST command");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "mixed ACES: the CPU oracle evaluates");
    if (frame.frame() == nullptr) {
        return;
    }
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        "mixed ACES: process identity matches the CPU frame");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        "mixed ACES: output descriptor matches the CPU frame");

    const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
    if (!run.ready) {
        std::cerr << "mixed ACES executor diagnostic: " << executor.executor->diagnostic().message
                  << '\n';
    }
    expectations.expect(run.ready, "mixed ACES: the executor reaches Ready");
    if (!run.ready || run.image == nullptr) {
        return;
    }
    expectations.expect(descriptorMatchesScene(*run.image, *prepared.scene),
                        "mixed ACES: the native descriptor matches the scene");
    expectations.expect(executor.executor->counters().ocioEffectDispatches >= 2,
                        "mixed ACES: the media transform and the CST both dispatched on the GPU");
    const auto readback = bloom::render::readbackResidentImage(*run.image, kReadbackBudget);
    expectations.expect(readback.hasValue(), "mixed ACES: the output reads back for the oracle");
    if (!readback) {
        return;
    }
    const auto& cpuImage = frame.frame()->processImage();
    expectations.expect(readback.pixels.size() == cpuImage.pixels().size(),
                        "mixed ACES: the pixel count matches the CPU frame");
    if (readback.pixels.size() != cpuImage.pixels().size()) {
        return;
    }
    expectations.expect(pixelsClose(readback.pixels, cpuImage.pixels()),
                        "mixed ACES: every pixel is within the strict 2e-6 process gate");

    // Colour identity invalidation: a changed working space keeps the decoded raw upload cached
    // while the media input->working OCIO transform identity changes. (An explicit CST has a fixed
    // from/to and is intentionally invariant across working spaces.)
    const auto alternate = builder.build(plan, acesRequest(*plan, kAcesAlternate));
    expectations.expect(alternate.hasValue(), "mixed ACES alternate: the changed space prepares");
    if (alternate) {
        expectations.expect(alternate.scene->mediaStatistics().uploadCacheHits == 1,
                            "mixed ACES alternate: the decoded raw upload is reused");
        const auto* uploadFirst = firstUpload(*prepared.scene);
        const auto* uploadSecond = firstUpload(*alternate.scene);
        expectations.expect(uploadFirst != nullptr && uploadSecond != nullptr &&
                                uploadFirst->semanticKey == uploadSecond->semanticKey,
                            "mixed ACES alternate: the upload key is working-space independent");
        const auto* mediaFirst = mediaEffect(*prepared.scene);
        const auto* mediaSecond = mediaEffect(*alternate.scene);
        expectations.expect(mediaFirst != nullptr && mediaSecond != nullptr &&
                                mediaFirst->semanticKey != mediaSecond->semanticKey,
                            "mixed ACES alternate: the input->working identity changes with the "
                            "working space");
    }

    // Pixel identity invalidation: a changed solid colour changes the solid command key without
    // changing the media upload identity.
    const auto recoloured =
        mixedPlan(format(8, 8), fixture.asset, Color4d{0.4, 0.2, -0.3, 0.75}, 6000);
    const auto oldKey = solidCommandKey(*prepared.scene);
    const auto recolouredPrepared =
        builder.build(recoloured, acesRequest(*recoloured, kAcesWorking));
    expectations.expect(recolouredPrepared.hasValue(),
                        "mixed ACES recolour: the recoloured scene prepares");
    if (recolouredPrepared) {
        expectations.expect(!oldKey.empty() && oldKey != solidCommandKey(*recolouredPrepared.scene),
                            "mixed ACES recolour: the solid command key changes");
        expectations.expect(firstUpload(*prepared.scene)->semanticKey ==
                                firstUpload(*recolouredPrepared.scene)->semanticKey,
                            "mixed ACES recolour: the media upload key is unchanged");
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options.valid) {
        std::cerr << "invalid arguments\n";
        return 2;
    }
    Expectations expectations;
    const auto fixture = makeAcesFixture();
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        std::cout << "SKIP: the ACES built-in is unavailable\n";
        return 77;
    }
    CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.directory);

#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return 77;
#else
    GpuOcioCompileOptions compileOptions;
    compileOptions.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    compileOptions.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    GpuSceneOcioContext ocioContext;
    ocioContext.preparer = std::make_shared<GpuOcioProgramPreparer>();
    ocioContext.compileOptions = compileOptions;

    GpuDeviceCreationOptions createOptions;
    createOptions.loader_path = options.loader_path;
    auto device = GpuDevice::create(createOptions);
    if (!device) {
        if (options.require_device) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return 77;
    }
    testMixedAces(expectations, *device.device, evaluator, fixture, ocioContext);
    if (!expectations.ok()) {
        std::cerr << "working-space expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: mixed ACES working-space scene vs CPU oracle\n";
    return 0;
#endif
}
