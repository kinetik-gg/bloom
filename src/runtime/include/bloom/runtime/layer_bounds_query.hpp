#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime {

class CpuCompositionEvaluator;
class SnapshotCompiler;

// The evaluated geometry of a composition's layers, without a delivered preview frame (ADR 0031).
//
// The interactive Viewer already has each layer's bounds in the frame it displays
// (CompositionPreviewController::currentLayerBounds). Headless callers -- scripting, MCP and
// bloom-cli -- get the same answer here by compiling the snapshot and evaluating one frame on the
// CPU reference path. Both read the Layer Output boundaries of the same plan through
// layerOutputBounds(), so the same snapshot and time give the same bounds.
//
// Heavy work: compile and evaluate. Call it from a worker or a script thread, never the UI thread.

// The Layer Output geometry of an evaluated frame: one entry per Layer Output boundary that has a
// layer id and non-empty output bounds, in plan order. `bounds` is indexed like the plan's
// operations (ProcessFrame::evaluatedBounds, PreparedPreviewFrame::evaluatedBounds).
[[nodiscard]] std::vector<EvaluatedOperationBounds>
layerOutputBounds(const CompiledCompositionPlan& plan,
                  std::span<const EvaluatedOperationBounds> bounds);

// A default per-query pixel budget large enough for an ordinary UHD composition's live images.
inline constexpr std::size_t kDefaultLayerBoundsPixelBudget = std::size_t{2} * 1024 * 1024 * 1024;

struct LayerBoundsQueryRequest final {
    document::Snapshot snapshot;
    document::CompositionId composition;
    core::RationalTime time;
    // The layers to report; empty reports every layer with visible bounds.
    std::vector<document::LayerId> layers;
    EvaluationColorIntent colorIntent = EvaluationColorIntent::LinearRec709Scene;
    std::size_t pixelStorageByteLimit = kDefaultLayerBoundsPixelBudget;
};

enum class LayerBoundsQueryStatus : std::uint8_t {
    Evaluated,
    // The composition does not compile (an unsupported graph); `diagnostic` says why.
    Unsupported,
    Cancelled,
    Failed,
};

struct LayerBoundsQueryResult final {
    LayerBoundsQueryStatus status = LayerBoundsQueryStatus::Failed;
    // The requested layers that have visible bounds at the time, in plan order. A requested layer
    // missing here is outside its range, disabled, or empty at that time.
    std::vector<EvaluatedOperationBounds> bounds;
    std::string diagnostic;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == LayerBoundsQueryStatus::Evaluated;
    }
};

// Compiles `request.snapshot` and evaluates `request.composition` at `request.time` with the
// reference evaluator (full composition resolution, look nodes included, media revalidated), then
// returns the requested layers' Layer Output bounds. The evaluator's asset base directory and
// caches are the caller's.
[[nodiscard]] LayerBoundsQueryResult queryLayerBounds(const SnapshotCompiler& compiler,
                                                      const CpuCompositionEvaluator& evaluator,
                                                      const LayerBoundsQueryRequest& request,
                                                      const CancellationToken& cancellation = {});

} // namespace bloom::runtime
