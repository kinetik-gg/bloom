#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QSet>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <vector>

class QLineEdit;
class QMenu;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace bloom::ui {

namespace kit {
class KLabel;
class KThumbnail;
} // namespace kit
class CompositionSession;

// The project's assets, compositions and folders as one sortable tree with columns, a search
// field and a selection header, after the After Effects Project panel (#378).
class AssetsEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    // Work counter: complete row rebuilds since construction.
    [[nodiscard]] std::uint64_t rowRebuildCount() const noexcept { return rowRebuilds_; }
    explicit AssetsEditor(CompositionSession& session, QWidget* parent = nullptr);
    ~AssetsEditor() override;

  private:
    EditorChromeSpec chrome_;
    void rebuild();
    // Rebuilds rows only when the asset/folder/composition records or their asset-status
    // projection changed; playback, freshness checks and unrelated edits keep the existing rows.
    void refreshRows();
    [[nodiscard]] QString rowSignature() const;
    void updateSelection();
    void selectionChanged();
    void applyFilter(const QString& text);
    void openComposition(document::CompositionId id);
    void commitRename(QTreeWidgetItem* item, int column);
    void showContextMenu(QPoint position);
    void addCommonActions(QMenu& menu);
    void showColumnMenu(QPoint globalPosition);
    void fillColumnMenu(QMenu& menu);
    void setColumnVisible(int column, bool visible);
    void saveHeaderState() const;
    void restoreHeaderState();
    void duplicateComposition(document::CompositionId id);
    void showNewCompositionDialog();
    void newCompositionFromSelection();
    void buildHeaderMenus();
    void buildFooter();
    void buildSelectionHeader();
    void refreshSelectionHeader();
    void createFolder();
    void beginRename(QTreeWidgetItem* item);
    void editTags(document::AssetId id);
    void editInputColor(document::AssetId id);
    void removeSelected();
    void revealInFileManager(document::AssetId id);
    void requestImport();
    // The folder new folders, compositions and imports land in for the current selection.
    [[nodiscard]] std::optional<document::AssetFolderId> selectedFolder() const;
    [[nodiscard]] std::vector<document::AssetId> selectedAssets() const;

    CompositionSession& session_;
    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QWidget* headerMenuWidget_ = nullptr;
    QWidget* footerWidget_ = nullptr;
    QWidget* selectionHeader_ = nullptr;
    kit::KThumbnail* thumbnail_ = nullptr;
    kit::KLabel* selectionName_ = nullptr;
    std::vector<kit::KLabel*> selectionFacts_;
    QMenu* columnMenu_ = nullptr;
    bool rebuilding_ = false;
    QString rowSignature_;
    std::uint64_t rowRebuilds_ = 0;
    QSet<qulonglong> collapsedFolders_;
    bool dataCollapsed_ = false;
};

} // namespace bloom::ui
