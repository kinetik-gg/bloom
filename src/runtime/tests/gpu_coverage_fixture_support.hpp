#pragma once

// Minimal, gate-owned fixture helpers for the bounded GPU coverage contract test. They are a
// deliberately small subset of the GPU scene preparation fixtures so the coverage gate does not
// pull in helpers it never calls (and therefore needs no warning suppression).

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bloom::gpu_coverage_fixtures {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::CompositionFormat;
using bloom::runtime::CompiledColorParameter;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::OperationIndex;


[[nodiscard]] inline std::string codeName(
    const bloom::runtime::PreparedGpuSceneDiagnosticCode code) {
    using bloom::runtime::PreparedGpuSceneDiagnosticCode;
    switch (code) {
    case PreparedGpuSceneDiagnosticCode::None:
        return "None";
    case PreparedGpuSceneDiagnosticCode::InvalidRequest:
        return "InvalidRequest";
    case PreparedGpuSceneDiagnosticCode::InvalidPlan:
        return "InvalidPlan";
    case PreparedGpuSceneDiagnosticCode::UnsupportedOperation:
        return "UnsupportedOperation";
    case PreparedGpuSceneDiagnosticCode::UnsupportedTransform:
        return "UnsupportedTransform";
    case PreparedGpuSceneDiagnosticCode::UnsupportedBlend:
        return "UnsupportedBlend";
    case PreparedGpuSceneDiagnosticCode::UnsupportedRequest:
        return "UnsupportedRequest";
    case PreparedGpuSceneDiagnosticCode::MediaUnavailable:
        return "MediaUnavailable";
    case PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded:
        return "PixelStorageBudgetExceeded";
    case PreparedGpuSceneDiagnosticCode::AllocationFailure:
        return "AllocationFailure";
    case PreparedGpuSceneDiagnosticCode::Cancelled:
        return "Cancelled";
    case PreparedGpuSceneDiagnosticCode::PreflightFailure:
        return "PreflightFailure";
    case PreparedGpuSceneDiagnosticCode::InternalInvariant:
        return "InternalInvariant";
    }
    return "Unknown";
}

[[nodiscard]] inline CompositionFormat format(const std::uint32_t width, const std::uint32_t height) {
    const auto value = CompositionFormat::create(width, height,
                                                 bloom::core::PixelAspectRatio::square());
    if (!value.has_value()) {
        throw std::logic_error("coverage fixture format must be valid");
    }
    return *value;
}

struct LayerIds final {
    bloom::document::ParameterId position;
    bloom::document::ParameterId anchor;
    bloom::document::ParameterId scale;
    bloom::document::ParameterId rotation;
    bloom::document::ParameterId opacity;
    bloom::document::ParameterId blendMode;
};

struct LayerValues final {
    bloom::document::Vec2d position{2.0, 1.0};
    bloom::document::Vec2d anchor{0.0, 0.0};
    bloom::document::Vec2d scale{1.0, 1.0};
    double rotation = 0.0;
    double opacity = 1.0;
    bloom::core::BlendMode blendMode = bloom::core::kDefaultBlendMode;
};

[[nodiscard]] inline CompiledLayerOutput layerOutput(const bloom::document::NodeId nodeId,
                                                     const bloom::document::LayerId layerId,
                                                     const OperationIndex input,
                                                     const LayerIds ids,
                                                     const LayerValues values) {
    return CompiledLayerOutput{nodeId,
                               layerId,
                               input,
                               CompiledVec2Parameter{ids.position, values.position},
                               CompiledVec2Parameter{ids.anchor, values.anchor},
                               CompiledVec2Parameter{ids.scale, values.scale},
                               CompiledScalarParameter{ids.rotation, values.rotation},
                               CompiledScalarParameter{ids.opacity, values.opacity},
                               ids.blendMode,
                               values.blendMode};
}

[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
publish(CompiledCompositionPlanDefinition definition) {
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] inline EvaluationRequest requestFor(const CompiledCompositionPlan& plan) {
    return EvaluationRequest{
        .time = RationalTime::fromInteger(0),
        .output = plan.output(),
        .resolution = bloom::runtime::CompositionFormatResolution{},
        .quality = bloom::runtime::EvaluationQuality::Reference,
        .colorIntent = bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = 1U << 28U,
    };
}

// A solid -> translation-only layer -> Normal merge -> output plan with two layers.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
twoLayerPlan(const CompositionFormat compositionFormat, const LayerValues a, const LayerValues b,
             const double solidWidth, const double solidHeight, const std::uint64_t idBase) {
    using bloom::document::LayerId;
    using bloom::document::LayerSlotId;
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    const LayerIds idsA{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                        ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                        ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{ParameterId::fromRaw(idBase + 6), ParameterId::fromRaw(idBase + 7),
                        ParameterId::fromRaw(idBase + 8), ParameterId::fromRaw(idBase + 9),
                        ParameterId::fromRaw(idBase + 10), ParameterId::fromRaw(idBase + 11)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{
        NodeId::fromRaw(idBase + 20),
        {ParameterId::fromRaw(idBase + 21), Color4d{0.5, 0.25, 0.125, 1.0}},
        {ParameterId::fromRaw(idBase + 22), solidWidth},
        {ParameterId::fromRaw(idBase + 23), solidHeight}});
    operations.emplace_back(
        layerOutput(NodeId::fromRaw(idBase + 24), LayerId::fromRaw(idBase + 30),
                    OperationIndex::fromRaw(0), idsA, a));
    operations.emplace_back(CompiledSolid{
        NodeId::fromRaw(idBase + 40),
        {ParameterId::fromRaw(idBase + 41), Color4d{0.125, 0.375, 0.75, 0.5}},
        {ParameterId::fromRaw(idBase + 42), solidWidth},
        {ParameterId::fromRaw(idBase + 43), solidHeight}});
    operations.emplace_back(
        layerOutput(NodeId::fromRaw(idBase + 44), LayerId::fromRaw(idBase + 45),
                    OperationIndex::fromRaw(2), idsB, b));
    const CompiledMergeInput first{LayerSlotId::fromRaw(idBase + 50), LayerId::fromRaw(idBase + 30),
                                   OperationIndex::fromRaw(1)};
    const CompiledMergeInput second{LayerSlotId::fromRaw(idBase + 51), LayerId::fromRaw(idBase + 45),
                                    OperationIndex::fromRaw(3)};
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 52), std::vector<CompiledMergeInput>{first, second}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 53), OperationIndex::fromRaw(4)});
    return publish(CompiledCompositionPlanDefinition{bloom::document::Revision::fromRaw(7),
                                                      bloom::document::ProjectId::fromRaw(1),
                                                      bloom::document::CompositionId::fromRaw(2),
                                                      compositionFormat, std::move(operations),
                                                      OperationIndex::fromRaw(5)});
}

} // namespace bloom::gpu_coverage_fixtures
