#pragma once

#include <bloom/ui/kit/popup_list.hpp>
#include <bloom/ui/kit/popup_window.hpp>

#include <QString>
#include <vector>

class QStandardItemModel;

namespace bloom::ui::kit {

// Marks a row that is a section heading rather than a result (task S1, item 4): the list
// surface's own heading role. Qt::UserRole carries a result's key.
inline constexpr int kSearchSectionRole = kListHeadingRole;

struct SearchEntry final {
    QString key;
    QString label;
    QString keywords;
    QString refusal;
    // The heading this entry is listed under. Entries are NOT regrouped: they appear in the order
    // the caller supplies them, and a heading is emitted whenever this string changes, so the
    // caller's own section order is what the artist reads. Empty lists the entry with no heading.
    QString section;
};

// The Add search: a KPopupWindow holding one KPopupList with its filter shown. Search owns the
// filtering, the ranking and the section headings; the list surface owns the rows, their states,
// the keyboard and pointer choice; the popup window owns placement, shadow and dismissal.
//
// A result the command layer would refuse is listed DISABLED, and its reason lives in its tooltip
// alone -- never appended to its label, which would make the list's widest row an error message and
// read as though the refusal were part of the node's name.
//
// The list is exactly as tall as the rows and headings it holds, up to the list surface's twelve-row
// cap, and re-fits as the filter narrows it: there is no dead band under the last row.
class KSearchPopup final : public KPopupWindow {
    Q_OBJECT
  public:
    explicit KSearchPopup(QWidget* parent = nullptr);
    void setEntries(std::vector<SearchEntry> entries);
    void openAt(QPoint globalPosition);
    [[nodiscard]] KPopupList* list() const noexcept { return list_; }

  Q_SIGNALS:
    void entryChosen(const QString& key);

  private:
    void filter();
    void choose(int row);
    [[nodiscard]] QSize wantedFrameSize() const;
    KPopupList* list_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    std::vector<SearchEntry> entries_;
};
} // namespace bloom::ui::kit
