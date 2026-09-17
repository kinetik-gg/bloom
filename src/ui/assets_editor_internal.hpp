#pragma once
#include <QString>
#include <QTreeWidget>
#include <bloom/document/ids.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::ui {
class CompositionSession;
namespace assets {
inline constexpr int kCompositionRole = Qt::UserRole + 1;
inline constexpr int kAssetRole = Qt::UserRole + 2;
inline constexpr int kFolderRole = Qt::UserRole + 3;
inline constexpr int kTagsRole = Qt::UserRole + 4;
inline constexpr int kCompositionRootRole = Qt::UserRole + 5;
inline constexpr auto kInternalMimeType = "application/x-bloom-asset-list";
inline document::CompositionId compositionId(const QTreeWidgetItem* item) {
    return document::CompositionId::fromRaw(item ? item->data(0, kCompositionRole).toULongLong()
                                                 : 0);
}
inline document::AssetId assetId(const QTreeWidgetItem* item) {
    return document::AssetId::fromRaw(item ? item->data(0, kAssetRole).toULongLong() : 0);
}
inline std::optional<document::AssetFolderId> folderId(const QTreeWidgetItem* item) {
    const auto id = item ? item->data(0, kFolderRole).toULongLong() : 0;
    return id ? std::optional(document::AssetFolderId::fromRaw(id)) : std::nullopt;
}
class AssetTree final : public QTreeWidget {
  public:
    AssetTree(CompositionSession& session, QWidget* parent);
    void beginRename(QTreeWidgetItem* item);
    // Test seam: overrides the GVFS root a dropped `smb://` URL resolves against (default:
    // bloom::ui::defaultGvfsRoot()), so an offscreen test can drive a drop through a fake mount
    // directory instead of this machine's real one.
    void setGvfsRootForTest(QString root) { gvfsRoot_ = std::move(root); }

  protected:
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void startDrag(Qt::DropActions actions) override;
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    // The row's kit disclosure owns the glyph; Qt still owns indentation and keyboard navigation.
    void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}

  private:
    std::vector<document::AssetId> internalAssets(const QMimeData& mime) const;
    CompositionSession& session_;
    QByteArray token_;
    QString gvfsRoot_;
};
} // namespace assets
} // namespace bloom::ui
