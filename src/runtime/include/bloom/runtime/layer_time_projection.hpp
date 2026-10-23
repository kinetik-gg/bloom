#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/composition_time.hpp>

#include <optional>

// ADR 0024 layer-owned time, Qt-free projections shared by the compiler and the editors: the
// natural length a Source content extent resolves to, and the exact inverse of a layer's linear
// clock, so the Timeline can place keys and draw the media extent in composition time.
namespace bloom::runtime {

// The natural content length of a media or composition source node: a sequence's member span at
// the composition rate, a video's frame count times its stream period, an audio asset's duration,
// or a referenced composition's duration and rate. nullopt when the node has no natural length (a
// still image, a missing asset) or applies its own non-identity timing (a start frame, or a
// composition source with an animated or non-identity offset/scale).
[[nodiscard]] std::optional<TimeExtent>
resolveSourceExtent(const document::Project& project, const document::Composition& composition,
                    document::NodeId sourceNodeId);

// The layer's resolved content extent, or nullopt for Unbounded (or an unresolvable Source).
[[nodiscard]] std::optional<TimeExtent>
resolveLayerExtent(const document::Project& project, const document::Composition& composition,
                   const document::LayerTiming& timing);

// Placement time of layer time tau: startTime + tau / speed, exactly (nullopt on overflow).
[[nodiscard]] std::optional<core::RationalTime>
placementTimeOfLayerTime(const document::LayerTiming& timing, core::RationalTime layerTime);

} // namespace bloom::runtime
