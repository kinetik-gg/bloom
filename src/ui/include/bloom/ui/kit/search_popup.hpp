#pragma once

#include <QString>
#include <QWidget>
#include <vector>

class QLineEdit;
class QStandardItemModel;

namespace bloom::ui::kit {
class KDropdownPopup;

// Marks a row that is a section heading rather than a result (task S1, item 4). Qt::UserRole
// already carries a result's key, so a heading is the one row that has no key and this role set
// instead.
inline constexpr int kSearchSectionRole = Qt::UserRole + 1;

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

// Search owns filtering, section headings and keyboard navigation; the existing dropdown owns the
// raised surface, row states, list clipping and pointer selection.
//
// A result the command layer would refuse is listed DISABLED, and its reason lives in its tooltip
// alone -- never appended to its label, which would make the list's widest row an error message and
// read as though the refusal were part of the node's name.
//
// The list is exactly as tall as the rows and headings it holds. There is no minimum row count and
// no dead band under the last row, so the popup cannot show empty surface below its own content.
class KSearchPopup final : public QWidget {
    Q_OBJECT
  public:
    explicit KSearchPopup(QWidget* parent = nullptr);
    void setEntries(std::vector<SearchEntry> entries);
    void openAt(QPoint globalPosition);

  Q_SIGNALS:
    void entryChosen(const QString& key);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    void filter();
    void choose(int row);
    void step(int direction);
    KDropdownPopup* dropdown_ = nullptr;
    QLineEdit* field_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    std::vector<SearchEntry> entries_;
};
} // namespace bloom::ui::kit
