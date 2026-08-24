#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QToolButton;
class QTreeWidget;

namespace bloom::ui {

class CompositionSession;

class TimelineEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit TimelineEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    void rebuild();
    void updateSelection();
    void updateHistoryActions();

    CompositionSession& session_;
    QTreeWidget* layers_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
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
