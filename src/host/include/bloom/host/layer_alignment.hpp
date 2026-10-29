#pragma once

#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// ADR 0031: the bridge from evaluated layer geometry to the align/distribute command. The command
// itself (commands::AlignLayers) never evaluates or samples; these Qt-free helpers turn evaluated
// bounds into its LayerAlignmentGeometry.
//
// - Interactive: the Viewer passes its delivered frame's CompositionPreviewController::
//   currentLayerBounds() to layerAlignmentGeometry(). Metadata only; no evaluation.
// - Headless: scripting, MCP and bloom-cli call measureLayerAlignmentGeometry(), which runs the
//   CPU reference evaluation (runtime::queryLayerBounds) and then the same
//   layerAlignmentGeometry().
namespace bloom::host {

// One LayerAlignmentGeometry per distinct layer in `bounds` (the first entry wins), in order.
// Position is sampled at each layer's own key time for `time`; the parent's linear transform is
// recovered from the layer's evaluated polygon and its own sampled scale and rotation, or from the
// parent's evaluated polygon when the layer's own transform is not sampleable (driven). A layer
// whose parent transform cannot be recovered gets a singular parentLinear, which the command skips
// with a reason.
[[nodiscard]] std::vector<commands::LayerAlignmentGeometry>
layerAlignmentGeometry(const document::Composition& composition, core::RationalTime time,
                       std::span<const runtime::EvaluatedOperationBounds> bounds);

struct LayerAlignmentMeasureRequest final {
    document::Snapshot snapshot;
    document::CompositionId composition;
    core::RationalTime time;
    std::filesystem::path assetBaseDirectory;
    runtime::EvaluationColorIntent colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
    std::size_t pixelStorageByteLimit = runtime::kDefaultLayerBoundsPixelBudget;
};

struct LayerAlignmentGeometryResult final {
    bool succeeded = false;
    // Every layer with visible bounds at the time.
    std::vector<commands::LayerAlignmentGeometry> geometry;
    std::string diagnostic;
};

// Headless measurement: compile and evaluate one frame on the CPU reference path, then
// layerAlignmentGeometry(). Heavy; never call it on the UI thread.
[[nodiscard]] LayerAlignmentGeometryResult
measureLayerAlignmentGeometry(const LayerAlignmentMeasureRequest& request,
                              const runtime::CancellationToken& cancellation = {});

} // namespace bloom::host
