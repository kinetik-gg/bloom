#ifndef BLOOM_RUNTIME_GPU_SCENE_NESTED_HPP
#define BLOOM_RUNTIME_GPU_SCENE_NESTED_HPP

// Private to src/runtime. The CompiledCompositionSource preparation for the shared builder.
//
// A nested composition is never CPU-evaluated into a finished frame and uploaded: this helper
// recursively builds the CHILD plan with the SAME production CpuGpuSceneBuilder and SPLICES the
// child's genuine GPU commands into the parent's command list, remapping every internal command
// reference by a constant base. The parent's composition-source operation then simply aliases the
// child's terminal Composition Output command, so the executor walks the child subtree as ordinary
// commands and every child branch keeps its own content-addressed semantic key. An unrelated child
// edit therefore still hits the cache for the untouched branches.
//
// The child request mirrors the CPU evaluator exactly: mapped time, child output, no ROI, the
// parent's remaining byte allowance, and (under a proxy parent) a child proxy extent derived from
// the same parent scales. Bounds, output window, descriptor and semantic identity all come from the
// child scene the production builder produced, so the CPU reference evaluator stays the oracle.
//
// Cycles and runaway nesting are bounded by an explicit depth limit; cancellation is checked before
// and during the splice. A child that fails closed propagates its diagnostic unchanged.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "gpu_scene_preparation_common.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/composition_time.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

// A depth ceiling, not a semantic limit: a document with deeper genuine nesting than this fails
// closed rather than recursing without bound. It also breaks a hand-built A->B->A cycle, which the
// CPU evaluator would otherwise recurse into forever.
inline constexpr std::size_t kMaxNestedCompositionDepth = 16;

// The nesting depth of the build currently executing on this thread. Incremented around the child
// build so a grandchild's dispatch observes the correct depth without widening the private
// buildImpl signature (which is shared with the concurrent ordinary-handler work).
[[nodiscard]] inline std::size_t& nestedCompositionDepth() noexcept {
    static thread_local std::size_t depth = 0;
    return depth;
}

class NestedCompositionDepthScope final {
  public:
    NestedCompositionDepthScope() noexcept { ++nestedCompositionDepth(); }
    ~NestedCompositionDepthScope() { --nestedCompositionDepth(); }
    NestedCompositionDepthScope(const NestedCompositionDepthScope&) = delete;
    NestedCompositionDepthScope& operator=(const NestedCompositionDepthScope&) = delete;
    NestedCompositionDepthScope(NestedCompositionDepthScope&&) = delete;
    NestedCompositionDepthScope& operator=(NestedCompositionDepthScope&&) = delete;
};

// True when a composition source names a present, compatible child plan: the exact conditions the
// CPU preflight's hasExpectedInputKinds enforces. A source that fails this is not a supported
// nested composition and remains OUTSIDE the prepared subset, which is why the builder reports it
// as Unsupported before any resolution, preserving the pre-nested refusal for a malformed source.
[[nodiscard]] inline bool
nestedCompositionReferenceReady(const CompiledCompositionSource& source,
                                const CompiledCompositionPlan& plan) noexcept {
    if (source.nestedPlanIndex >= plan.nestedPlans().size())
        return false;
    const auto& nested = plan.nestedPlans()[source.nestedPlanIndex];
    return nested != nullptr && nested->projectId() == plan.projectId() &&
           nested->compositionId() != plan.compositionId() &&
           nested->sourceRevision() == plan.sourceRevision() &&
           nested->duration() > core::RationalTime{} && source.timeMapping.loopMode >= 0 &&
           source.timeMapping.loopMode <= 2;
}

// The full child-classification check the SHARED screen uses before a composition source is
// admitted: every nested reference is present and compatible, no composition identity repeats along
// the ancestor chain (a cycle), and the chain stays within the depth ceiling. It walks the REAL
// nested plans, never a fabricated one. It does not screen the child's own operation kinds -- the
// child build does that against the same shared screen -- so a valid chain that later contains an
// unsupported child operation still fails closed when that child is built.
[[nodiscard]] bool nestedCompositionChainIsSupported(const CompiledCompositionSource& source,
                                                     const CompiledCompositionPlan& plan);

