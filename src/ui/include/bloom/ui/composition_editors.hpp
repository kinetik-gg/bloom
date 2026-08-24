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
    QToolButton* addTextButton_ = nullptr;
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

    CompositionSession& session_;
    QLabel* selectionLabel_ = nullptr;
    QDoubleSpinBox* positionX_ = nullptr;
    QDoubleSpinBox* positionY_ = nullptr;
    QDoubleSpinBox* opacity_ = nullptr;
    bool rebuilding_ = false;
};

class ViewerEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit ViewerEditor(CompositionSession& session, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    CompositionSession& session_;
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
