#pragma once

#include <QWidget>

class QListWidget;

namespace bloom::ui {

class CompositionSession;

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