struct GpuSceneNestedResult final {
    bool prepared = false;
    GpuSceneCommandIndex outputCommand = kInvalidGpuSceneCommand;
    std::string outputKey;
    EvaluatedOperationBounds bounds;
    // ImageWindow has no default constructor, so this aggregate holds the child's output window in
    // an optional exactly like the media upload leaf result does.
    std::optional<render::ImageWindow> outputWindow;
    std::uint64_t residentBytes = 0;
};

// Adds `base` to every command index a copied child command references. Leaf commands reference
// nothing, so they are left alone.
inline void offsetNestedCommandReferences(GpuSceneCommand& command,
                                          const GpuSceneCommandIndex base) {
    std::visit(
        [base](auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, GpuSceneTranslationCommand> ||
                          std::is_same_v<T, GpuSceneAffineCommand>) {
                if (item.input != kInvalidGpuSceneCommand)
                    item.input = static_cast<GpuSceneCommandIndex>(item.input + base);
            } else if constexpr (std::is_same_v<T, GpuSceneBlendCommand>) {
                if (item.source != kInvalidGpuSceneCommand)
                    item.source = static_cast<GpuSceneCommandIndex>(item.source + base);
                if (item.destination != kInvalidGpuSceneCommand)
                    item.destination = static_cast<GpuSceneCommandIndex>(item.destination + base);
            } else if constexpr (std::is_same_v<T, GpuSceneMergeCommand>) {
                for (auto& foreground : item.foregrounds) {
                    if (foreground != kInvalidGpuSceneCommand)
                        foreground = static_cast<GpuSceneCommandIndex>(foreground + base);
                }
            } else if constexpr (std::is_same_v<T, GpuSceneCompositionOutputCommand>) {
                if (item.input != kInvalidGpuSceneCommand)
                    item.input = static_cast<GpuSceneCommandIndex>(item.input + base);
            }
        },
        command);
}

[[nodiscard]] inline const std::string& commandSemanticKey(const GpuSceneCommand& command) {
    return std::visit([](const auto& item) -> const std::string& { return item.semanticKey; },
                      command);
}

namespace nested_detail {

inline void addWindowBytes(std::uint64_t& total, const render::ImageWindow window) noexcept {
    const std::uint64_t width = window.extent().width();
    const std::uint64_t height = window.extent().height();
    if (width == 0 || height == 0)
        return;
    const std::uint64_t pixels = width * height;
    const std::uint64_t bytes = pixels * sizeof(render::Rgba32f);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    total = bytes > maximum - total ? maximum : total + bytes;
}

} // namespace nested_detail

// Retained host-byte total for a spliced child scene: the frozen upload images plus each unique
// coverage raster once. It deliberately does NOT sum the RGBA32F command outputs, which are
// allocated at executor time under the executor's own live-pin budget; counting their mutually
// exclusive lifetimes here would re-introduce the artificial per-frame refusal the builder fixed.
[[nodiscard]] inline std::uint64_t nestedSceneResidentBytes(const PreparedGpuScene& scene) {
    std::uint64_t total = 0;
    std::unordered_set<const void*> countedCoverage;
    for (const auto& command : scene.commands()) {
        std::visit(
            [&total, &countedCoverage](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, GpuSceneCoverageSolidCommand>) {
                    if (item.coverage != nullptr &&
                        countedCoverage.insert(item.coverage.get()).second) {
                        const auto maximum = std::numeric_limits<std::uint64_t>::max();
                        const std::uint64_t bytes = item.coverage->size();
                        total = bytes > maximum - total ? maximum : total + bytes;
                    }
                } else if constexpr (std::is_same_v<T, GpuSceneUploadCommand>) {
                    nested_detail::addWindowBytes(total, item.descriptor.dataWindow());
                }
            },
            command);
    }
    return total;
}

