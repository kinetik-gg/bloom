#pragma once

// The Export panel's render list: the After Effects Render Queue order on Bloom's Assets row
// system. One QTreeWidget with real column headers (Render, #, Composition, Status, Started,
// Render Time) whose rows are painted exactly as an Assets row is (one Size::AssetRow, the
// alternation, hover and selected recipe across the whole viewport). A job is a top-level row;
// open, it shows one "Render Settings" line and one "Output Module" line per output, each with its
// own disclosure to a one-line summary. The lines are kit widgets set on spanned child rows.
//
// The list owns no queue state and issues no command. ExportEditor fills it from the snapshot and
// handles its signals; the list only measures its columns and the lines' shared label column, and
// starts a drag of the job rows the artist picks up.

#include <bloom/document/ids.hpp>

#include <QStyledItemDelegate>
#include <QTreeWidget>

#include <optional>

namespace bloom::ui::exportpanel {

enum ListColumn : int {
    // "Include in Start All": a centred KCheckBox, after the row's disclosure.
    RenderColumn,
    NumberColumn,
    CompositionColumn,
    StatusColumn,
    StartedColumn,
    RenderTimeColumn,
    // No title: the row's ⋯ button, right after the last column.
    ActionsColumn,
    // Empty: it takes the slack, so every column keeps the width of its content on a wide panel.
    FillerColumn,
    ListColumnCount
};

enum class RowKind : int { Job, RenderSettings, Output, Summary };

inline constexpr int kRowKindRole = Qt::UserRole + 1;
inline constexpr int kJobRole = Qt::UserRole + 2;
inline constexpr int kOutputRole = Qt::UserRole + 3;
// A drag of job rows inside the list: the job ids, as decimal text, one per line.
inline constexpr auto kExportJobsMimeType = "application/x-bloom-export-jobs";

[[nodiscard]] QString listColumnTitle(int column);
[[nodiscard]] RowKind rowKind(const QTreeWidgetItem* item);
[[nodiscard]] document::ExportJobId rowJob(const QTreeWidgetItem* item);
[[nodiscard]] document::ExportOutputId rowOutput(const QTreeWidgetItem* item);

// Paints the text cells of a job row on the Assets recipe: the composition in Foreground, the
// other columns Muted, every text one header padding in from its cell. Line rows are widgets.
class RenderListDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    // A cell's widget fills its cell exactly, so the lines and cells align with the column titles.
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override;
};

class RenderList final : public QTreeWidget {
    Q_OBJECT

  public:
    explicit RenderList(QWidget* parent);

    // Sizes every column to its content and the title, hiding Started and then Render Time when
    // the panel is too narrow for them, and narrowing Composition last. Runs after each fill and on
    // every resize.
    void fitColumns();

    // The shared label column of the lines ("Render Settings:", "Output Module:", "Output To:"),
    // right-aligned so their values start at one x; the module link's shared width, so the + and −
    // and Output To line up across every output; and where a line's values start in the viewport.
    [[nodiscard]] int lineLabelWidth() const;
    [[nodiscard]] int moduleWidth() const noexcept { return moduleWidth_; }
    void setModuleWidth(int width);
    [[nodiscard]] int valueLeft() const;
    // The x of a row's content at `depth` (0 for a job, 1 for its lines, 2 for a summary).
    [[nodiscard]] int depthLeft(int depth) const;

    [[nodiscard]] QTreeWidgetItem* jobItem(document::ExportJobId job) const;
    // The job row under `position` (viewport coordinates), the job a line or summary belongs to
    // included; nullptr over empty space.
    [[nodiscard]] QTreeWidgetItem* jobItemAt(QPoint position) const;

  Q_SIGNALS:
    // The ⋯ button or a right-click on a job row.
    void menuRequested(qulonglong job, QPoint globalPosition);
    // The list's width changed enough that a line may need to fold onto two lines.
    void widthChanged();

  protected:
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
    bool viewportEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;

  private:
    void setHoverRow(const QModelIndex& index);
    QPersistentModelIndex hoverRow_;
    int moduleWidth_ = 0;
    bool fitting_ = false;
};

} // namespace bloom::ui::exportpanel
