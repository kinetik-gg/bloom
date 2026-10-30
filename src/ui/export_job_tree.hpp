#pragma once

// The Export panel's job list: AE Render Queue order on the Assets row system. A job is a top-level
// row (label colour, enabled check, name, composition, range, status) and its outputs are child
// rows (preset, destination, status). Row height, alternation, hover and the selected recipe are
// the Assets ones (Size::AssetRow, kit::paintSelectedRow); every glyph is monochrome in the row's
// ink and colour is left for status alone.
//
// The tree holds no queue state: ExportEditor fills it from a snapshot and reads selection back by
// id. It reports gestures (enable toggles, reorders, composition drops) as signals; it never
// calls the queue itself.

#include <QStyledItemDelegate>
#include <QTreeWidget>

#include <QList>
#include <QPersistentModelIndex>

namespace bloom::ui {
class CompositionSession;
namespace exportpanel {

inline constexpr int kJobRole = Qt::UserRole + 1;    // qulonglong ExportJobId; on every row
inline constexpr int kOutputRole = Qt::UserRole + 2; // qulonglong ExportOutputId; 0 on a job row
inline constexpr int kToneRole = Qt::UserRole + 3;   // int StatusTone of the Status cell
inline constexpr int kGlyphRole = Qt::UserRole + 4;  // int kit::IconId of the Name cell
inline constexpr int kLabelRole = Qt::UserRole + 5;  // QColor of the label swatch, when set
inline constexpr int kEditableRole = Qt::UserRole + 6; // bool: the enabled check may be toggled
inline constexpr int kDestinationRole = Qt::UserRole + 7; // an output's destination text
inline constexpr auto kJobMimeType = "application/x-bloom-export-job";

enum Column : int {
    LabelColumn,
    EnabledColumn,
    NameColumn,
    CompositionColumn,
    RangeColumn,
    StatusColumn,
    ColumnCount
};

class ExportRowDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
    // Geometry shared with the tests: the enabled check's box inside its cell.
    [[nodiscard]] static QRect checkRect(const QRect& cell);
};

class ExportJobTree final : public QTreeWidget {
    Q_OBJECT

  public:
    ExportJobTree(CompositionSession& session, QWidget* parent);

    // The job row of `job`, or nullptr.
    [[nodiscard]] QTreeWidgetItem* jobItem(qulonglong job) const;
    [[nodiscard]] QTreeWidgetItem* outputItem(qulonglong job, qulonglong output) const;
    // The top-level row a drop at `position` (viewport coordinates) would insert before, counted
    // among the job rows; the job count for "after the last".
    [[nodiscard]] int dropIndexAt(QPoint position) const;
    // Fits the shown columns: Name takes the slack, the rest keep their widths.
    void fitColumns();

  Q_SIGNALS:
    void enabledToggled(qulonglong job, qulonglong output, bool enabled);
    // Drag-reorder: `job` should end at `index` among the jobs after the move.
    void moveRequested(qulonglong job, int index);
    // Compositions dragged in from Assets, in the drag's order.
    void compositionsDropped(const QList<qulonglong>& compositions);
    void removeRequested();

  protected:
    bool viewportEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    [[nodiscard]] bool acceptsDrag(const QMimeData& mime) const;
    void setHoverRow(const QModelIndex& index);
    void setDropRow(int index);

    CompositionSession& session_;
    QPersistentModelIndex hoverRow_;
    QPersistentModelIndex pressedCheck_;
    int dropRow_ = -1;
};

} // namespace exportpanel
} // namespace bloom::ui
