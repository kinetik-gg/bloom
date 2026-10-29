#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QElapsedTimer>
#include <QPainterPath>
#include <QString>
#include <QWidget>

class QAbstractItemModel;
class QListView;
class QModelIndex;

namespace bloom::ui::kit {

class KSearchField;

// A row that is a section heading rather than an entry: drawn as a Faint UiSmall label, never
// current, never chosen. Qt::UserRole is left to the caller's own payload.
inline constexpr int kListHeadingRole = Qt::UserRole + 1;
// A row that is a separator: a hairline between groups, never current, never chosen.
inline constexpr int kListSeparatorRole = Qt::UserRole + 2;

// The most rows a list popup shows before it scrolls.
inline constexpr int kListPopupMaxRows = 12;

// The one list-in-popup: what a dropdown, the Add search and any other popup list are made of.
//
// Rows are painted by the kit's own delegate, edge to edge, at the menu row's height and insets: a
// reserved check column marks the committed value (or frames the row's icon, exactly as a checked
// menu action does), the label starts at the menu's text offset, and one Accent bar marks the
// current row -- the row under the pointer or the keyboard's, which are the same row. The bar is
// clipped to the popup frame's rounded inner edge by an antialiased path, so the first and last rows
// meet the frame's corners without squaring them off. Headings and separators come from the roles
// above. An elided row explains itself in a tooltip; a row's own tooltip (a refusal reason) wins.
//
// Keyboard: Up/Down step over disabled rows, headings and separators; Home/End and PgUp/PgDn;
// Enter/Return choose the current row; Tab and Shift+Tab choose it and ask to move focus on; Esc
// asks to close without choosing (closing a list is not an edit); typing jumps to the next row
// whose label starts with what was typed. Only the left button chooses.
//
// With the filter shown, a KSearchField sits above the rows. It keeps the text focus while
// Up/Down, PgUp/PgDn, Enter, Tab and Esc go to the list.
class KPopupList final : public QWidget {
    Q_OBJECT

  public:
    explicit KPopupList(QWidget* parent = nullptr);

    void setModel(QAbstractItemModel* model);
    [[nodiscard]] QAbstractItemModel* model() const;
    [[nodiscard]] QListView* view() const noexcept { return view_; }

    // Every row the same height (no headings or separators), which lets a long list -- the system
    // font catalogue -- lay out without measuring each row.
    void setUniformRows(bool uniform);

    void setFilterVisible(bool visible);
    [[nodiscard]] bool filterVisible() const noexcept;
    [[nodiscard]] KSearchField* filterField() const noexcept { return filter_; }

    // The row marked by the check column (the value the popup's owner holds), or -1.
    void setCommittedRow(int row);
    [[nodiscard]] int committedRow() const noexcept { return committedRow_; }

    // The row the Accent bar marks, or -1. setCurrentRow() refuses a row that cannot be chosen.
    [[nodiscard]] int currentRow() const;
    void setCurrentRow(int row);
    // Moves the current row by `direction` choosable rows, skipping what cannot be chosen; from no
    // current row, Down enters at the first and Up at the last.
    void stepCurrent(int direction);
    void stepToEnd(bool last);
    void stepPage(int direction);
    [[nodiscard]] bool isChoosable(int row) const;

    // Heights: one entry row, one heading, one separator -- and the list's own height for the rows
    // it holds, capped at kListPopupMaxRows entry rows.
    [[nodiscard]] int rowHeight() const;
    [[nodiscard]] int headingHeight() const;
    [[nodiscard]] int separatorHeight() const;
    [[nodiscard]] int fittedListHeight() const;
    // The surface's whole height: the filter (with its hairline) and the fitted list.
    [[nodiscard]] int fittedHeight() const;
    // Where row text starts, from the surface's left edge.
    [[nodiscard]] static int textOffset();
    // The width a row needs to show `text` whole: the text offset, the text and the trailing inset.
    [[nodiscard]] int rowWidthFor(const QString& text) const;

    // The rounded inner edge of the frame this surface fills, in the surface's own coordinates. The
    // current-row bar is clipped to it.
    void setFrameCorner(qreal corner);
    [[nodiscard]] QPainterPath rowClip() const;

    // Resets the filter, makes the committed row current and scrolls it into view.
    void prepareForOpen();
    void scrollToCurrent();

  Q_SIGNALS:
    // A row the artist chose (left-button release, Enter/Return, Tab), in the model's row numbers.
    void rowChosen(int row);
    // Tab or Shift+Tab after rowChosen: the owner moves focus on past its anchor.
    void tabbed(bool forward);
    void dismissed();
    void filterChanged(const QString& text);
    // Rows were inserted, removed or reset: an open popup re-fits its height.
    void rowsChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    bool handleKey(QKeyEvent* key, bool fromFilter);
    void typeAhead(const QString& text);
    void chooseCurrent();

    QListView* view_ = nullptr;
    KSearchField* filter_ = nullptr;
    QWidget* filterBox_ = nullptr;
    int committedRow_ = -1;
    qreal frameCorner_ = 0.0;
    QString typed_;
    QElapsedTimer typedClock_;
};

} // namespace bloom::ui::kit
