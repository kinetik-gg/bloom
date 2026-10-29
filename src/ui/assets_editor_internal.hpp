#pragma once
#include <QPoint>
#include <QString>
#include <QPersistentModelIndex>
#include <QSet>
#include <QStyledItemDelegate>
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
class Composition;
class Project;
struct AssetRecord;
} // namespace bloom::document

class QLineEdit;
class QMenu;

namespace bloom::ui {
class CompositionSession;
namespace kit {
class KLabel;
class KThumbnail;
} // namespace kit
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
// The Name cell's glyph as an int(kit::IconId), painted by AssetRowDelegate in the row's ink.
inline constexpr int kGlyphRole = Qt::UserRole + 9;
// An int(kit::Color) when the glyph carries state (Warn for a missing source); absent otherwise,
// so the glyph takes the row's ink like its text.
inline constexpr int kGlyphToneRole = Qt::UserRole + 10;
// Muted text after the Name ("3" for a folder holding three items), never edited by Rename.
inline constexpr int kSecondaryTextRole = Qt::UserRole + 11;
// True on secondary rows (the Data group), which take Muted ink.
inline constexpr int kSecondaryRowRole = Qt::UserRole + 12;
inline constexpr auto kInternalMimeType = "application/x-bloom-asset-list";
// Per-user column visibility, widths, order and sort (never project data). Version 2 adds File Size
// and the Name/Type/Dimensions defaults, so an older saved layout does not hide the new defaults.
inline constexpr auto kHeaderStateKey = "assets/header-state-v2";
// The columns whose width the user chose by dragging; the rest fit their content.
inline constexpr auto kSizedColumnsKey = "assets/sized-columns";
// View > Show Info: whether the summary strip shows (a per-user setting).
inline constexpr auto kShowInfoKey = "assets/show-info";

// One tree with After Effects Project panel columns (#378). Name always shows and takes the slack.
enum Column : int {
    NameColumn,
    TypeColumn,
    DimensionsColumn,
    DurationColumn,
    FrameRateColumn,
    FileSizeColumn,
    TagsColumn,
    LocationColumn,
    ColumnCount
};
// Name, Type and Dimensions show by default; the rest are chosen from the header's context menu or
// View > Columns.
[[nodiscard]] constexpr bool defaultColumnVisible(const int column) noexcept {
    return column == NameColumn || column == TypeColumn || column == DimensionsColumn;
}
[[nodiscard]] QString columnTitle(int column);

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
[[nodiscard]] QString timecodeForFrames(std::uint64_t frames, document::FrameRate rate);
// The whole frames `time` spans at `rate`, counted down as the timecode counts them.
[[nodiscard]] std::uint64_t framesAt(core::RationalTime time, document::FrameRate rate);
// One duration format (#380) for video, sequences and compositions: the timecode at the item's
// rate with its frame count, "0:00:02:00 (48 frames)".
[[nodiscard]] QString durationText(std::uint64_t frames, document::FrameRate rate);
// The Duration cell: the timecode alone; durationText() and the rate go in its tooltip.
[[nodiscard]] QString durationTooltip(std::uint64_t frames, document::FrameRate rate);
// Bytes on disk for the File Size column, in the locale's SI units ("12.4 MB").
[[nodiscard]] QString fileSizeText(std::uint64_t bytes);
// A sequence plays one member per composition frame, so it is timed at the active composition's
// rate (the default composition rate when none is open) over its whole numbered range.
[[nodiscard]] document::FrameRate sequenceRate(const document::Composition* active);
[[nodiscard]] std::uint64_t sequenceFrames(const document::AssetRecord& asset);
[[nodiscard]] QString assetKindText(const document::AssetRecord& asset);
[[nodiscard]] QString assetLocationText(const document::AssetRecord& asset);
// The kind glyph. Icons are monochrome (visual-language.md): a row paints it in its own ink, and
// colour is left for state (Warn) alone.
[[nodiscard]] kit::IconId assetKindIcon(const document::AssetRecord& asset);
// "2 compositions · 5 sequences": a count of each kind, most numerous first.
struct KindCounts final {
    std::size_t folders = 0;
    std::size_t compositions = 0;
    std::size_t sequences = 0;
    std::size_t images = 0;
    std::size_t videos = 0;
    std::size_t audio = 0;
    std::size_t fonts = 0;
    std::size_t luts = 0;
    std::size_t data = 0;
    void add(const QTreeWidgetItem& item, const document::Project& project);
    [[nodiscard]] std::size_t items() const noexcept;
    [[nodiscard]] QString text() const;
};

// Where new things go for this item: a folder row itself, otherwise the folder holding the item.
[[nodiscard]] std::optional<document::AssetFolderId> targetFolder(const QTreeWidgetItem* item);

// Folders sort before everything else in either direction and the Data group after; within a kind
// a column's kSortRole value (or its text, in natural order) decides.
class AssetItem final : public QTreeWidgetItem {
  public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override;
};

// The one painter of an Assets row's cells: in Name the kind glyph at the cell's left, the name
// and its muted secondary text; elsewhere the column text, muted, with File Location elided in the
// middle so the file name stays. Every glyph is monochrome in the row's ink. The row's background
// (alternation, hover, the selected recipe) spans the viewport and is AssetTree::drawRow's.
class AssetRowDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override;
    // Geometry within a Name cell, for painting and tests: the glyph box and where the text starts.
    [[nodiscard]] static QRect glyphRect(const QRect& cell);
    [[nodiscard]] static int textLeft(const QRect& cell, int column);
    // The ink a row's text and glyph take: OnAccent selected, Muted on a secondary row (the Data
    // group), Foreground otherwise; `tone` (a state glyph's own colour) wins for the glyph.
    [[nodiscard]] static kit::Color glyphInk(const QModelIndex& index, bool selected);

