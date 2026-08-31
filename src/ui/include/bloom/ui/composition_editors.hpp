#pragma once

#include <bloom/ui/playback_controller.hpp>

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QToolButton;
class QTreeWidget;

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;
class TimelineKeyframePanel;
class TimelineRuler;

class TimelineEditor final : public QWidget {
    Q_OBJECT

  public:
    TimelineEditor(CompositionSession& session, CompositionPreviewController& previewController,
                   QWidget* parent = nullptr);

  private:
    void rebuild();
    void updateSelection();
    void updateHistoryActions();
    // Reflects PlaybackController::stateChanged() onto the toggle button's text/tooltip/checked
    // state (design decision 4: "button/icon state reflects transport state via a signal").
    void updatePlaybackButton(PlaybackState state);

    CompositionSession& session_;
    TimelineRuler* ruler_ = nullptr;
    TimelineKeyframePanel* keyframes_ = nullptr;
    QTreeWidget* layers_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
    // Owned here rather than shared with any sibling TimelineEditor a workspace split could open
    // (issue #105): each TimelineEditor instance gets its own transport, matching how every other
    // per-panel affordance in this class (the layer tree selection sync, the undo/redo buttons'
    // enabled state) is already independently driven off the shared CompositionSession/
    // CompositionPreviewController rather than a single cross-panel singleton.
    PlaybackController* playback_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
    bool rebuilding_ = false;
};

class PropertiesEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit PropertiesEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    void rebuild();
    void configureOpacity();
    void configureSolidColor();

    CompositionSession& session_;
    QLabel* selectionLabel_ = nullptr;
    QDoubleSpinBox* positionX_ = nullptr;
    QDoubleSpinBox* positionY_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    QWidget* solidColorPanel_ = nullptr;
    QLabel* solidColorValue_ = nullptr;
    QLabel* solidAlphaAssociation_ = nullptr;
    QLabel* solidColorEncoding_ = nullptr;
    bool rebuilding_ = false;
};

class MediaEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit MediaEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    void rebuild();
    void updateSelection();

    CompositionSession& session_;
    QListWidget* compositions_ = nullptr;
    bool rebuilding_ = false;
};

} // namespace bloom::ui
