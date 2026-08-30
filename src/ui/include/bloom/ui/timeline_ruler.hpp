#pragma once

#include <bloom/document/ids.hpp>

#include <QWidget>

#include <optional>
#include <vector>

class QVBoxLayout;

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;

// The scrub ruler above TimelineEditor's layer tree (docs/architecture/animation-and-time.md,
// "Session Time And Scrubbing"): paints frame ticks and the session's exact-time playhead, and
// turns click/drag into session.setCurrentTime() calls that request the preview at Interactive
// priority through the controller's trailing cadence (CompositionPreviewController::
// beginInteractiveScrub()/notifyScrubEnded()). Projection and scrub only: no direct Viewer
// manipulation, no playback transport, no key-editing gestures.
class TimelineRuler final : public QWidget {
    Q_OBJECT

  public:
    TimelineRuler(CompositionSession& session, CompositionPreviewController& previewController,
                  QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void scrubToPixel(int pixelX);

    CompositionSession& session_;
    CompositionPreviewController& previewController_;
    bool scrubbing_ = false;
};

// One row per animated parameter of the current selection's layer (position/opacity only in
// version 1 -- docs/architecture/animation-and-time.md, "Durable Type Model"): a name label plus a
// key lane painting each key at its exact time. Clicking a key selects it.
//
// Selection-model finding: bloom::ui::CompositionSelection (composition_session.hpp) has no
// keyframe concept -- its SelectionTarget variant covers only LayerId/NodeId/ParameterId. Per this
// task's decision 3, key selection is therefore kept as TimelineEditor-local state here, keyed by
// the durable KeyframeId, and survives a rebuild (curve edit, undo/redo) when the key still exists.
// Unifying it into one primary/contextual selection truth across editors is left to the
// direct-manipulation slice that consumes it.
class TimelineKeyframePanel final : public QWidget {
    Q_OBJECT

  public:
    explicit TimelineKeyframePanel(CompositionSession& session, QWidget* parent = nullptr);

    [[nodiscard]] std::optional<document::KeyframeId> selectedKeyframe() const noexcept {
        return selectedKeyframe_;
    }

  private:
    void rebuild();
    void handleKeySelected(document::KeyframeId id);
    void pruneSelectionIfMissing();

    CompositionSession& session_;
    QVBoxLayout* rowsLayout_ = nullptr;
    std::vector<class TimelineKeyframeRow*> rows_;
    std::optional<document::KeyframeId> selectedKeyframe_;
};

} // namespace bloom::ui
