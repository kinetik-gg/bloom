#pragma once

#include <bloom/ui/kit/popup_window.hpp>

class QAbstractItemModel;
class QListView;
class QSortFilterProxyModel;

namespace bloom::ui::kit {

class KPopupList;

// The popup a KDropdown opens: a KPopupWindow holding one KPopupList over the dropdown's items.
//
// Searchable, it shows the list's filter and filters through ONE proxy model it keeps for its whole
// life; the filter is cleared on every open, and the popup re-fits its height as rows filter away
// or come back, so there is never a dead band under the last row.
class KDropdownPopup final : public KPopupWindow {
    Q_OBJECT

  public:
    explicit KDropdownPopup(QWidget* parent = nullptr);

    void setModel(QAbstractItemModel* model);
    void setSearchable(bool searchable);
    void setUniformRows(bool uniform);
    [[nodiscard]] bool searchable() const noexcept { return searchable_; }
    [[nodiscard]] KPopupList* list() const noexcept { return list_; }
    [[nodiscard]] QListView* view() const noexcept;

    // The surface the artist sees: the list (and its filter), filling the frame inside its hairline.
    [[nodiscard]] QWidget* surface() const noexcept;

    // Opens against `anchor` with `committedRow` checked and current. `anchorTextOffset` is where
    // the anchor's own text starts, so the popup hangs its check column to the left and its row
    // text lines up with the closed field's.
    void openFor(const QWidget& anchor, int committedRow, int anchorTextOffset);

    // The frame size the current rows want: at least `anchorWidth` wide, wide enough for the
    // widest row, and exactly the rows tall (twelve at most) plus the frame's hairlines.
    [[nodiscard]] QSize wantedFrameSize(int anchorWidth) const;
    void setWidestRowText(int widestTextWidth);

  Q_SIGNALS:
    // A row the artist chose, in the dropdown's own item numbers. Emitted only for an item that may
    // be chosen; a disabled row swallows the click.
    void itemChosen(int index);
    // Tab or Shift+Tab chose the current row (or nothing) and asks focus to move on.
    void focusMoveRequested(bool forward);

  private:
    [[nodiscard]] int sourceRow(int row) const;
    [[nodiscard]] int viewRow(int sourceRow) const;
    void refitToRows();

    KPopupList* list_ = nullptr;
    QAbstractItemModel* sourceModel_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    int anchorWidth_ = 0;
    int widestText_ = 0;
    int committedSource_ = -1;
    bool searchable_ = false;
};

} // namespace bloom::ui::kit
