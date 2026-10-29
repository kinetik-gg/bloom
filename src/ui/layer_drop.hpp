#pragma once
// Dropping footage and compositions onto the Timeline or the Viewer creates layers, as in After
// Effects. The operation below is the one undoable drop; the drop target reads the drag, asks the
// host panel where the pointer is, shows its insertion indicator, and submits the operation (after
// an import, for files from the file manager).
#include <QPoint>
#include <bloom/commands/operation.hpp>
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

using LayerDropItem = std::variant<document::AssetId, document::CompositionId>;

// Where dropped items join the composition.
struct LayerDropPlacement final {
    enum class Stack { Top, Before, Bottom };
    Stack stack = Stack::Top;
    // For Before: the new layers go directly above this layer.
    document::LayerSlotId before{};
    // The first layer's in point, on the composition frame grid.
    core::RationalTime start{};
    // End to end (After Effects' Sequence Layers): each layer starts where the one before ends.
    bool sequence = false;

    friend bool operator==(const LayerDropPlacement&, const LayerDropPlacement&) = default;
};

// Footage (image, sequence, video, audio) and compositions other than `target` become layers;
// fonts, LUTs and the target itself do not.
[[nodiscard]] bool droppableAsLayer(const document::Project& project,
                                    document::CompositionId target, const LayerDropItem& item);

// Adds one layer per item, in order, through the ordinary add-layer command for its kind
// (AddImageLayer, AddAudioLayer, or a nested composition layer), so each layer keeps its media's
// own length (ADR 0024). The first item is uppermost; the layers start at the placement time, or
// end to end when sequenced, through the Timeline's own move math. Outputs one "layer" per item.
class AddDroppedLayers final : public commands::Operation {
  public:
    AddDroppedLayers(document::CompositionId composition, std::vector<LayerDropItem> items,
                     LayerDropPlacement placement)
        : composition_(composition), items_(std::move(items)), placement_(placement) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.layer.add-dropped";
    }
    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override;
    // Only the target composition's graph changes (and so whatever renders it).
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return composition_;
    }

  private:
    document::CompositionId composition_;
    std::vector<LayerDropItem> items_;
    LayerDropPlacement placement_;
};

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
