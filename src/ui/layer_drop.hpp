#pragma once
// Dropping footage and compositions onto the Timeline or the Viewer creates layers, as in After
// Effects. AddDroppedLayers (bloom/commands) is the one undoable drop; the drop target reads the
// drag, asks the host panel where the pointer is, shows its insertion indicator, and submits the
// operation (after an import, for files from the file manager).
#include <QPoint>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

class QObject;
class QMimeData;
class QString;

namespace bloom::document {
class Project;
}

namespace bloom::ui {
class CompositionSession;

using commands::AddDroppedLayers;
using commands::droppableAsLayer;
using commands::LayerDropItem;
using commands::LayerDropPlacement;

// The undo label for a drop of `items`: "Add Layer", "Add Composition Layer" or "Add 3 Layers".
[[nodiscard]] std::string layerDropLabel(const std::vector<LayerDropItem>& items);

// What a panel reports for a pointer position during a drag.
struct LayerDropHit final {
    LayerDropPlacement::Stack stack = LayerDropPlacement::Stack::Top;
    document::LayerSlotId before{};
    // nullopt: the playhead.
    std::optional<core::RationalTime> time;
    // The panel's own row for its insertion indicator; -1 for none.
    int row = -1;
};
struct LayerDropHooks final {
    // Where a drop at `position` (in the target's coordinates) lands; absent: on top, at the
    // playhead.
    std::function<LayerDropHit(QPoint position)> hit;
    // Shows the insertion indicator for a hit; nullopt hides it.
    std::function<void(const std::optional<LayerDropHit>&)> indicate;
};

// The items a drag from Assets carries, in selection order, that can become layers in the active
// composition. Empty for anything else, or for a stale or foreign drag.
[[nodiscard]] std::vector<LayerDropItem> layerDropItems(const QMimeData& mime,
                                                        const CompositionSession& session);

// What follows an import of files meant to become layers in `composition` (a file drop, or the
// Viewer's Add > Import and Add…): once the import commits, one undoable step adds a layer for each
// imported item that can be one, at `placement`, and selects them. Returns the status message.
[[nodiscard]] std::function<QString(const std::vector<document::AssetId>&)>
importedLayersFollowUp(CompositionSession& session, document::CompositionId composition,
                       LayerDropPlacement placement);

// Accepts Assets items and files on `target` (a widget or a window): items become layers at once;
// files are imported first (ADR 0029) and become layers in a second undoable step once the import
// commits. Alt at the drop sequences several items end to end.
void installLayerDropTarget(QObject& target, CompositionSession& session,
                            LayerDropHooks hooks = {});
} // namespace bloom::ui
