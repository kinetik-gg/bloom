// INSTANCE-0 throwaway spike. It is NOT product code and is excluded from the default build.
//
// It answers one question with the real CPU evaluator: what does an instance set COST if it is
// expanded eagerly into the plan grammar Bloom already has -- one shared source, N Layer Outputs,
// one Merge with N entries -- and how much of that cost survives into a second frame through the
// operation cache. Everything the ADR claims about an Instancer's evaluation shape is measured
// against this baseline, so the design argues from numbers rather than from intuition.
//
// Four measurements, printed as labelled lines and repeated in a machine-readable summary:
//   budget      what the evaluator's preflight estimate says about N expanded instances
//   expansion   one cold evaluation of N instances, wall time and retained bytes
//   warm        the same plan again on the same evaluator: cache hits, misses, wall time
//   edit        one instance moved and the plan republished at a new revision: how many
//               operations the content-addressed cache still serves
//   values      a value graph of 4 x N per-element scalars, to price per-element attributes as
//               individual value operations
//
// Build and run: see README.md in this directory.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/operation_index.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace core = bloom::core;
namespace document = bloom::document;
namespace runtime = bloom::runtime;

// Identities are arbitrary but distinct: the evaluator keys cache entries by source node id, so two
// instances that shared one would look like one operation to the cache and make the whole
// measurement meaningless.
constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(1);
constexpr auto kSolidNode = document::NodeId::fromRaw(1);
constexpr auto kMergeNode = document::NodeId::fromRaw(2);
constexpr auto kOutputNode = document::NodeId::fromRaw(3);
constexpr std::uint64_t kInstanceNodeBase = 1000;
constexpr std::uint64_t kValueNodeBase = 900000;

struct Settings final {
    std::size_t instances = 500;
    std::uint32_t compositionWidth = 1920;
    std::uint32_t compositionHeight = 1080;
    double cellWidth = 64.0;
    double cellHeight = 64.0;
    std::size_t sweepHeight = 135; // one eighth of 1080, to isolate the merge's height term
    // The budget an interactive preview actually offers a request. The preflight estimate charges
    // one FULL composition frame per image operation that is alive at once, so this is the number
    // the expansion baseline collides with before it draws a pixel.
    std::size_t previewBudgetBytes = std::size_t{2} * 1024 * 1024 * 1024;
    // Deliberately far above anything the measurement needs, so the remaining runs measure
    // evaluation rather than the estimate.
    std::size_t measurementBudgetBytes = std::size_t{64} * 1024 * 1024 * 1024;
};

struct Measurement final {
    double milliseconds = 0.0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t retainedBytes = 0;
};

[[nodiscard]] document::CompositionFormat format(const std::uint32_t width,
                                                 const std::uint32_t height) {
    const auto value = document::CompositionFormat::create(width, height);
    if (!value.has_value())
        throw std::logic_error("spike composition format must be valid");
    return *value;
}

[[nodiscard]] document::ParameterId parameterFor(const std::uint64_t node,
                                                 const std::uint64_t role) {
    return document::ParameterId::fromRaw(node * 16 + role);
}

// The grid layout the design calls the first built-in solver: index -> (column, row) -> a centre in
// composition space. Written here as plain arithmetic because the point of the spike is the cost of
// EVALUATING N placed instances, not the cost of choosing the places.
[[nodiscard]] document::Vec2d gridPosition(const std::size_t index, const std::size_t columns,
                                           const Settings& settings) {
    const auto column = static_cast<double>(index % columns);
    const std::size_t rowIndex = index / columns;
    const auto row = static_cast<double>(rowIndex);
    return {column * settings.cellWidth + settings.cellWidth / 2.0,
            row * settings.cellHeight + settings.cellHeight / 2.0};
}

[[nodiscard]] std::size_t columnsFor(const Settings& settings) {
    const auto columns =
        static_cast<std::size_t>(std::floor(settings.compositionWidth / settings.cellWidth));
    return columns == 0 ? 1 : columns;
}

