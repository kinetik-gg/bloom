#include <bloom/runtime/compiled_plan.hpp>

#include <utility>

namespace bloom::runtime {

CompiledCompositionPlan::CompiledCompositionPlan(
    CompiledCompositionPlanDefinition definition) noexcept
    : sourceRevision_(definition.sourceRevision), projectId_(definition.projectId),
      compositionId_(definition.compositionId), format_(definition.format),
      operations_(std::move(definition.operations)), output_(definition.output),
      scalarCurves_(std::move(definition.scalarCurves)),
      vec2Curves_(std::move(definition.vec2Curves)),
      planSemanticsVersion_(definition.planSemanticsVersion),
      animationSamplingSemanticsVersion_(definition.animationSamplingSemanticsVersion) {}

CompiledCompositionPlanDefinition CompiledCompositionPlan::copyDefinition() const {
    return {.sourceRevision = sourceRevision_,
            .projectId = projectId_,
            .compositionId = compositionId_,
            .format = format_,
            .operations = operations_,
            .output = output_,
            .scalarCurves = scalarCurves_,
            .vec2Curves = vec2Curves_,
            .planSemanticsVersion = planSemanticsVersion_,
            .animationSamplingSemanticsVersion = animationSamplingSemanticsVersion_};
}

} // namespace bloom::runtime
