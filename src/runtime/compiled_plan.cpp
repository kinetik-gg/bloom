#include <bloom/runtime/compiled_plan.hpp>

#include <utility>

namespace bloom::runtime {

CompiledCompositionPlan::CompiledCompositionPlan(CompiledCompositionPlanDefinition definition)
    : bypassOperationCache_(definition.bypassOperationCache),
      sourceRevision_(definition.sourceRevision), projectId_(definition.projectId),
      compositionId_(definition.compositionId), format_(definition.format),
      operations_(std::move(definition.operations)), output_(definition.output),
      scalarCurves_(std::move(definition.scalarCurves)),
      vec2Curves_(std::move(definition.vec2Curves)), vec3Curves_(std::move(definition.vec3Curves)),
      color4Curves_(std::move(definition.color4Curves)),
      valueOperations_(std::move(definition.valueOperations)),
      valueOutputCount_(definition.valueOutputCount),
      planSemanticsVersion_(definition.planSemanticsVersion),
      animationSamplingSemanticsVersion_(definition.animationSamplingSemanticsVersion),
      audioMix_(std::move(definition.audioMix)) {
    analyzeTimeDependence();
}

bool operator==(const CompiledCompositionPlan& lhs, const CompiledCompositionPlan& rhs) {
    return lhs.sourceRevision_ == rhs.sourceRevision_ && lhs.projectId_ == rhs.projectId_ &&
           lhs.compositionId_ == rhs.compositionId_ && lhs.format_ == rhs.format_ &&
           lhs.operations_ == rhs.operations_ && lhs.output_ == rhs.output_ &&
           lhs.scalarCurves_ == rhs.scalarCurves_ && lhs.vec2Curves_ == rhs.vec2Curves_ &&
           lhs.vec3Curves_ == rhs.vec3Curves_ && lhs.color4Curves_ == rhs.color4Curves_ &&
           lhs.valueOperations_ == rhs.valueOperations_ &&
           lhs.valueOutputCount_ == rhs.valueOutputCount_ &&
           lhs.planSemanticsVersion_ == rhs.planSemanticsVersion_ &&
           lhs.animationSamplingSemanticsVersion_ == rhs.animationSamplingSemanticsVersion_ &&
           lhs.audioMix_ == rhs.audioMix_;
}

CompiledCompositionPlanDefinition CompiledCompositionPlan::copyDefinition() const {
    return {.sourceRevision = sourceRevision_,
            .projectId = projectId_,
            .compositionId = compositionId_,
            .format = format_,
            .operations = operations_,
            .output = output_,
            .scalarCurves = scalarCurves_,
            .vec2Curves = vec2Curves_,
            .vec3Curves = vec3Curves_,
            .color4Curves = color4Curves_,
            .valueOperations = valueOperations_,
            .valueOutputCount = valueOutputCount_,
            .planSemanticsVersion = planSemanticsVersion_,
            .animationSamplingSemanticsVersion = animationSamplingSemanticsVersion_,
            .bypassOperationCache = bypassOperationCache_,
            .audioMix = audioMix_};
}

} // namespace bloom::runtime
