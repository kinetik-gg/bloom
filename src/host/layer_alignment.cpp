#include <bloom/host/layer_alignment.hpp>

#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <unordered_set>

namespace bloom::host {
namespace {

using commands::AlignmentLinear;
using document::Vec2d;

constexpr AlignmentLinear kSingular{0.0, 0.0, 0.0, 0.0};

[[nodiscard]] const document::ParameterRecord*
parameterOf(const document::Composition& composition, const document::LayerOutputBoundary& layer,
            const std::string_view role) {
    const auto* node = composition.graph().findNode(layer.nodeId);
    if (node == nullptr)
        return nullptr;
    const auto binding =
        std::ranges::find(node->parameters, role, &document::ParameterBinding::role);
    return binding == node->parameters.end() ? nullptr
                                             : composition.parameters().find(binding->parameterId);
}

// The authored value at a key time: a constant, or its curve sampled exactly as the evaluator
// samples it. Driven values (and anything else) have no document-side sample.
[[nodiscard]] std::optional<Vec2d> sampleVec2(const document::Composition& composition,
                                              const document::ParameterRecord* record,
                                              const core::RationalTime keyTime) {
    if (record == nullptr)
        return std::nullopt;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source)) {
        const auto* value = std::get_if<Vec2d>(&constant->value);
        return value ? std::optional(*value) : std::nullopt;
    }
    const auto* animated = std::get_if<document::AnimationCurveSource>(&record->source);
    const auto* curve =
        animated ? composition.animationCurves().findVec2(animated->curveId) : nullptr;
    if (curve == nullptr)
        return std::nullopt;
    Vec2d fallback{};
    if (animated->defaultValue)
        if (const auto* value = std::get_if<Vec2d>(&*animated->defaultValue))
            fallback = *value;
    const auto sample = runtime::sampleAnimationCurve(*curve, keyTime, fallback);
    return sample ? sample.value : std::nullopt;
}

[[nodiscard]] std::optional<double> sampleScalar(const document::Composition& composition,
                                                 const document::ParameterRecord* record,
                                                 const core::RationalTime keyTime) {
    if (record == nullptr)
        return std::nullopt;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source)) {
        const auto* value = std::get_if<double>(&constant->value);
        return value ? std::optional(*value) : std::nullopt;
    }
    const auto* animated = std::get_if<document::AnimationCurveSource>(&record->source);
    const auto* curve =
        animated ? composition.animationCurves().findScalar(animated->curveId) : nullptr;
    if (curve == nullptr)
        return std::nullopt;
    const auto sample = runtime::sampleAnimationCurve(*curve, keyTime);
    return sample ? sample.value : std::nullopt;
}

// The linear part the evaluator's polygon was mapped with: polygon = world * local corners.
[[nodiscard]] std::optional<AlignmentLinear>
worldLinear(const runtime::EvaluatedOperationBounds& b) {
    const double width = b.local.right - b.local.left;
    const double height = b.local.bottom - b.local.top;
    if (!(width > 0.0) || !(height > 0.0))
        return std::nullopt;
    const AlignmentLinear world{
        (b.polygon[1].x - b.polygon[0].x) / width, (b.polygon[1].y - b.polygon[0].y) / width,
        (b.polygon[3].x - b.polygon[0].x) / height, (b.polygon[3].y - b.polygon[0].y) / height};
    if (!std::isfinite(world.xx) || !std::isfinite(world.xy) || !std::isfinite(world.yx) ||
        !std::isfinite(world.yy))
        return std::nullopt;
    return world;
}

// runtime::detail::LayerMatrix::authored's linear part, with its exact quarter-turn cases.
[[nodiscard]] AlignmentLinear authoredLinear(const Vec2d scale, const double degrees) {
    const double angle = std::remainder(degrees, 360.0);
    double cosine = std::cos(angle * std::numbers::pi / 180.0);
    double sine = std::sin(angle * std::numbers::pi / 180.0);
    if (angle == 0) {
        cosine = 1;
        sine = 0;
    } else if (angle == 90) {
        cosine = 0;
        sine = 1;
    } else if (angle == -90) {
        cosine = 0;
        sine = -1;
    } else if (std::abs(angle) == 180) {
        cosine = -1;
        sine = 0;
    }
    return {cosine * scale.x, sine * scale.x, -sine * scale.y, cosine * scale.y};
}

