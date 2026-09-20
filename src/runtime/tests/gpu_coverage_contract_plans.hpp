#pragma once

// Private plan builders for the bounded GPU coverage contract fixtures. Every plan here is a real
// CompiledCompositionPlan that the production CpuGpuSceneBuilder accepts; the driver keeps only the
// fixture catalogue and the gate loop.

#include "gpu_coverage_fixture_support.hpp"

#include <bloom/core/blend_mode.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::gpu_coverage_plans {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::CompositionFormat;
using bloom::document::CompositionId;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::document::ShapeKind;
using bloom::document::Vec2d;
using bloom::gpu_coverage_fixtures::format;
using bloom::gpu_coverage_fixtures::LayerIds;
using bloom::gpu_coverage_fixtures::layerOutput;
using bloom::gpu_coverage_fixtures::LayerValues;
using bloom::gpu_coverage_fixtures::publish;
using bloom::gpu_coverage_fixtures::twoLayerPlan;
using bloom::runtime::CompiledColorParameter;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledCompositionSource;
using bloom::runtime::CompiledCompositionTimeMapping;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledShape;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledText;
using bloom::runtime::CompiledTextLayout;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::ImageEffectKernel;
using bloom::runtime::OperationIndex;

[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan> basePlan() {
    return twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                        LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 90000);
}

[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
withSource(const CompiledOperation& source, const std::uint64_t idBase,
           const std::shared_ptr<const CompiledCompositionPlan>* nested = nullptr,
           const double layerOpacity = 1.0) {
    auto definition = basePlan()->copyDefinition();
    definition.operations[0] = source;
    if (layerOpacity != 1.0) {
        auto& layer = std::get<CompiledLayerOutput>(definition.operations[1]);
        layer.opacity = CompiledScalarParameter{layer.opacity.id, layerOpacity};
    }
    if (nested != nullptr) {
        definition.nestedPlans.push_back(*nested);
    }
    (void)idBase;
    return publish(std::move(definition));
}

// A strict, kind-valid shape: a Line has no fill so it must carry a nonzero stroke, and a Path must
// carry real anchors. Non-Line kinds may additionally request a stroke so the native CPU oracle
// parity exercises the vector stroke/opacity arm rather than fill alone.
[[nodiscard]] inline CompiledShape shape(const ShapeKind kind, const std::uint64_t idBase,
                                         const bool withStroke = false) {
    CompiledShape value{};
    value.sourceNodeId = NodeId::fromRaw(idBase);
    value.kind = kind;
    value.size = CompiledVec2Parameter{ParameterId::fromRaw(idBase + 1), Vec2d{6.0, 5.0}};
    value.fillEnabled = true;
    value.fillColor =
        CompiledColorParameter{ParameterId::fromRaw(idBase + 2), Color4d{0.8, 0.4, 0.2, 1.0}};
    value.strokeEnabled = withStroke;
    value.strokeColor =
        CompiledColorParameter{ParameterId::fromRaw(idBase + 3), Color4d{0.1, 0.6, 0.9, 1.0}};
    value.strokeWidth =
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 4), withStroke ? 1.5 : 0.0};
    if (kind == ShapeKind::Line) {
        value.fillEnabled = false;
        value.strokeEnabled = true;
        value.strokeWidth = CompiledScalarParameter{ParameterId::fromRaw(idBase + 4), 1.5};
        value.lineStart = Vec2d{0.0, 0.0};
        value.lineEnd = Vec2d{6.0, 5.0};
    } else if (kind == ShapeKind::Path) {
        value.path.closed = true;
        value.path.anchors = {
            bloom::document::PathAnchor{Vec2d{0.0, 0.0}, std::nullopt, std::nullopt},
            bloom::document::PathAnchor{Vec2d{6.0, 0.0}, std::nullopt, std::nullopt},
            bloom::document::PathAnchor{Vec2d{3.0, 5.0}, std::nullopt, std::nullopt}};
    }
    return value;
}

[[nodiscard]] inline CompiledText text(const std::uint64_t idBase) {
    return CompiledText{NodeId::fromRaw(idBase),
                        ParameterId::fromRaw(idBase + 1),
                        "gpu",
                        {ParameterId::fromRaw(idBase + 2), 12.0},
                        {ParameterId::fromRaw(idBase + 3), Color4d{1.0, 1.0, 1.0, 1.0}},
                        CompiledTextLayout{ParameterId::fromRaw(idBase + 4),
                                           0,
                                           {ParameterId::fromRaw(idBase + 5), 1.0},
                                           {ParameterId::fromRaw(idBase + 6), 0.0}}};
}

[[nodiscard]] inline CompiledImageEffect effect(const ImageEffectKernel& kernel,
                                                const std::uint64_t idBase) {
    return CompiledImageEffect{NodeId::fromRaw(idBase), OperationIndex::fromRaw(0), kernel, false,
                               false};
}

