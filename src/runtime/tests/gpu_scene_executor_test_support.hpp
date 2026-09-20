#pragma once

// Compiled-plan fixture helpers and CPU oracle access for the GPU scene executor tests.
//
// These helpers are the same real CompiledCompositionPlan builders the CPU scene-preparation
// package uses (`bloom/runtime/cpu_gpu_scene_preparation.cpp`): a real solid -> translation-only
// layer -> Normal merge -> composition output graph with canonical parameter identities. No
// hand-built PreparedGpuScene is ever constructed by a test.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::runtime::executor_test {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::CompositionFormat;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
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
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

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

inline constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
inline constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);

[[nodiscard]] inline CompositionFormat
format(const std::uint32_t width, const std::uint32_t height,
       const bloom::core::PixelAspectRatio pixelAspect = bloom::core::PixelAspectRatio::square()) {
    const auto value = CompositionFormat::create(width, height, pixelAspect);
    if (!value.has_value()) {
        throw std::logic_error("test composition format must be valid");
    }
    return *value;
}

// Checked constructors for the canonical test fixture values, mirroring the scene-preparation
// support header. They keep every call site free of an unchecked optional dereference while
// preserving the fail-fast behaviour on an invalid fixture.
[[nodiscard]] inline bloom::core::PixelAspectRatio pixelAspect(const std::uint64_t numerator,
                                                               const std::uint64_t denominator) {
    const auto value = bloom::core::PixelAspectRatio::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("test pixel aspect must be valid");
    }
    return *value;
}

[[nodiscard]] inline RationalTime rationalTime(const std::int64_t numerator,
                                               const std::int64_t denominator) {
    const auto value = RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("test rational time must be valid");
    }
    return *value;
}

[[nodiscard]] inline CompiledLayerOutput layerOutput(const bloom::document::NodeId nodeId,
                                                     const bloom::document::LayerId layerId,
                                                     const OperationIndex input, const LayerIds ids,
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

[[nodiscard]] inline EvaluationRequest
requestFor(const CompiledCompositionPlan& plan,
           const RationalTime time = RationalTime::fromInteger(0),
           const std::size_t budget = 1U << 28U) {
    return EvaluationRequest{
        .time = time,
        .output = plan.output(),
        .resolution = bloom::runtime::CompositionFormatResolution{},
        .quality = bloom::runtime::EvaluationQuality::Reference,
        .colorIntent = bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
        .pixelStorageByteLimit = budget,
    };
}

// A two-solid graph: solid A -> layer A, solid B -> layer B, both merged bottom-to-top (A is the
// bottom entry), then a composition output. `colorA`/`colorB` are straight lin_rec709_scene values
// (RGB may be HDR or signed; alpha must be in [0, 1]).
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
twoSolidPlan(const CompositionFormat compositionFormat, const Color4d colorA,
             const LayerValues valuesA, const Color4d colorB, const LayerValues valuesB,
             const double solidWidth, const double solidHeight, const std::uint64_t idBase) {
    const LayerIds idsA{bloom::document::ParameterId::fromRaw(idBase + 0),
                        bloom::document::ParameterId::fromRaw(idBase + 1),
                        bloom::document::ParameterId::fromRaw(idBase + 2),
                        bloom::document::ParameterId::fromRaw(idBase + 3),
                        bloom::document::ParameterId::fromRaw(idBase + 4),
                        bloom::document::ParameterId::fromRaw(idBase + 5)};
    const LayerIds idsB{bloom::document::ParameterId::fromRaw(idBase + 6),
                        bloom::document::ParameterId::fromRaw(idBase + 7),
                        bloom::document::ParameterId::fromRaw(idBase + 8),
                        bloom::document::ParameterId::fromRaw(idBase + 9),
                        bloom::document::ParameterId::fromRaw(idBase + 10),
                        bloom::document::ParameterId::fromRaw(idBase + 11)};
    const auto solidNodeA = bloom::document::NodeId::fromRaw(idBase + 12);
    const auto solidNodeB = bloom::document::NodeId::fromRaw(idBase + 40);
    const auto layerNodeA = bloom::document::NodeId::fromRaw(idBase + 13);
    const auto layerNodeB = bloom::document::NodeId::fromRaw(idBase + 44);
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{solidNodeA,
                      {bloom::document::ParameterId::fromRaw(idBase + 20), colorA},
                      {bloom::document::ParameterId::fromRaw(idBase + 21), solidWidth},
                      {bloom::document::ParameterId::fromRaw(idBase + 22), solidHeight}});
    operations.emplace_back(layerOutput(layerNodeA, bloom::document::LayerId::fromRaw(idBase + 30),
                                        OperationIndex::fromRaw(0), idsA, valuesA));
    operations.emplace_back(
        CompiledSolid{solidNodeB,
                      {bloom::document::ParameterId::fromRaw(idBase + 41), colorB},
                      {bloom::document::ParameterId::fromRaw(idBase + 42), solidWidth},
                      {bloom::document::ParameterId::fromRaw(idBase + 43), solidHeight}});
    operations.emplace_back(layerOutput(layerNodeB, bloom::document::LayerId::fromRaw(idBase + 45),
                                        OperationIndex::fromRaw(2), idsB, valuesB));
    const CompiledMergeInput first{bloom::document::LayerSlotId::fromRaw(idBase + 50),
                                   bloom::document::LayerId::fromRaw(idBase + 30),
                                   OperationIndex::fromRaw(1)};
    const CompiledMergeInput second{bloom::document::LayerSlotId::fromRaw(idBase + 51),
                                    bloom::document::LayerId::fromRaw(idBase + 45),
                                    OperationIndex::fromRaw(3)};
    operations.emplace_back(CompiledMerge{bloom::document::NodeId::fromRaw(idBase + 52),
                                          std::vector<CompiledMergeInput>{first, second}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 53),
                                                      OperationIndex::fromRaw(4)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(5)});
}

// Backwards-compatible two-layer fixture with the canonical test colours.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
twoLayerPlan(const CompositionFormat compositionFormat, const LayerValues valuesA,
             const LayerValues valuesB, const double solidWidth, const double solidHeight,
             const std::uint64_t idBase) {
    return twoSolidPlan(compositionFormat, Color4d{0.5, 0.25, 0.125, 1.0}, valuesA,
                        Color4d{0.125, 0.375, 0.75, 0.5}, valuesB, solidWidth, solidHeight, idBase);
}

} // namespace bloom::runtime::executor_test