// One shared Solid, N Layer Outputs over it, one Merge holding all N, one Output. This is exactly
// what "make 500 copies" means in today's plan grammar, and therefore the honest baseline an
// Instancer operation has to beat.
[[nodiscard]] runtime::CompiledCompositionPlanDefinition
expandedInstanceSet(const Settings& settings, const std::uint64_t revision,
                    const std::size_t movedInstance = static_cast<std::size_t>(-1),
                    const double movedOffset = 0.0) {
    const auto compositionFormat = format(settings.compositionWidth, settings.compositionHeight);
    const auto columns = columnsFor(settings);

    std::vector<runtime::CompiledOperation> operations;
    operations.reserve(settings.instances + 3);
    operations.emplace_back(runtime::CompiledSolid{
        kSolidNode,
        {parameterFor(kSolidNode.value(), 0), core::Color4d{0.8, 0.3, 0.1, 1.0}},
        {parameterFor(kSolidNode.value(), 1), settings.cellWidth},
        {parameterFor(kSolidNode.value(), 2), settings.cellHeight}});

    std::vector<runtime::CompiledMergeInput> entries;
    entries.reserve(settings.instances);
    for (std::size_t instance = 0; instance < settings.instances; ++instance) {
        const auto node = kInstanceNodeBase + instance;
        auto position = gridPosition(instance, columns, settings);
        if (instance == movedInstance)
            position.x += movedOffset;
        const auto layerId = document::LayerId::fromRaw(node);
        operations.emplace_back(
            runtime::CompiledLayerOutput{document::NodeId::fromRaw(node),
                                         layerId,
                                         runtime::OperationIndex::fromRaw(0),
                                         {parameterFor(node, 0), position},
                                         {parameterFor(node, 1), document::Vec2d{0.0, 0.0}},
                                         {parameterFor(node, 2), document::Vec2d{1.0, 1.0}},
                                         {parameterFor(node, 3), 0.0},
                                         {parameterFor(node, 4), 1.0},
                                         parameterFor(node, 5),
                                         core::BlendMode::Normal});
        entries.push_back(
            runtime::CompiledMergeInput{document::LayerSlotId::fromRaw(node), layerId,
                                        runtime::OperationIndex::fromRaw(operations.size() - 1)});
    }

    operations.emplace_back(runtime::CompiledMerge{kMergeNode, std::move(entries)});
    const auto mergeIndex = runtime::OperationIndex::fromRaw(operations.size() - 1);
    operations.emplace_back(runtime::CompiledCompositionOutput{kOutputNode, mergeIndex});
    const auto outputIndex = runtime::OperationIndex::fromRaw(operations.size() - 1);

    return runtime::CompiledCompositionPlanDefinition{document::Revision::fromRaw(revision),
                                                      kProjectId,
                                                      kCompositionId,
                                                      compositionFormat,
                                                      std::move(operations),
                                                      outputIndex};
}

// The cheapest possible image plan, carrying a value graph of `count` independent scalar
// operations. It prices ONE per-element attribute evaluated as its own value-graph node, which is
// the alternative the ADR rejects in favour of an array value kind.
[[nodiscard]] runtime::CompiledCompositionPlanDefinition
perElementValueGraph(const Settings& settings, const std::size_t count) {
    Settings single = settings;
    single.instances = 1;
    auto definition = expandedInstanceSet(single, 11);

    definition.valueOperations.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto node = document::NodeId::fromRaw(kValueNodeBase + index);
        runtime::CompiledValueScalarMath math;
        math.operation = core::primitives::ScalarPrimitive::MultiplyAdd;
        math.operands = {
            {parameterFor(node.value(), 0), runtime::CompiledValue{static_cast<double>(index)}},
            {parameterFor(node.value(), 1), runtime::CompiledValue{0.125}},
            {parameterFor(node.value(), 2), runtime::CompiledValue{1.0}}};
        definition.valueOperations.push_back(
            runtime::CompiledValueOperation{node, runtime::ValueOutputIndex::fromRaw(index), 1,
                                            runtime::CompiledValueKernel{std::move(math)}, false});
    }
    definition.valueOutputCount = count;
    return definition;
}

[[nodiscard]] runtime::EvaluationRequest requestFor(const runtime::CompiledCompositionPlan& plan,
                                                    const std::size_t budgetBytes) {
    return {.time = core::RationalTime::fromInteger(0),
            .output = plan.output(),
            .resolution = runtime::CompositionFormatResolution{},
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
            .pixelStorageByteLimit = budgetBytes};
}

// The refusal an under-budget request produces, with the evaluator's own estimate in it. Returned
// rather than thrown because the preflight probe EXPECTS one.
[[nodiscard]] std::string
refusalDetail(const runtime::CpuCompositionEvaluator& evaluator,
              const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
              const std::size_t budgetBytes) {
    auto result = evaluator.evaluate(plan, requestFor(*plan, budgetBytes), {});
    if (result.status() == runtime::EvaluationStatus::Evaluated)
        return "accepted";
    std::string detail;
    for (const auto& diagnostic : result.diagnostics())
        detail += diagnostic.summary + " (" + diagnostic.detail + ") ";
    return detail.empty() ? "refused without a diagnostic" : detail;
}

