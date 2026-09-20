// CPU-side acceptance for the production image-effect scene preparation: identity/bypass/look
// aliasing, the missing-LUT CPU refusal passthrough, the fail-closed non-identity CST, the
// fail-closed real file transform, and (with the pinned tools) the genuine GPU OCIO command a
// non-identity CST emits, including the explicit materialization of a text leaf input. It needs no
// device and no loader; the native builder -> executor -> CPU oracle parity is a separate harness.

#include "gpu_scene_preparation_test_support.hpp"

#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/input_color_context.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using bloom::document::AssetId;
using bloom::document::AssetKind;
using bloom::document::AssetRecord;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledText;
using bloom::runtime::CstKernel;
using bloom::runtime::FileTransformKernel;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::IdentityImageKernel;
using bloom::runtime::ImageEffectKernel;

[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
effectPlan(const std::vector<CompiledOperation>& leading, const ImageEffectKernel& kernel,
           const std::uint64_t idBase) {
    const LayerIds ids{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                       ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                       ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations = leading;
    const auto effectIndex = static_cast<std::size_t>(leading.size());
    operations.emplace_back(CompiledImageEffect{NodeId::fromRaw(idBase + 10),
                                                OperationIndex::fromRaw(effectIndex - 1), kernel,
                                                false, false});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 11), LayerId::fromRaw(idBase + 12),
                                        OperationIndex::fromRaw(effectIndex), ids, LayerValues{}));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 13),
                      std::vector<CompiledMergeInput>{CompiledMergeInput{
                          LayerSlotId::fromRaw(idBase + 14), LayerId::fromRaw(idBase + 12),
                          OperationIndex::fromRaw(effectIndex + 1)}}});
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 15),
                                                      OperationIndex::fromRaw(effectIndex + 2)});
    return publish(bloom::runtime::CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, format(8, 6),
        std::move(operations), OperationIndex::fromRaw(effectIndex + 3)});
}

[[nodiscard]] CompiledOperation solidLeaf(const std::uint64_t idBase) {
    return bloom::runtime::CompiledSolid{
        NodeId::fromRaw(idBase),
        {ParameterId::fromRaw(idBase + 1), Color4d{0.5, 0.25, 0.125, 1.0}},
        {ParameterId::fromRaw(idBase + 2), 8.0},
        {ParameterId::fromRaw(idBase + 3), 6.0}};
}

[[nodiscard]] CompiledOperation textLeaf(const std::uint64_t idBase) {
    return CompiledText{
        NodeId::fromRaw(idBase),
        ParameterId::fromRaw(idBase + 1),
        "BLOOM",
        {ParameterId::fromRaw(idBase + 2), 10.0},
        {ParameterId::fromRaw(idBase + 3), Color4d{0.8, 0.4, 0.2, 1.0}},
        bloom::runtime::CompiledTextLayout{ParameterId::fromRaw(idBase + 4),
                                           0,
                                           {ParameterId::fromRaw(idBase + 5), 1.0},
                                           {ParameterId::fromRaw(idBase + 6), 0.0}}};
}

[[nodiscard]] const GpuSceneOcioEffectCommand*
findOcioCommand(const bloom::runtime::PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
            return ocio;
        }
    }
    return nullptr;
}

void testIdentityAndRefusals(Expectations& expectations) {
    // An identity CST aliases the input command exactly.
    const auto identity =
        effectPlan({solidLeaf(100)}, CstKernel{"lin_rec709_scene", "lin_rec709_scene"}, 1000);
    const auto identityPrepared = CpuGpuSceneBuilder{}.build(identity, requestFor(*identity));
    expectations.expect(identityPrepared.hasValue(), "an identity CST prepares");
    if (identityPrepared) {
        const auto& map = identityPrepared.scene->commandForOperation();
        expectations.expect(map[1] == map[0], "an identity CST aliases its input command");
        expectations.expect(findOcioCommand(*identityPrepared.scene) == nullptr,
                            "an identity CST emits no GPU OCIO command");
    }

    // IdentityImageKernel and bypass alias too.
    const auto identityKernel = effectPlan({solidLeaf(200)}, IdentityImageKernel{}, 2000);
    const auto kernelPrepared =
        CpuGpuSceneBuilder{}.build(identityKernel, requestFor(*identityKernel));
    expectations.expect(kernelPrepared.hasValue(), "an IdentityImageKernel prepares");
    if (kernelPrepared) {
        expectations.expect(kernelPrepared.scene->commandForOperation()[1] ==
                                kernelPrepared.scene->commandForOperation()[0],
                            "an IdentityImageKernel aliases its input command");
    }

    // A missing-LUT file transform is the CPU refusal-warning passthrough: exact identity.
    const auto missingLut = effectPlan(
        {solidLeaf(300)},
        FileTransformKernel{AssetId::fromRaw(0), 0, 0, "lin_rec709_scene", std::nullopt}, 3000);
    const auto missingPrepared = CpuGpuSceneBuilder{}.build(missingLut, requestFor(*missingLut));
    expectations.expect(missingPrepared.hasValue(), "a missing-LUT file transform prepares");
    if (missingPrepared) {
        expectations.expect(missingPrepared.scene->commandForOperation()[1] ==
                                missingPrepared.scene->commandForOperation()[0],
                            "a missing-LUT file transform aliases its input command");
    }
}

