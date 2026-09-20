#pragma once

// Working-space colour-transform fixture for the bounded GPU coverage gate. It builds one genuine
// CompiledCompositionPlan that mixes every working-space-relevant branch in a single composition --
// an authored signed/HDR translucent Solid, an explicit non-identity CST round trip, a Text layer, a
// real signed/HDR EXR ImageSource whose input is explicitly "sRGB - Texture", and a non-Normal
// (Multiply) blend -- under the ACEScg working space of the pinned ACES built-in. The production
// CpuGpuSceneBuilder then emits real GPU OCIO ProcessEffect commands for the CST legs and the media
// input transform, so the native gate executes and compares genuine GPU pixels against the unchanged
// CPU evaluator at the documented 2e-6 gate.

#include "gpu_coverage_fixture_support.hpp"
#include "gpu_coverage_media_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bloom::gpu_coverage_working_space {

using bloom::core::BlendMode;
using bloom::core::Color4d;
using bloom::document::AssetRecord;
using bloom::document::CompositionFormat;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::gpu_coverage_fixtures::LayerIds;
using bloom::gpu_coverage_fixtures::layerOutput;
using bloom::gpu_coverage_fixtures::LayerValues;
using bloom::gpu_coverage_fixtures::publish;
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
using bloom::runtime::CstKernel;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::OperationIndex;

inline constexpr std::string_view kAcesWorkingSpace = "ACEScg";
inline constexpr std::string_view kAcesAlternateSpace = "ACES2065-1";
inline constexpr std::string_view kAcesTextureInput = "sRGB - Texture";

// The mixed composition: solid -> CST round trip -> layer A; text -> layer B (Multiply);
// image source -> layer C; all merged bottom-to-top, then composition output.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
mixedPlan(const CompositionFormat compositionFormat, const AssetRecord& asset,
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
    operations.emplace_back(
        CompiledImageEffect{NodeId::fromRaw(idBase + 24), OperationIndex::fromRaw(0),
                            CstKernel{std::string{kAcesWorkingSpace},
                                      std::string{kAcesAlternateSpace}},
                            false, false});
    operations.emplace_back(
        CompiledImageEffect{NodeId::fromRaw(idBase + 25), OperationIndex::fromRaw(1),
                            CstKernel{std::string{kAcesAlternateSpace},
                                      std::string{kAcesWorkingSpace}},
                            false, false});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 26), LayerId::fromRaw(idBase + 30),
                                        OperationIndex::fromRaw(2), idsA,
                                        LayerValues{.position = {3.0, 2.0}, .opacity = 0.5}));
    operations.emplace_back(
        CompiledText{NodeId::fromRaw(idBase + 40), ParameterId::fromRaw(idBase + 41), "ACES",
                     {ParameterId::fromRaw(idBase + 42), 9.0},
                     {ParameterId::fromRaw(idBase + 43), Color4d{0.9, -0.1, 0.4, 1.0}},
                     CompiledTextLayout{ParameterId::fromRaw(idBase + 44), 0,
                                        {ParameterId::fromRaw(idBase + 45), 1.0},
                                        {ParameterId::fromRaw(idBase + 46), 0.0}}});
    operations.emplace_back(layerOutput(
        NodeId::fromRaw(idBase + 47), LayerId::fromRaw(idBase + 31), OperationIndex::fromRaw(4),
        idsB,
        LayerValues{.position = {5.0, 4.0}, .opacity = 0.5, .blendMode = BlendMode::Multiply}));
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
        bloom::document::Revision::fromRaw(7), bloom::document::ProjectId::fromRaw(1),
        bloom::document::CompositionId::fromRaw(2), compositionFormat, std::move(operations),
        OperationIndex::fromRaw(9)});
}

// The ACEScg request identity: the exact pinned ACES built-in URI and a scene-linear working space.
// The default requestFor() is the Bloom Neutral space, so the working-space fixture must override it.
[[nodiscard]] inline EvaluationRequest acesRequest(const CompiledCompositionPlan& plan) {
    auto request = bloom::gpu_coverage_fixtures::requestFor(plan);
    request.colorIntent = EvaluationColorIntent{
        .workingColorSpaceId = kAcesWorkingSpace,
        .ocioConfigRevision = {},
        .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri,
    };
    return request;
}

// The real signed/HDR EXR asset with an explicit "sRGB - Texture" input so every working space
// produces a genuine non-identity input transform (the decoded bytes stay identical).
[[nodiscard]] inline AssetRecord textureInputAsset() {
    AssetRecord asset = bloom::gpu_coverage_media::syntheticImage().asset;
    asset.interpretation.inputColorSpaceId = std::string{kAcesTextureInput};
    return asset;
}

} // namespace bloom::gpu_coverage_working_space
