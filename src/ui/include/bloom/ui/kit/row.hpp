#pragma once
#include <QHBoxLayout>
#include <QWidget>
#include <bloom/ui/kit/controls.hpp>
#include <initializer_list>
#include <optional>
namespace bloom::ui::kit {
class KPropertyRow final : public QWidget {
    Q_OBJECT
  public:
    KPropertyRow(QLabel* label, QWidget* indicator, std::initializer_list<QWidget*> values,
                 QWidget* parent = nullptr, bool leadingIndicator = false);
    QSize minimumSizeHint() const override;

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    QLabel* label_;
    bool leadingIndicator_ = false;
};
QLabel* makePropertyRowLabel(const QString& text, QWidget* parent);
class KRow : public QWidget {
    Q_OBJECT
  public:
    explicit KRow(QWidget* parent = nullptr);
    void setCells(const QList<QWidget*>& toggles, QWidget* name, const QList<QWidget*>& columns,
                  QWidget* trailing = nullptr);
    void setRowState(int index, bool selected);
    void setName(const QString& text, std::optional<IconId> disclosure = std::nullopt);
    KIconButton* disclosureButton() const { return disclosure_; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QHBoxLayout* row_;
    QWidget* nameCell_;
    KLabel* name_;
    KIconButton* disclosure_;
    int index_ = 0;
    bool selected_ = false;
};
} // namespace bloom::ui::kit
