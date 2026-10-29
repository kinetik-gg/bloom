#pragma once

#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/document/ids.hpp>

#include <QObject>
#include <QString>

#include <array>
#include <optional>
#include <vector>

class QAction;
class QMenu;
class QWidget;

namespace bloom::ui {

class CompositionSession;
class CompositionPreviewController;

// ADR 0031: Align and Distribute for the selected layers. One set of actions serves the context
// bar's buttons and the Composition > Align submenu, so both are one command with one enabled
// state and one reason. Each run measures the frame the Viewer was delivered (metadata only, no
// UI-thread evaluation), builds the geometry with host::layerAlignmentGeometry and executes one
// commands::AlignLayers transaction: one undo step.
class LayerAlignmentActions final : public QObject {
    Q_OBJECT

  public:
    LayerAlignmentActions(CompositionSession& session, CompositionPreviewController* preview,
                          QObject* parent = nullptr);

    [[nodiscard]] QAction* action(commands::LayerArrangement arrangement) const;
    // The three Align To choices, exclusive and checkable; they write the session's
    // ToolController::alignmentTarget().
    [[nodiscard]] QAction* targetAction(commands::AlignmentTarget target) const;
    // A new "Align" menu holding the six aligns, the two distributes and the Align To choices.
    [[nodiscard]] QMenu* createMenu(QWidget* parent) const;

    // The layers an action would move, top of the stack first, and the key layer (the primary
    // selected layer) Key Object aligns to.
    [[nodiscard]] std::vector<document::LayerId> selectedLayers() const;
    [[nodiscard]] std::optional<document::LayerId> keyLayer() const;
    [[nodiscard]] QString keyLayerName() const;
    // Why `arrangement` cannot run now; empty when it can.
    [[nodiscard]] QString unavailableReason(commands::LayerArrangement arrangement) const;
    // Runs one arrangement. False when it is unavailable or the command was refused.
    bool run(commands::LayerArrangement arrangement);
    void refresh();

  signals:
    // A short status line: the layers that were skipped, and why.
    void notice(const QString& message);
    void changed();

  private:
    CompositionSession& session_;
    CompositionPreviewController* preview_;
    std::array<QAction*, 8> actions_{};
    std::array<QAction*, 3> targets_{};
};

[[nodiscard]] QString layerArrangementName(commands::LayerArrangement arrangement);

} // namespace bloom::ui
