// ADR 0024 layer-owned time: resolves every layer time clock of a compiled plan for one request.
// Clock 0 is the request time. A Layer clock maps its parent clock through the defining Layer
// Output's start time and speed; a Content clock samples that layer's Time Remap on the Layer clock
// and applies its out-of-extent modes, and may be absent. Each present clock then samples every
// curve and evaluates the value graph at its own time, so an operation's operands resolve exactly
// as on clock 0, only at a different time. Shared by the CPU evaluator and GPU scene preparation
// through detail::preflight.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"

#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/composition_time.hpp>

#include <utility>

namespace bloom::runtime::detail {
namespace {

// The base resolution re-sampled at `time`: curve samples and value outputs change, everything
// else (descriptor, scales, budgets) is the request's.
[[nodiscard]] std::optional<ResolvedEvaluation>
sampleAt(const CompiledCompositionPlan& plan, const ResolvedEvaluation& base,
         const core::RationalTime time, const CancellationToken& cancellation,
         OperationCache* cache, OperationCacheStatistics* statistics, bool& cancelled) {
    ResolvedEvaluation result = base;
    result.scalarCurveValues.clear();
    result.vec2CurveValues.clear();
    result.color4CurveValues.clear();
    result.scalarCurveValues.reserve(plan.scalarCurves().size());
    for (const auto& curve : plan.scalarCurves()) {
        const auto sample = sampleAnimationCurve(curve, time, cancellation);
        if (sample.error == AnimationSamplingError::Cancelled) {
            cancelled = true;
            return std::nullopt;
        }
        if (!sample || !sample.value || !sample.segmentStart)
            return std::nullopt;
        result.scalarCurveValues.push_back({*sample.value, *sample.segmentStart});
    }
    result.vec2CurveValues.reserve(plan.vec2Curves().size());
    for (const auto& curve : plan.vec2Curves()) {
        const auto sample = sampleAnimationCurve(curve, time, cancellation);
        if (sample.error == AnimationSamplingError::Cancelled) {
            cancelled = true;
            return std::nullopt;
        }
        if (!sample || !sample.value || !sample.segmentStart)
            return std::nullopt;
        result.vec2CurveValues.push_back({*sample.value, *sample.segmentStart});
    }
    result.color4CurveValues.reserve(plan.color4Curves().size());
    for (const auto& curve : plan.color4Curves()) {
        const auto sample = sampleAnimationCurve(curve, time, cancellation);
        if (sample.error == AnimationSamplingError::Cancelled) {
            cancelled = true;
            return std::nullopt;
        }
        if (!sample || !sample.value || !sample.segmentStart)
            return std::nullopt;
        result.color4CurveValues.push_back({*sample.value, *sample.segmentStart});
    }
    if (!plan.valueOperations().empty()) {
        const bool hasPostImageValues = std::ranges::any_of(
            plan.valueOperations(), [](const auto& operation) { return operation.requiresPostImage; });
        ValueGraphMemoization memoization{cache,
                                          statistics,
                                          plan.sourceRevision(),
                                          plan.projectId(),
                                          plan.compositionId(),
                                          &cancellation,
                                          plan.valueTimeDependence(),
                                          {},
                                          {},
                                          {}};
        memoization.pass = hasPostImageValues ? ValueGraphPass::PreImage : ValueGraphPass::All;
        auto graph = evaluateValueGraph(plan.valueOperations(), plan.valueOutputCount(), time,
                                        plan.format().frameRate(),
                                        ValueGraphCurves{plan.scalarCurves(), plan.vec2Curves(),
                                                         plan.color4Curves(), plan.vec3Curves()},
                                        memoization);
        if (cancellation.isCancellationRequested()) {
            cancelled = true;
            return std::nullopt;
        }
        result.valueOutputs = std::move(graph.outputs);
    }
    return result;
}

} // namespace

PreflightOutcome resolveLayerClocks(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                    const EvaluationRequest& request, ResolvedEvaluation base,
                                    const CancellationToken& cancellation, OperationCache* cache,
                                    OperationCacheStatistics* statistics) {
    const auto clocks = plan->clocks();
    if (clocks.size() <= 1)
        return PreflightOutcome::success(std::move(base));
    std::vector<std::optional<core::RationalTime>> times(clocks.size());
    // Absent clocks keep a copy of the base resolution; it is never read (timeForClock answers
    // nullopt for them first).
    std::vector<ResolvedEvaluation> resolutions(clocks.size(), base);
    times[0] = request.time;
    const auto failure = [](std::string summary, const document::NodeId layerNode) {
        EvaluationSubject subject;
        subject.nodeId = layerNode;
        subject.field = "layerTime";
        return PreflightOutcome::failure(diagnostic(EvaluationDiagnosticCode::ArithmeticOverflow,
                                                    std::move(summary), {}, std::move(subject)));
    };
    for (std::size_t index = 1; index < clocks.size(); ++index) {
        if (cancellation.isCancellationRequested())
            return PreflightOutcome::cancellation();
        const auto& clock = clocks[index];
        const auto layerIndex = clock.layer.value();
        const auto* layer = layerIndex < plan->operations().size()
                                ? std::get_if<CompiledLayerOutput>(&plan->operations()[layerIndex])
                                : nullptr;
        if (clock.parent >= index || layer == nullptr || !layer->time)
            return PreflightOutcome::failure(diagnostic(EvaluationDiagnosticCode::InvalidPlan,
                                                        "Layer time clock is malformed"));
        const auto& mapping = *layer->time;
        const auto& parentTime = times[clock.parent];
        if (!parentTime)
            continue; // an absent parent makes every descendant clock absent
        std::optional<core::RationalTime> time;
        if (clock.kind == CompiledClockKind::Layer) {
            time = mapLayerTime(*parentTime, mapping.startTime, mapping.speed);
            if (!time)
                return failure("Layer time cannot be represented", layer->sourceNodeId);
        } else {
            auto remapped = *parentTime;
            if (mapping.timeRemap) {
                const auto& layerClock =
                    clock.parent == 0 ? base : resolutions[clock.parent];
                const auto sampled = resolveParameter(*mapping.timeRemap, *plan, layerClock);
                const auto seconds = sampled ? secondsToTime(sampled->value) : std::nullopt;
                if (!seconds)
                    return failure("Layer Time Remap cannot be represented", layer->sourceNodeId);
                remapped = *seconds;
            }
            if (mapping.extentDuration) {
                const auto wrapped =
                    wrapToExtent(remapped, {*mapping.extentDuration, mapping.extentFrameRate},
                                 mapping.beforeMode, mapping.afterMode);
                if (!wrapped)
                    return failure("Layer content time cannot be represented",
                                   layer->sourceNodeId);
                if (!wrapped->present)
                    continue; // absent content: transparent and silent
                remapped = wrapped->time;
            }
            time = remapped;
        }
        bool cancelled = false;
        auto sampled = sampleAt(*plan, base, *time, cancellation, cache, statistics, cancelled);
        if (cancelled)
            return PreflightOutcome::cancellation();
        if (!sampled)
            return PreflightOutcome::failure(diagnostic(
                EvaluationDiagnosticCode::InvalidPlan,
                "Layer time clock could not sample the plan's animation"));
        times[index] = time;
        resolutions[index] = std::move(*sampled);
    }
    base.clockTimes = std::move(times);
    base.clockResolutions = std::move(resolutions);
    return PreflightOutcome::success(std::move(base));
}

} // namespace bloom::runtime::detail