[[nodiscard]] Measurement
evaluateOnce(const runtime::CpuCompositionEvaluator& evaluator,
             const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
             const std::size_t budgetBytes) {
    runtime::OperationCacheStatistics statistics;
    const auto start = std::chrono::steady_clock::now();
    auto result =
        evaluator.evaluate(plan, requestFor(*plan, budgetBytes), {}, {}, nullptr, &statistics);
    const auto finish = std::chrono::steady_clock::now();
    if (result.status() != runtime::EvaluationStatus::Evaluated) {
        for (const auto& diagnostic : result.diagnostics())
            std::cerr << "  diagnostic: " << diagnostic.summary << ' ' << diagnostic.detail << '\n';
        throw std::runtime_error("spike evaluation did not produce a frame");
    }
    return {std::chrono::duration<double, std::milli>(finish - start).count(), statistics.hits,
            statistics.misses, evaluator.operationCache()->retainedBytes()};
}

void report(const std::string& label, const Measurement& measurement) {
    std::cout << std::left << std::setw(28) << label << std::right << std::fixed
              << std::setprecision(2) << std::setw(10) << measurement.milliseconds << " ms"
              << std::setw(10) << measurement.hits << " hits" << std::setw(10) << measurement.misses
              << " misses" << std::setw(12) << measurement.retainedBytes / 1024 / 1024 << " MiB\n";
}

[[nodiscard]] std::size_t argumentOr(const int argc, char** argv, const int position,
                                     const std::size_t fallback) {
    if (position >= argc)
        return fallback;
    return static_cast<std::size_t>(std::strtoull(argv[position], nullptr, 10));
}

} // namespace

int main(int argc, char** argv) {
    try {
        Settings settings;
        settings.instances = argumentOr(argc, argv, 1, settings.instances);

        std::cout << "INSTANCE-0 eager-expansion cost spike\n"
                  << "composition " << settings.compositionWidth << 'x'
                  << settings.compositionHeight << ", instance cell " << settings.cellWidth << 'x'
                  << settings.cellHeight << ", instances " << settings.instances << "\n\n";

        {
            const runtime::CpuCompositionEvaluator evaluator;
            const auto plan = std::make_shared<const runtime::CompiledCompositionPlan>(
                expandedInstanceSet(settings, 1));
            std::cout << "preview budget " << settings.previewBudgetBytes / 1024 / 1024
                      << " MiB: " << refusalDetail(evaluator, plan, settings.previewBudgetBytes)
                      << "\n\n";

            const auto cold = evaluateOnce(evaluator, plan, settings.measurementBudgetBytes);
            report("expansion (cold)", cold);
            const auto warm = evaluateOnce(evaluator, plan, settings.measurementBudgetBytes);
            report("expansion (warm)", warm);

            // A new revision carrying one moved instance. The cache re-looks-up by resolved
            // content, so every unchanged instance is adopted into the new revision and only the
            // moved instance, the Merge and the Output are re-evaluated.
            const auto edited = std::make_shared<const runtime::CompiledCompositionPlan>(
                expandedInstanceSet(settings, 2, settings.instances / 2, 3.0));
            report("one instance moved",
                   evaluateOnce(evaluator, edited, settings.measurementBudgetBytes));
        }

        // The same instance count over a composition an eighth as tall. The per-instance resample
        // work is unchanged; only the Merge's row dispatch, which walks the merge window once per
        // entry, shrinks with it.
        {
            Settings shortFrame = settings;
            shortFrame.compositionHeight = static_cast<std::uint32_t>(settings.sweepHeight);
            const runtime::CpuCompositionEvaluator evaluator;
            const auto plan = std::make_shared<const runtime::CompiledCompositionPlan>(
                expandedInstanceSet(shortFrame, 1));
            report("expansion (short frame)",
                   evaluateOnce(evaluator, plan, settings.measurementBudgetBytes));
        }

        std::cout << '\n';
        for (const std::size_t count : {std::size_t{1}, std::size_t{50}, std::size_t{100},
                                        std::size_t{250}, std::size_t{500}, std::size_t{1000}}) {
            Settings sweep = settings;
            sweep.instances = count;
            const runtime::CpuCompositionEvaluator evaluator;
            const auto plan = std::make_shared<const runtime::CompiledCompositionPlan>(
                expandedInstanceSet(sweep, 1));
            report("sweep N=" + std::to_string(count),
                   evaluateOnce(evaluator, plan, settings.measurementBudgetBytes));
        }

        std::cout << '\n';
        for (const std::size_t attributes : {std::size_t{0}, settings.instances * 4}) {
            const runtime::CpuCompositionEvaluator evaluator;
            const auto plan = std::make_shared<const runtime::CompiledCompositionPlan>(
                perElementValueGraph(settings, attributes));
            report("value ops=" + std::to_string(attributes),
                   evaluateOnce(evaluator, plan, settings.measurementBudgetBytes));
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "instance0 spike failed: " << error.what() << '\n';
        return 1;
    }
}
