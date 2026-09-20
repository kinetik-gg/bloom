#pragma once

// Shared fixtures and CPU reference replay for the GPU scene preparation tests.

// Focused tests for CPU-side GPU scene preparation (solid/translation-only/Normal merge/output).
//
// Every fixture is a real CompiledCompositionPlan built with canonical, globally unique parameter
// identities. The prepared commands are compared against a genuine CpuCompositionEvaluator frame:
// identity, bounds, output descriptor, and -- by replaying the commands with the EXISTING CPU
// primitives in this test only -- every output pixel bit for bit.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include "gpu_scene_coverage_geometry_test_support.hpp"
#include "layer_parent_transform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <ranges>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::CompositionFormat;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
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
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::PreparedGpuSceneDiagnosticCode;
using bloom::runtime::ProcessFrame;

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

[[maybe_unused, nodiscard]] std::string firstMismatch(const Rgba32fImage& replayed,
                                                      const Rgba32fImage& cpu) {
    if (replayed.pixels().size() != cpu.pixels().size()) {
        return "size " + std::to_string(replayed.pixels().size()) + " vs " +
               std::to_string(cpu.pixels().size());
    }
    for (std::size_t i = 0; i < replayed.pixels().size(); ++i) {
        if (!(replayed.pixels()[i] == cpu.pixels()[i])) {
            return "index " + std::to_string(i) + " (" +
                   std::to_string(replayed.pixels()[i].red()) + "," +
                   std::to_string(replayed.pixels()[i].green()) + ") vs (" +
                   std::to_string(cpu.pixels()[i].red()) + "," +
                   std::to_string(cpu.pixels()[i].green()) + ")";
        }
    }
    return "equal";
}

constexpr auto kProjectId = bloom::document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = bloom::document::CompositionId::fromRaw(2);
constexpr auto kSolidNodeA = bloom::document::NodeId::fromRaw(10);
constexpr auto kLayerNodeA = bloom::document::NodeId::fromRaw(11);

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

[[nodiscard]] CompositionFormat
format(const std::uint32_t width, const std::uint32_t height,
       const bloom::core::PixelAspectRatio pixelAspect = bloom::core::PixelAspectRatio::square()) {
    const auto value = CompositionFormat::create(width, height, pixelAspect);
    if (!value.has_value()) {
        throw std::logic_error("test composition format must be valid");
    }
    return *value;
}

// Checked constructors for the canonical test fixture values. These keep every call site free of an
// unchecked optional dereference while preserving the fail-fast behaviour on an invalid fixture.
[[maybe_unused, nodiscard]] bloom::core::PixelAspectRatio
pixelAspect(const std::uint64_t numerator, const std::uint64_t denominator) {
    const auto value = bloom::core::PixelAspectRatio::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("test pixel aspect must be valid");
    }
    return *value;
}

[[maybe_unused, nodiscard]] RationalTime rationalTime(const std::int64_t numerator,
                                                      const std::int64_t denominator) {
    const auto value = RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("test rational time must be valid");
    }
    return *value;
}

[[nodiscard]] CompiledLayerOutput layerOutput(const bloom::document::NodeId nodeId,
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

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
publish(CompiledCompositionPlanDefinition definition) {
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] EvaluationRequest requestFor(const CompiledCompositionPlan& plan,
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

#include "gpu_scene_preparation_plan_builders.ipp"

#include "gpu_scene_preparation_replay.ipp"

} // namespace
