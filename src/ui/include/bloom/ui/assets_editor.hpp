#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QSet>
#include <QWidget>

class QLineEdit;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace bloom::ui {

class CompositionSession;

class AssetsEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    explicit AssetsEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    EditorChromeSpec chrome_;
    void rebuild();
    void updateSelection();
    void refreshRowSelection();
    void applyFilter(const QString& text);
    void openComposition(document::CompositionId id);
    void commitRename(QTreeWidgetItem* item, int column);
    void showContextMenu(QPoint position);
    void duplicateComposition(document::CompositionId id);
    void deleteComposition(document::CompositionId id);
    void showNewCompositionDialog();
    void buildHeaderMenus();
    void createFolder();
    void beginRename(QTreeWidgetItem* item);
    void editTags(document::AssetId id);
    void removeSelected();
    void refreshDisclosure(QTreeWidgetItem* item);
    void buildFooter();

    CompositionSession& session_;
    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QWidget* headerMenuWidget_ = nullptr;
    QWidget* footerWidget_ = nullptr;
    bool rebuilding_ = false;
    QSet<qulonglong> collapsedFolders_;
    bool compositionsCollapsed_ = false;
};

} // namespace bloom::ui