[[nodiscard]] AlignmentLinear times(const AlignmentLinear& a, const AlignmentLinear& b) {
    return {a.xx * b.xx + a.yx * b.xy, a.xy * b.xx + a.yy * b.xy, a.xx * b.yx + a.yx * b.yy,
            a.xy * b.yx + a.yy * b.yy};
}

[[nodiscard]] std::optional<AlignmentLinear> inverse(const AlignmentLinear& m) {
    const double determinant = m.xx * m.yy - m.yx * m.xy;
    if (!std::isfinite(determinant) || determinant == 0.0)
        return std::nullopt;
    const AlignmentLinear result{m.yy / determinant, -m.xy / determinant, -m.yx / determinant,
                                 m.xx / determinant};
    if (!std::isfinite(result.xx) || !std::isfinite(result.xy) || !std::isfinite(result.yx) ||
        !std::isfinite(result.yy))
        return std::nullopt;
    return result;
}

} // namespace

std::vector<commands::LayerAlignmentGeometry>
layerAlignmentGeometry(const document::Composition& composition, const core::RationalTime time,
                       const std::span<const runtime::EvaluatedOperationBounds> bounds) {
    std::vector<commands::LayerAlignmentGeometry> result;
    std::unordered_set<std::uint64_t> seen;
    const auto boundsOf = [&bounds](const document::LayerId id) {
        const auto found =
            std::ranges::find(bounds, id, &runtime::EvaluatedOperationBounds::layerId);
        return found == bounds.end() ? nullptr : &*found;
    };
    for (const auto& entry : bounds) {
        const auto* layer = composition.graph().findLayer(entry.layerId);
        if (layer == nullptr || entry.output.empty() || !seen.insert(entry.layerId.value()).second)
            continue;
        commands::LayerAlignmentGeometry geometry{.layer = entry.layerId,
                                                  .bounds = {entry.output.left, entry.output.top,
                                                             entry.output.right,
                                                             entry.output.bottom}};
        const auto keyTime = commands::layerKeyTimeOfCompositionTime(layer->timing, time);
        if (keyTime) {
            const auto position = sampleVec2(
                composition, parameterOf(composition, *layer, document::kPositionParameterRole),
                *keyTime);
            geometry.position = position;
        }
        if (layer->parent) {
            geometry.parentLinear = kSingular;
            // parent = world * own^-1, with the layer's own scale and rotation at its key time.
            const auto scale =
                keyTime
                    ? sampleVec2(composition,
                                 parameterOf(composition, *layer, document::kScaleParameterRole),
                                 *keyTime)
                    : std::nullopt;
            const auto rotation = keyTime
                                      ? sampleScalar(composition,
                                                     parameterOf(composition, *layer,
                                                                 document::kRotationParameterRole),
                                                     *keyTime)
                                      : std::nullopt;
            const auto world = worldLinear(entry);
            const auto own =
                scale && rotation ? inverse(authoredLinear(*scale, *rotation)) : std::nullopt;
            if (world && own) {
                geometry.parentLinear = times(*world, *own);
            } else if (const auto* parent = boundsOf(*layer->parent)) {
                // The parent's own world transform is exactly this layer's parent transform.
                if (const auto parentWorld = worldLinear(*parent))
                    geometry.parentLinear = *parentWorld;
            }
        }
        result.push_back(geometry);
    }
    return result;
}

LayerAlignmentGeometryResult
measureLayerAlignmentGeometry(const LayerAlignmentMeasureRequest& request,
                              const runtime::CancellationToken& cancellation) {
    LayerAlignmentGeometryResult result;
    const auto* composition = request.snapshot.project().findComposition(request.composition);
    if (composition == nullptr) {
        result.diagnostic = "The composition does not exist";
        return result;
    }
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(request.assetBaseDirectory);
    // Every layer: a moving layer's parent supplies its transform when the layer's own cannot.
    const auto bounds =
        runtime::queryLayerBounds(compiler, evaluator,
                                  {.snapshot = request.snapshot,
                                   .composition = request.composition,
                                   .time = request.time,
                                   .layers = {},
                                   .colorIntent = request.colorIntent,
                                   .pixelStorageByteLimit = request.pixelStorageByteLimit},
                                  cancellation);
    if (!bounds.succeeded()) {
        result.diagnostic = bounds.diagnostic;
        return result;
    }
    result.succeeded = true;
    result.geometry = layerAlignmentGeometry(*composition, request.time, bounds.bounds);
    return result;
}

} // namespace bloom::host
