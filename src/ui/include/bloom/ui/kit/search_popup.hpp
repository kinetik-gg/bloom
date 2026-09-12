#pragma once

#include <QWidget>
#include <QString>
#include <vector>

class QLineEdit;
class QStandardItemModel;

namespace bloom::ui::kit {
class KDropdownPopup;

struct SearchEntry final {
    QString key;
    QString label;
    QString keywords;
    QString refusal;
};

// Search owns filtering and keyboard navigation; the existing dropdown owns the raised surface,
// row states, list clipping and pointer selection. Disabled results retain their reason.
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