void testFailClosed(Expectations& expectations, const std::string& from, const std::string& to) {
    // A non-identity CST with no injected preparer must fail closed (never a false identity).
    const auto plan = effectPlan({solidLeaf(400)}, CstKernel{from, to}, 4000);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(!prepared.hasValue(), "a non-identity CST without a preparer fails closed");
    if (!prepared) {
        expectations.expect(prepared.diagnostic.code ==
                                PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "the fail-closed diagnostic is UnsupportedOperation");
    }

    // A valid LUT asset whose bytes are unreadable is the CPU refusal-warning passthrough: exact
    // identity, not a failure and not a false transform.
    FileTransformKernel file;
    file.lutAssetId = AssetId::fromRaw(42);
    AssetRecord asset;
    asset.id = AssetId::fromRaw(42);
    asset.kind = AssetKind::Lut;
    asset.locator.path = "missing.cube";
    asset.locator.relinkHint = "file://missing.cube";
    file.asset = asset;
    const auto filePlan = effectPlan({solidLeaf(500)}, file, 5000);
    const auto filePrepared = CpuGpuSceneBuilder{}.build(filePlan, requestFor(*filePlan));
    expectations.expect(filePrepared.hasValue(), "an unreadable LUT prepares as identity");
    if (filePrepared) {
        expectations.expect(filePrepared.scene->commandForOperation()[1] ==
                                filePrepared.scene->commandForOperation()[0],
                            "an unreadable LUT aliases its input command");
    }
}