[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
effectPlan(const ImageEffectKernel& kernel, const std::uint64_t idBase) {
    auto definition = basePlan()->copyDefinition();
    auto solid = definition.operations[0];
    auto layer = std::get<CompiledLayerOutput>(definition.operations[1]);
    layer.input = OperationIndex::fromRaw(1);
    auto merge = std::get<CompiledMerge>(definition.operations[4]);
    merge.entries = {CompiledMergeInput{LayerSlotId::fromRaw(idBase), layer.layerId,
                                        OperationIndex::fromRaw(2)}};
    definition.operations.clear();
    definition.operations.push_back(std::move(solid));
    definition.operations.push_back(effect(kernel, idBase + 10));
    definition.operations.push_back(layer);
    definition.operations.push_back(merge);
    definition.operations.push_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 30), OperationIndex::fromRaw(3)});
    definition.output = OperationIndex::fromRaw(4);
    return publish(std::move(definition));
}

[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
modifiedLayer(const std::function<void(CompiledLayerOutput&)>& edit) {
    auto definition = basePlan()->copyDefinition();
    edit(std::get<CompiledLayerOutput>(definition.operations[1]));
    return publish(std::move(definition));
}

// A fully reachable layer fed by another layer (the generic graph-input axis): every operation is
// reachable from the output, so the chain rasterizes through the composed matrix.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
layerOnLayerPlan(const std::uint64_t idBase) {
    auto definition = basePlan()->copyDefinition();
    auto solid = definition.operations[0];
    auto layerA = std::get<CompiledLayerOutput>(definition.operations[1]);
    auto layerB = std::get<CompiledLayerOutput>(definition.operations[3]);
    layerB.input = OperationIndex::fromRaw(1);
    CompiledMerge merge{NodeId::fromRaw(idBase + 1),
                        {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 2), layerB.layerId,
                                            OperationIndex::fromRaw(2)}}};
    definition.operations.clear();
    definition.operations.push_back(std::move(solid));
    definition.operations.push_back(layerA);
    definition.operations.push_back(layerB);
    definition.operations.push_back(std::move(merge));
    definition.operations.push_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 3), OperationIndex::fromRaw(3)});
    definition.output = OperationIndex::fromRaw(4);
    return publish(std::move(definition));
}

// A valid parented layer: layer B names layer A as its parent, so its composed matrix inherits the
// parent's shear. A parent must be an earlier Layer Output.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
parentedPlan(const std::uint64_t idBase) {
    auto definition = twoLayerPlan(format(24, 16),
                                   LayerValues{.position = {12.0, 8.0},
                                               .anchor = {1.5, -0.5},
                                               .scale = {1.5, -0.75},
                                               .rotation = 30.0},
                                   LayerValues{.position = {7.0, 9.0},
                                               .anchor = {0.25, 0.5},
                                               .scale = {2.0, 0.5},
                                               .rotation = -15.0},
                                   9.0, 7.0, idBase)
                          ->copyDefinition();
    std::get<CompiledLayerOutput>(definition.operations[3]).parent = OperationIndex::fromRaw(1);
    return publish(std::move(definition));
}

enum class AffineAxis : std::uint8_t { Scale, Rotation, Anchor };

// A raster generic input: two affine vector layers merged, then a layer over the merge result. The
// layer over a merge is raster, so the builder must emit GpuAffine rather than a coverage command.
// Each axis exercises one transform component on that raster layer.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
rasterAffinePlan(const AffineAxis axis, const std::uint64_t idBase) {
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6),  ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8),  ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    const LayerIds idsC{ParameterId::fromRaw(idBase + 12), ParameterId::fromRaw(idBase + 13),
                        ParameterId::fromRaw(idBase + 14), ParameterId::fromRaw(idBase + 15),
                        ParameterId::fromRaw(idBase + 16), ParameterId::fromRaw(idBase + 17)};
    const auto layerAId = LayerId::fromRaw(idBase + 20);
    const auto layerBId = LayerId::fromRaw(idBase + 21);
    const auto layerCId = LayerId::fromRaw(idBase + 22);
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 1),
                      {ParameterId::fromRaw(idBase + 40), Color4d{0.5, 0.25, 0.125, 1.0}},
                      {ParameterId::fromRaw(idBase + 41), 6.0},
                      {ParameterId::fromRaw(idBase + 42), 5.0}});
    operations.emplace_back(
        layerOutput(NodeId::fromRaw(idBase + 2), layerAId, OperationIndex::fromRaw(0), idsA,
                    LayerValues{.position = {4.3, 3.1}, .scale = {1.25, 1.25}, .rotation = 10.0}));
    // Signed/HDR with a tiny alpha so the affine resample covers that domain too.
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 3),
                      {ParameterId::fromRaw(idBase + 43), Color4d{2.5, -0.3, 0.1, 1.0e-5}},
                      {ParameterId::fromRaw(idBase + 44), 6.0},
                      {ParameterId::fromRaw(idBase + 45), 5.0}});
    operations.emplace_back(layerOutput(
        NodeId::fromRaw(idBase + 4), layerBId, OperationIndex::fromRaw(2), idsB,
        LayerValues{
            .position = {11.5, 8.2}, .scale = {0.5, 1.5}, .rotation = -12.0, .opacity = 0.75}));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 5),
                      {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 30), layerAId,
                                          OperationIndex::fromRaw(1)},
                       CompiledMergeInput{LayerSlotId::fromRaw(idBase + 31), layerBId,
                                          OperationIndex::fromRaw(3)}}});
    LayerValues top{.position = {8.0, 6.0}};
    switch (axis) {
    case AffineAxis::Scale:
        top.scale = {1.5, 0.5};
        break;
    case AffineAxis::Rotation:
        top.rotation = 25.0;
        break;
    case AffineAxis::Anchor:
        top.anchor = {1.0, -0.5};
        break;
    }
    operations.emplace_back(
        layerOutput(NodeId::fromRaw(idBase + 6), layerCId, OperationIndex::fromRaw(4), idsC, top));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 7),
                      {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 32), layerCId,
                                          OperationIndex::fromRaw(5)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 8), OperationIndex::fromRaw(6)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), bloom::document::ProjectId::fromRaw(1),
        CompositionId::fromRaw(2), format(16, 12), std::move(operations),
        OperationIndex::fromRaw(7)});
}

