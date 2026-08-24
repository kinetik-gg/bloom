#pragma once

#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QWidget>

class QWheelEvent;

namespace bloom::ui {

class CompositionSession;

inline constexpr int kNodeItemKindRole = Qt::UserRole + 1;
inline constexpr int kNodeStableIdRole = Qt::UserRole + 2;

class NodeGraphicsScene final : public QGraphicsScene {
    Q_OBJECT

  public:
    explicit NodeGraphicsScene(QObject* parent = nullptr);

    void setProjection(const document::Snapshot& snapshot, document::CompositionId compositionId);
    [[nodiscard]] QGraphicsItem* findNodeItem(document::NodeId nodeId) const;

  private:
    void rebuildEdges(const document::Composition& composition);
};

class NodeGraphicsView final : public QGraphicsView {
    Q_OBJECT

  public:
    explicit NodeGraphicsView(QWidget* parent = nullptr);

  protected:
    void wheelEvent(QWheelEvent* event) override;
};

class NodeGraphEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit NodeGraphEditor(CompositionSession& session, QWidget* parent = nullptr);
    ~NodeGraphEditor() override;

    [[nodiscard]] NodeGraphicsScene* graphScene() const noexcept;
    [[nodiscard]] NodeGraphicsView* graphView() const noexcept;

  private:
    void rebuild();
    void updateSelection();
    void sceneSelectionChanged();

    CompositionSession& session_;
    NodeGraphicsScene* scene_ = nullptr;
    NodeGraphicsView* view_ = nullptr;
    bool rebuilding_ = false;
};

} // namespace bloom::ui