[[nodiscard]] std::size_t countOcioCommands(const bloom::runtime::PreparedGpuScene& scene) {
    std::size_t count = 0;
    for (const auto& command : scene.commands()) {
        count += std::holds_alternative<GpuSceneOcioEffectCommand>(command) ? 1U : 0U;
    }
    return count;
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
[[nodiscard]] GpuSceneOcioContext ocioContext() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    GpuSceneOcioContext context;
    context.preparer = std::make_shared<GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

void testRealCst(Expectations& expectations, const std::string& from, const std::string& to) {
    const auto context = ocioContext();

    // A raster (solid) input emits a real ProcessEffect command whose input is the solid command.
    const auto rasterPlan = effectPlan({solidLeaf(600)}, CstKernel{from, to}, 6000);
    const CpuGpuSceneBuilder rasterBuilder(nullptr, {}, context);
    const auto rasterPrepared = rasterBuilder.build(rasterPlan, requestFor(*rasterPlan));
    expectations.expect(rasterPrepared.hasValue(), "a real non-identity CST prepares");
    if (rasterPrepared) {
        const auto* ocio = findOcioCommand(*rasterPrepared.scene);
        expectations.expect(ocio != nullptr, "a real CST emits a GPU OCIO command");
        if (ocio != nullptr) {
            expectations.expect(ocio->program != nullptr && ocio->program->encoding() ==
                                                                GpuOcioOutputEncoding::FinalRgba32f,
                                "the emitted command is a ProcessEffect RGBA32F program");
            expectations.expect(ocio->input == rasterPrepared.scene->commandForOperation()[0],
                                "the OCIO command consumes the solid command");
        }
    }

    // A vector leaf input (text) is materialized as a coverage command before the OCIO command.
    const auto textPlanValue = effectPlan({textLeaf(700)}, CstKernel{from, to}, 7000);
    const CpuGpuSceneBuilder textBuilder(nullptr, {}, context);
    const auto textPrepared = textBuilder.build(textPlanValue, requestFor(*textPlanValue));
    expectations.expect(textPrepared.hasValue(), "a non-identity CST over text prepares");
    if (textPrepared) {
        const auto* ocio = findOcioCommand(*textPrepared.scene);
        expectations.expect(ocio != nullptr, "a text-fed CST emits a GPU OCIO command");
        if (ocio != nullptr) {
            expectations.expect(
                std::holds_alternative<bloom::runtime::GpuSceneCoverageSolidCommand>(
                    textPrepared.scene->commands()[ocio->input]),
                "the text leaf was materialized as an accepted coverage GPU command");
        }
    }
}

#if defined(__linux__)
void testRealFileTransform(Expectations& expectations, const std::string& working,
                           const std::string& process) {
    const auto base = std::filesystem::temp_directory_path() / "bloom_gpu_effect_test";
    std::error_code directoryError;
    std::filesystem::create_directories(base, directoryError);
    expectations.expect(!directoryError, "the LUT fixture directory is creatable");
    if (directoryError) {
        return;
    }
    const auto path = base / "curve.cube";
    {
        std::ofstream file(path);
        file << "TITLE \"test\"\nLUT_1D_SIZE 4\n";
        file << "0.0 0.0 0.0\n0.2 0.3 0.4\n0.6 0.5 0.7\n1.0 1.0 1.0\n";
    }
    const auto resource = bloom::color::readLutFile(path);
    expectations.expect(resource.error == bloom::color::LutError::None,
                        "the LUT fixture reads for its digest");
    if (resource.error != bloom::color::LutError::None) {
        return;
    }
    const auto makeAsset = [&]() {
        AssetRecord asset;
        asset.id = AssetId::fromRaw(77);
        asset.kind = AssetKind::Lut;
        asset.locator.path = "curve.cube";
        asset.locator.relinkHint = "file://curve.cube";
        asset.contentDigest = resource.digest;
        return asset;
    };
    const auto context = ocioContext();
    bloom::runtime::GpuSceneMediaContext media;
    media.assetBaseDirectory = base;

    // The default process space is the working space: one real FileTransform command.
    FileTransformKernel sameSpace;
    sameSpace.lutAssetId = AssetId::fromRaw(77);
    sameSpace.asset = makeAsset();
    const auto samePlan = effectPlan({solidLeaf(800)}, sameSpace, 8000);
    const CpuGpuSceneBuilder sameBuilder(nullptr, media, context);
    const auto samePrepared = sameBuilder.build(samePlan, requestFor(*samePlan));
    expectations.expect(samePrepared.hasValue(), "a working-space LUT prepares");
    if (samePrepared) {
        expectations.expect(countOcioCommands(*samePrepared.scene) == 1,
                            "a working-space LUT emits exactly one OCIO command");
        const auto* ocio = findOcioCommand(*samePrepared.scene);
        expectations.expect(ocio != nullptr && ocio->program != nullptr &&
                                ocio->program->program().semanticsId ==
                                    bloom::color::kOcioGpuFileTransformSemanticsId,
                            "the emitted command is the FileTransform program");
    }

    // An explicit non-working process space emits the CPU's CST/LUT/CST chain.
    FileTransformKernel crossSpace;
    crossSpace.lutAssetId = AssetId::fromRaw(77);
    crossSpace.asset = makeAsset();
    crossSpace.processSpaceId = process;
    const auto crossPlan = effectPlan({solidLeaf(900)}, crossSpace, 9000);
    const CpuGpuSceneBuilder crossBuilder(nullptr, media, context);
    const auto crossPrepared = crossBuilder.build(crossPlan, requestFor(*crossPlan));
    expectations.expect(crossPrepared.hasValue(), "a cross-space LUT prepares");
    if (crossPrepared) {
        expectations.expect(countOcioCommands(*crossPrepared.scene) == 3,
                            "a cross-space LUT emits the working/process CST chain");
        const auto& map = crossPrepared.scene->commandForOperation();
        expectations.expect(map[1] != map[0],
                            "a cross-space LUT publishes a distinct chained command");
    }
    static_cast<void>(working);
}
#endif
#endif

} // namespace

int main() {
    Expectations expectations;
    testIdentityAndRefusals(expectations);

    const auto config = bloom::runtime::detail::resolveInputColorConfig(
        bloom::runtime::EvaluationColorIntent::LinearRec709Scene);
    if (!config.has_value()) {
        std::cerr << "SKIP: the Bloom Neutral OCIO config is unavailable\n";
        return 77;
    }
    const std::string from{config->processColorSpaceId()};
    const std::string to{config->sRgbTextureColorSpaceId()};
    testFailClosed(expectations, from, to);
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    testRealCst(expectations, from, to);
#if defined(__linux__)
    testRealFileTransform(expectations, from, to);
#endif
#else
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set (real arms skipped)\n";
#endif

    if (!expectations.ok()) {
        std::cerr << "image-effect scene preparation expectations failed\n";
        return 1;
    }
    std::cout << "PASS: image-effect GPU scene preparation\n";
    return 0;
}