// A child composition: two independently-coloured solid branches merged bottom-to-top. The distinct
// composition id and nonzero duration are what make a nested reference valid rather than a cycle.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
nestedChildPlan(const std::uint64_t idBase, const std::uint64_t compositionRaw,
                const Color4d colorB) {
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6),  ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8),  ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    const auto layerAId = LayerId::fromRaw(idBase + 30);
    const auto layerBId = LayerId::fromRaw(idBase + 31);
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{NodeId::fromRaw(idBase + 10),
                      {ParameterId::fromRaw(idBase + 11), Color4d{0.5, 0.25, 0.125, 1.0}},
                      {ParameterId::fromRaw(idBase + 12), 8.0},
                      {ParameterId::fromRaw(idBase + 13), 6.0}});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 20), layerAId,
                                        OperationIndex::fromRaw(0), idsA,
                                        LayerValues{.position = {4.0, 3.0}}));
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 40),
                                          {ParameterId::fromRaw(idBase + 41), colorB},
                                          {ParameterId::fromRaw(idBase + 42), 6.0},
                                          {ParameterId::fromRaw(idBase + 43), 5.0}});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 50), layerBId,
                                        OperationIndex::fromRaw(2), idsB,
                                        LayerValues{.position = {6.0, 5.0}, .opacity = 0.75}));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 60),
                      {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 61), layerAId,
                                          OperationIndex::fromRaw(1)},
                       CompiledMergeInput{LayerSlotId::fromRaw(idBase + 62), layerBId,
                                          OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 70), OperationIndex::fromRaw(4)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(7),
                                                 bloom::document::ProjectId::fromRaw(1),
                                                 CompositionId::fromRaw(compositionRaw),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(5)};
    definition.duration = RationalTime::fromInteger(100);
    return publish(std::move(definition));
}

// A parent that transforms the nested child through a Layer Output before its merge. The nested
// source is a raster leaf, so it takes the translation/affine raster path exactly like media.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
nestedParentPlan(const std::shared_ptr<const CompiledCompositionPlan>& child,
                 const std::uint64_t idBase, const std::uint64_t compositionRaw,
                 const LayerValues layerValues = LayerValues{.position = {6.0, 5.0}}) {
    const LayerIds ids{ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11),
                       ParameterId::fromRaw(idBase + 12), ParameterId::fromRaw(idBase + 13),
                       ParameterId::fromRaw(idBase + 14), ParameterId::fromRaw(idBase + 15)};
    const auto layerId = LayerId::fromRaw(idBase + 20);
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledCompositionSource{
        NodeId::fromRaw(idBase), 0,
        CompiledCompositionTimeMapping{
            {ParameterId::fromRaw(idBase + 1), 0.0}, {ParameterId::fromRaw(idBase + 2), 1.0}, 0}});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 21), layerId,
                                        OperationIndex::fromRaw(0), ids, layerValues));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 22),
                      {CompiledMergeInput{LayerSlotId::fromRaw(idBase + 23), layerId,
                                          OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 24), OperationIndex::fromRaw(2)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(7),
                                                 bloom::document::ProjectId::fromRaw(1),
                                                 CompositionId::fromRaw(compositionRaw),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(3)};
    definition.duration = RationalTime::fromInteger(100);
    definition.nestedPlans.push_back(child);
    return publish(std::move(definition));
}

struct NestedBranchReusePlans final {
    std::shared_ptr<const CompiledCompositionPlan> planA;
    std::shared_ptr<const CompiledCompositionPlan> planB;
};

// Two parents whose nested children differ in ONE branch (the second solid's colour). The first
// child branch keeps the same content-addressed key, so the executor must reuse it across the
// single-branch edit.
[[nodiscard]] inline NestedBranchReusePlans nestedBranchReusePlans() {
    const auto childA = nestedChildPlan(1000, 201, Color4d{0.125, 0.375, 0.75, 0.5});
    const auto childB = nestedChildPlan(2000, 202, Color4d{0.9, 0.1, 0.05, 1.0});
    return NestedBranchReusePlans{nestedParentPlan(childA, 9000, 100),
                                  nestedParentPlan(childB, 9100, 103)};
}

} // namespace bloom::gpu_coverage_plans