  protected:
    bool eventFilter(QObject* object, QEvent* event) override;
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
    // Test seam: the payload a drag of the current selection carries, as startDrag builds it.
    [[nodiscard]] QMimeData* selectionMimeDataForTest() const { return mimeData(selectedItems()); }

  protected:
    bool event(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    // The row background across the whole viewport -- alternation, hover and the selected recipe
    // with its Accent edge at the row's left, never at its indent -- before the cells.
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
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
    void setHoverRow(const QModelIndex& index);
    CompositionSession& session_;
    QByteArray token_;
    QString gvfsRoot_;
    QPersistentModelIndex hoverRow_;
};

// The compact summary strip above the tree (one fixed SummaryStrip height): with one item, a
// thumbnail well, the name with its usage trailing in Faint, and one Muted line of skimmable facts
// (`Sequence · 2048 × 858 · 0:00:16:02 · 386 f · 24 fps · sRGB · Straight`), a warning in Warn; with
// several, their count and kinds; with none, the project's kinds in one Faint line. The detailed
// sentence of each fact is its tooltip. It reads the project and the controllers' published state
// only; thumbnails are prepared on their workers.
class SummaryStrip final : public QWidget {
  public:
    // `tree` is the one the strip summarises when nothing is selected.
    SummaryStrip(CompositionSession& session, const QTreeWidget& tree, QWidget* parent);
    void refresh(const QList<QTreeWidgetItem*>& selection);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    const QTreeWidget* tree_ = nullptr;
    kit::KThumbnail* thumbnail_ = nullptr;
    kit::KLabel* name_ = nullptr;
    kit::KLabel* usage_ = nullptr;
    kit::KLabel* kind_ = nullptr;
    kit::KLabel* colour_ = nullptr;
    QString nameText_, usageText_, kindText_, colourText_;
};

// The collapsible search row under the panel header: a KSearchField, a muted match count and a
// Close button. Hidden until opened (the header's Search toggle or Ctrl+F); Escape clears the text,
// a second Escape closes it. While text filters the tree it stays open, so no filter hides assets
// out of sight.
class SearchBar final : public QWidget {
  public:
    explicit SearchBar(QWidget* parent);
    [[nodiscard]] QLineEdit* field() const noexcept { return field_; }
    void open();
    // Clears the text and hides the row; `closed` runs after (the editor focuses the tree).
    void close();
    void setMatches(int matches, int total);
    std::function<void()> closed;
    std::function<void(bool)> visibilityChanged;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QLineEdit* field_ = nullptr;
    kit::KLabel* count_ = nullptr;
};

// Which columns show, their widths and the sort, as a per-user setting: Name stretches to take the
// slack so rows always span the panel; the rest fit their content until the user drags one.
class ColumnController final : public QObject {
  public:
    explicit ColumnController(QTreeWidget& tree);
    void restore();
    void save() const;
    void setVisible(int column, bool visible);
    void fill(QMenu& menu);
    // Fits every shown column the user has not sized to its content (after a row rebuild).
    void fit();

  private:
    void fitColumn(int column);
    QTreeWidget& tree_;
    QSet<int> sized_;
    bool fitting_ = false;
    bool restoring_ = false;
};
} // namespace assets
} // namespace bloom::ui
