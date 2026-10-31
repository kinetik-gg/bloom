#pragma once

// The Timeline's reads and writes of the two document 1.30 layer fields the layer table needs:
// Shy (presentation only; never part of rendering or cache identity) and "name follows source" (a
// layer made from an asset or a composition shows that source's name until the artist renames
// the layer, as in After Effects). The one place the Timeline touches either field.

#include <bloom/document/ids.hpp>

#include <QString>

#include <vector>

namespace bloom::ui {
class CompositionSession;

namespace layer_fields {

[[nodiscard]] bool shy(const CompositionSession& session, document::LayerId layer);
// One undoable Set Layer Shy step for every listed layer that differs. False when nothing changed.
bool setShy(CompositionSession& session, const std::vector<document::LayerId>& layers, bool shy);

// The name of the asset or composition the layer is made from, or an empty string for a layer
// without one (a solid, text or shape layer).
[[nodiscard]] QString sourceName(const CompositionSession& session, document::LayerId layer);
// Whether the layer shows its source's name: true until the artist renames the layer.
[[nodiscard]] bool nameFollowsSource(const CompositionSession& session, document::LayerId layer);
// What the layer table, the rename field and every Timeline surface show as the layer's name.
[[nodiscard]] QString displayName(const CompositionSession& session, document::LayerId layer);

} // namespace layer_fields
} // namespace bloom::ui
