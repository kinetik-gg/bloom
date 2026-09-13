#pragma once
#include <bloom/document/project.hpp>
#include <algorithm>
namespace bloom::commands::detail {
inline bool changesLockedLayers(const document::Project& before, const document::Project& after) {
    for (const auto& composition : before.compositions()) {
        const auto* changed = after.findComposition(composition.id());
        if (!changed) continue; // Composition deletion is a separate authoring scope.
        for (const auto& layer : composition.graph().layerOutputs()) {
            if (!layer.locked) continue;
            const auto* next = changed->graph().findLayer(layer.layerId);
            if (!next || layer.inPoint != next->inPoint || layer.endPoint(composition.duration()) != next->endPoint(changed->duration())) return true;
        }
        for (const auto& node : composition.graph().nodes()) {
            if (!composition.nodeLocked(node.id)) continue;
            const auto* next = changed->graph().findNode(node.id);
            if (!next || node != *next) return true;
            for (const auto& binding : node.parameters) {
                const auto* parameter = composition.parameters().find(binding.parameterId);
                const auto* edited = changed->parameters().find(binding.parameterId);
                if (!parameter || !edited || *parameter != *edited) return true;
                if (const auto* curve = std::get_if<document::AnimationCurveSource>(&parameter->source)) {
                    const auto* oldCurve = composition.animationCurves().find(curve->curveId);
                    const auto* newCurve = changed->animationCurves().find(curve->curveId);
                    if (!oldCurve || !newCurve || *oldCurve != *newCurve) return true;
                }
            }
        }
    }
    return false;
}
} // namespace bloom::commands::detail
