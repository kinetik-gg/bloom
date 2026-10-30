#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace bloom::runtime {
namespace {

[[nodiscard]] bool assetValid(const std::optional<document::AssetRecord>& asset) {
    return !asset || asset->validate().ok();
}

[[nodiscard]] bool operationAssetsValid(const CompiledOperation& operation) {
    if (const auto* image = std::get_if<CompiledImageSource>(&operation))
        return assetValid(image->asset);
    if (const auto* video = std::get_if<CompiledVideoSource>(&operation))
        return assetValid(video->asset);
    const auto sourcesValid = [](const auto& sources) {
        return std::ranges::all_of(sources,
                                   [](const auto& source) { return assetValid(source.asset); });
    };
    if (const auto* cryptomatte = std::get_if<CompiledCryptomatte>(&operation))
        return sourcesValid(cryptomatte->channels.sources);
    if (const auto* shuffle = std::get_if<CompiledChannelShuffle>(&operation))
        return sourcesValid(shuffle->channels.sources);
    if (const auto* effect = std::get_if<CompiledImageEffect>(&operation))
        if (const auto* file = std::get_if<FileTransformKernel>(&effect->kernel))
            return assetValid(file->asset);
    return true;
}

} // namespace

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
      audioMix_(std::move(definition.audioMix)), nestedPlans_(std::move(definition.nestedPlans)),
      duration_(definition.duration), clocks_(std::move(definition.clocks)),
      operationClocks_(std::move(definition.operationClocks)),
      placementClocks_(std::move(definition.placementClocks)) {
    analyzeTimeDependence();
    analyzeAssetValidity();
}

void CompiledCompositionPlan::analyzeAssetValidity() {
    operationAssetsValid_.reserve(operations_.size());
    for (const auto& operation : operations_)
        operationAssetsValid_.push_back(runtime::operationAssetsValid(operation) ? 1 : 0);
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
           lhs.audioMix_ == rhs.audioMix_ && lhs.duration_ == rhs.duration_ &&
           lhs.clocks_ == rhs.clocks_ && lhs.operationClocks_ == rhs.operationClocks_ &&
           lhs.placementClocks_ == rhs.placementClocks_ &&
           std::ranges::equal(lhs.nestedPlans_, rhs.nestedPlans_,
                              [](const auto& left, const auto& right) {
                                  return left == right || (left && right && *left == *right);
                              });
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
            .audioMix = audioMix_,
            .nestedPlans = nestedPlans_,
            .duration = duration_,
            .clocks = clocks_,
            .operationClocks = operationClocks_,
            .placementClocks = placementClocks_};
}

} // namespace bloom::runtime