// Builds the child scene through the production builder and splices its command list into
// `commands` at a constant base offset. `buildChild` is the production `CpuGpuSceneBuilder::build`
// entry (the public one), so the child's plan validation, resolution and command emission are all
// genuine.
template <typename BuildChild>
[[nodiscard]] std::optional<GpuSceneLeafFailure> prepareNestedComposition(
    const CompiledCompositionSource& source, const CompiledCompositionPlan& parentPlan,
    const EvaluationRequest& parentRequest, const ResolvedEvaluation& parentResolved,
    const double parentHorizontalScale, const double parentVerticalScale, const std::size_t depth,
    const std::uint64_t remainingByteBudget, const CancellationToken& cancellation,
    BuildChild&& buildChild, std::vector<GpuSceneCommand>& commands, GpuSceneNestedResult& result) {
    if (depth >= kMaxNestedCompositionDepth) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Nested composition depth exceeds the supported limit");
    }
    if (source.nestedPlanIndex >= parentPlan.nestedPlans().size()) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Composition source references a missing nested plan");
    }
    const auto& nested = parentPlan.nestedPlans()[source.nestedPlanIndex];
    if (nested == nullptr) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Composition source references a null nested plan");
    }
    if (nested->projectId() != parentPlan.projectId() ||
        nested->compositionId() == parentPlan.compositionId() ||
        nested->sourceRevision() != parentPlan.sourceRevision() ||
        !(nested->duration() > core::RationalTime{})) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Nested composition is incompatible with its parent");
    }
    if (cancellation.isCancellationRequested()) {
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
    }

    const auto& mapping = source.timeMapping;
    if (mapping.loopMode < 0 || mapping.loopMode > 2) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Composition source time mapping has an invalid loop mode");
    }
    const auto offset = resolveParameter(mapping.offset, parentPlan, parentResolved);
    const auto scale = resolveParameter(mapping.scale, parentPlan, parentResolved);
    if (!offset || !scale) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Composition source time mapping is not evaluable");
    }
    const auto nestedTime =
        mapCompositionTime(parentRequest.time, offset->value, scale->value, nested->duration(),
                           nested->format().frameRate(), mapping.loopMode);
    if (!nestedTime) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Composition source time cannot be represented");
    }

    EvaluationRequest nestedRequest = parentRequest;
    nestedRequest.time = *nestedTime;
    nestedRequest.output = nested->output();
    nestedRequest.roi.reset();
    nestedRequest.pixelStorageByteLimit = remainingByteBudget;
    if (std::holds_alternative<ProxyResolution>(parentRequest.resolution)) {
        const auto extent = render::ImageExtent::create(
            static_cast<std::uint64_t>(
                std::max(1.0, std::ceil(nested->format().width() * parentHorizontalScale))),
            static_cast<std::uint64_t>(
                std::max(1.0, std::ceil(nested->format().height() * parentVerticalScale))));
        if (!extent) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Nested composition proxy extent is invalid");
        }
        nestedRequest.resolution = ProxyResolution{*extent.value()};
    }

    PreparedGpuSceneBuildResult childResult;
    {
        NestedCompositionDepthScope scope;
        childResult = buildChild(nested, nestedRequest, cancellation);
    }
    if (!childResult) {
        if (childResult.diagnostic.code == PreparedGpuSceneDiagnosticCode::Cancelled ||
            cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        const auto code = childResult.diagnostic.code == PreparedGpuSceneDiagnosticCode::None
                              ? PreparedGpuSceneDiagnosticCode::InvalidPlan
                              : childResult.diagnostic.code;
        return fail(code, childResult.diagnostic.message);
    }
    const auto& childScene = *childResult.scene;
    const auto childOutput = childScene.outputCommand();
    if (childOutput == kInvalidGpuSceneCommand ||
        static_cast<std::size_t>(childOutput) >= childScene.commands().size()) {
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Nested composition published no terminal output command");
    }
    if (nested->output().value() >= childScene.bounds().size()) {
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Nested composition published no output bounds");
    }

    const auto base = static_cast<GpuSceneCommandIndex>(commands.size());
    const auto childCount = static_cast<GpuSceneCommandIndex>(childScene.commands().size());
    if (childCount > std::numeric_limits<GpuSceneCommandIndex>::max() - base) {
        return fail(PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                    "Nested composition command list overflows");
    }
    for (const auto& childCommand : childScene.commands()) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        GpuSceneCommand copy = childCommand;
        std::visit(
            [base](auto& item) {
                item.index = static_cast<GpuSceneCommandIndex>(item.index + base);
            },
            copy);
        offsetNestedCommandReferences(copy, base);
        commands.push_back(std::move(copy));
    }

    result.prepared = true;
    result.outputCommand = static_cast<GpuSceneCommandIndex>(childOutput + base);
    result.outputKey = commandSemanticKey(childScene.commands()[childOutput]);
    result.bounds = childScene.bounds()[nested->output().value()];
    // The CPU evaluator carries a Composition Source's output bounds in BOTH fields.
    result.bounds.local = result.bounds.output;
    result.outputWindow = childScene.outputDescriptor().dataWindow();
    result.residentBytes = nestedSceneResidentBytes(childScene);
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_NESTED_HPP
