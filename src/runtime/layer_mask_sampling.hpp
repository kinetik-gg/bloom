#pragma once

// ADR 0028: samples a compiled LayerMaskKernel for one request and prepares its geometry. Shared by
// the CPU evaluator and GPU scene preparation so both routes see the same prepared masks.

#include "cpu_composition_resolution.hpp"
#include "layer_mask_coverage.hpp"

#include <bloom/runtime/animation_sampling.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace bloom::runtime::detail {

// Null when a value is not evaluable (absent clock time, unresolved operand, out-of-domain value).
[[nodiscard]] inline std::shared_ptr<const PreparedLayerMasks>
sampleLayerMasks(const LayerMaskKernel& kernel, const CompiledCompositionPlan& plan,
                 const ResolvedEvaluation& resolved, const std::optional<core::RationalTime> time,
                 const double horizontalScale, const double verticalScale) {
    std::vector<SampledLayerMask> sampled;
    sampled.reserve(kernel.masks.size());
    for (const auto& mask : kernel.masks) {
        SampledLayerMask item{mask.mode, mask.inverted, {}, 0.0, 0.0, 1.0};
        if (const auto* constant = std::get_if<document::PathValue>(&mask.path.source)) {
            item.path = *constant;
        } else {
            if (!time)
                return nullptr;
            auto path = samplePathCurve(std::get<CompiledPathCurve>(mask.path.source), *time);
            if (!path)
                return nullptr;
            item.path = std::move(*path);
        }
        const auto feather = resolveParameter(mask.feather, plan, resolved);
        const auto expansion = resolveParameter(mask.expansion, plan, resolved);
        const auto opacity = resolveParameter(mask.opacity, plan, resolved);
        if (!feather || !expansion || !opacity)
            return nullptr;
        item.feather = feather->value;
        item.expansion = expansion->value;
        item.opacity = opacity->value;
        sampled.push_back(std::move(item));
    }
    auto prepared = prepareLayerMasks(sampled, horizontalScale, verticalScale);
    return prepared ? std::make_shared<const PreparedLayerMasks>(std::move(*prepared)) : nullptr;
}

} // namespace bloom::runtime::detail
