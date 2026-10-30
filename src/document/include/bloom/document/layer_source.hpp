#pragma once

// The source a layer was made from: the asset an image, video or audio source node plays, or the
// composition a composition source nests. A layer's source is the node feeding its Layer Output's
// content ("image") input, else its "audio" input. Solids, text, shapes and any other generated
// content have no source.

#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>

#include <optional>
#include <string>
#include <variant>

namespace bloom::document {

class Composition;
class Project;

using LayerSource = std::variant<AssetId, CompositionId>;

[[nodiscard]] std::optional<LayerSource> layerSource(const Composition& composition,
                                                     const LayerOutputBoundary& layer);

// The source's current name, or nullopt when the layer has no source or the source is missing.
[[nodiscard]] std::optional<std::string> layerSourceName(const Project& project,
                                                         const Composition& composition,
                                                         const LayerOutputBoundary& layer);

// Re-applies every following layer's source name, project-wide or in one composition. Returns true
// when a name changed. Commands that rename or replace a source call it so following layers keep
// showing their source.
bool refreshLayerNamesFromSources(Project& project,
                                  std::optional<CompositionId> only = std::nullopt);

// Document 1.30 migration rule for a project saved before the field existed: a layer follows its
// source exactly when its current name already IS its source's name, so no visible name changes.
void inferLayerNameFollowsSource(Project& project);

} // namespace bloom::document
