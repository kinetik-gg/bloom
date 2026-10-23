#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QSet>
#include <QWidget>
#include <cstdint>

class QLineEdit;
class QButtonGroup;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace bloom::ui {

class CompositionSession;

class AssetsEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    // Work counter: complete row rebuilds since construction.
    [[nodiscard]] std::uint64_t rowRebuildCount() const noexcept { return rowRebuilds_; }
    explicit AssetsEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    EditorChromeSpec chrome_;
    void rebuild();
    // Rebuilds rows only when the asset/folder/composition records or their asset-status
    // projection changed; playback, freshness checks and unrelated edits keep the existing rows.
    void refreshRows();
    [[nodiscard]] QString rowSignature() const;
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
    void editInputColor(document::AssetId id);
    void removeSelected();
    void refreshDisclosure(QTreeWidgetItem* item);
    void buildFooter();
    void setFilter(int filter);

    CompositionSession& session_;
    QLineEdit* search_ = nullptr;
    QButtonGroup* filterButtons_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QWidget* headerMenuWidget_ = nullptr;
    QWidget* footerWidget_ = nullptr;
    bool rebuilding_ = false;
    QString rowSignature_;
    std::uint64_t rowRebuilds_ = 0;
    QSet<qulonglong> collapsedFolders_;
    bool compositionsCollapsed_ = false;
    // -1 preserves the pre-Inspector view: media and compositions are visible together. The
    // Data choice opts into the separate data-root surface.
    int filterMode_ = -1;
};

} // namespace bloom::ui
