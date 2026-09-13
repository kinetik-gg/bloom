#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QWidget>

class QLineEdit;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace bloom::ui {

class CompositionSession;

class AssetsEditor final : public QWidget,
                           public EditorHeaderMenuProvider,
                           public EditorFooterProvider {
    Q_OBJECT

  public:
    explicit AssetsEditor(CompositionSession& session, QWidget* parent = nullptr);

    [[nodiscard]] QWidget* takeHeaderMenuWidget() override;
    [[nodiscard]] QWidget* takeFooterWidget() override;

  private:
    void rebuild();
    void updateSelection();
    void applyFilter(const QString& text);
    void openComposition(document::CompositionId id);
    void commitRename(QTreeWidgetItem* item, int column);
    void showContextMenu(QPoint position);
    void duplicateComposition(document::CompositionId id);
    void deleteComposition(document::CompositionId id);
    void showNewCompositionDialog();
    void buildHeaderMenus();
    void buildFooter();

    CompositionSession& session_;
    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QWidget* headerMenuWidget_ = nullptr;
    QWidget* footerWidget_ = nullptr;
    bool headerMenuWidgetTaken_ = false;
    bool footerWidgetTaken_ = false;
    bool rebuilding_ = false;
};

} // namespace bloom::ui
