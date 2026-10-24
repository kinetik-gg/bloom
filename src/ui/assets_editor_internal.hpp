#pragma once
#include <QPoint>
#include <QString>
#include <QTreeWidget>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::document {
class Project;
struct AssetRecord;
} // namespace bloom::document

namespace bloom::ui {
class CompositionSession;
namespace assets {
inline constexpr int kCompositionRole = Qt::UserRole + 1;
inline constexpr int kAssetRole = Qt::UserRole + 2;
inline constexpr int kFolderRole = Qt::UserRole + 3;
inline constexpr int kTagsRole = Qt::UserRole + 4;
inline constexpr int kDataBlockRole = Qt::UserRole + 6;
// Per column: the value a column sorts by when its text would sort wrongly (sizes, times, rates).
inline constexpr int kSortRole = Qt::UserRole + 7;
// The row's status for tests and accessibility: "missing" (the warning glyph), "colourNote" (the
// assumed-colour Info glyph) or empty.
inline constexpr int kStateRole = Qt::UserRole + 8;
inline constexpr auto kInternalMimeType = "application/x-bloom-asset-list";
// Per-user column visibility, widths, order and sort (never project data).
inline constexpr auto kHeaderStateKey = "assets/header-state";

// One tree with After Effects Project panel columns (#378). Name is always shown.
enum Column : int {
    NameColumn,
    TypeColumn,
    SizeColumn,
    DurationColumn,
    FrameRateColumn,
    TagsColumn,
    LocationColumn,
    ColumnCount
};

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
// The virtual Data group (typed data blocks) and its rows: shown, never organised.
inline bool isData(const QTreeWidgetItem* item) {
    return item && item->data(0, kDataBlockRole).isValid();
}

// How often an asset or composition is used: nodes whose asset, font, LUT or composition
// reference names it, across every composition.
struct Usage final {
    std::size_t uses = 0;
    std::size_t compositions = 0;
};
[[nodiscard]] Usage assetUsage(const document::Project& project, document::AssetId id);
[[nodiscard]] Usage compositionUsage(const document::Project& project, document::CompositionId id);

// Column and header text shared by the rows and the selection header.
[[nodiscard]] QString frameRateText(document::FrameRate rate);
// A video's first picture stream rate, when it states one.
[[nodiscard]] std::optional<document::FrameRate> videoFrameRate(const document::AssetRecord& asset);
// After Effects style H:MM:SS:FF at `rate` (frames counted with the rate rounded up, as AE does).
[[nodiscard]] QString timecodeText(core::RationalTime time, document::FrameRate rate);
[[nodiscard]] QString assetKindText(const document::AssetRecord& asset);
[[nodiscard]] QString assetLocationText(const document::AssetRecord& asset);
[[nodiscard]] kit::IconId assetKindIcon(const document::AssetRecord& asset);
// The per-kind label colour, as After Effects assigns labels by type; matches the Timeline bars.
[[nodiscard]] kit::Color assetLabelColor(const document::AssetRecord& asset);

// Where new things go for this item: a folder row itself, otherwise the folder holding the item.
[[nodiscard]] std::optional<document::AssetFolderId> targetFolder(const QTreeWidgetItem* item);

// Folders sort before everything else in either direction and the Data group after; within a kind
// a column's kSortRole value (or its text, in natural order) decides.
class AssetItem final : public QTreeWidgetItem {
  public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override;
};

class AssetTree final : public QTreeWidget {
  public:
    AssetTree(CompositionSession& session, QWidget* parent);
    void beginRename(QTreeWidgetItem* item);
    // Test seam: overrides the GVFS root a dropped `smb://` URL resolves against (default:
    // bloom::ui::defaultGvfsRoot()), so an offscreen test can drive a drop through a fake mount
    // directory instead of this machine's real one.
    void setGvfsRootForTest(QString root) { gvfsRoot_ = std::move(root); }
    // The folder a drop at `position` (viewport coordinates) lands in: a folder row itself, the
    // folder of an asset or composition row, or the root (an empty optional) for empty space.
    // Nothing at all over the Data group, which holds no organisable items.
    [[nodiscard]] std::optional<std::optional<document::AssetFolderId>>
    dropFolder(QPoint position) const;
    // Delete or Backspace on the focused tree.
    std::function<void()> deleteRequested;

  protected:
    bool event(QEvent* event) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void startDrag(Qt::DropActions actions) override;
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    // The organisable items an internal drag carries, valid only for this tree and revision.
    struct Payload final {
        std::vector<document::AssetId> assets;
        std::vector<document::CompositionId> compositions;
        std::vector<document::AssetFolderId> folders;
        [[nodiscard]] bool empty() const noexcept {
            return assets.empty() && compositions.empty() && folders.empty();
        }
    };
    [[nodiscard]] Payload internalPayload(const QMimeData& mime) const;
    // Whether `payload` may land in `folder`: no folder into itself or one of its subfolders.
    [[nodiscard]] bool acceptsMove(const Payload& payload,
                                   std::optional<document::AssetFolderId> folder) const;
    CompositionSession& session_;
    QByteArray token_;
    QString gvfsRoot_;
};
} // namespace assets
} // namespace bloom::ui
