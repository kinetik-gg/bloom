#pragma once
#include <bloom/ui/kit/icons.hpp>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QMenu>

namespace bloom::ui::kit {
class KMenuButton : public QToolButton {
    Q_OBJECT
  public:
    explicit KMenuButton(QWidget* parent = nullptr);
    QSize sizeHint() const override;
};
class KIconButton : public QToolButton {
    Q_OBJECT
  public:
    explicit KIconButton(QWidget* parent = nullptr);
};
class KIconToggle final : public KIconButton {
    Q_OBJECT
  public:
    explicit KIconToggle(IconId id, QWidget* parent = nullptr);
    void setGlyph(IconId id);
    [[nodiscard]] QPixmap glyphPixmap() const;
  protected:
    void paintEvent(QPaintEvent* event) override;
  private:
    IconId glyph_;
};
class KLabel : public QLabel {
    Q_OBJECT
  public:
    explicit KLabel(QWidget* parent = nullptr);
    explicit KLabel(const QString& text, QWidget* parent = nullptr, TypeRole role = TypeRole::Ui);
    void setTypeRole(TypeRole role);
};
class KSearchField : public QLineEdit {
    Q_OBJECT
  public:
    explicit KSearchField(QWidget* parent = nullptr);
};
[[nodiscard]] QMenu* makeMenu(QWidget* parent = nullptr);
[[nodiscard]] QMenu* makeMenu(const QString& title, QWidget* parent = nullptr);
} // namespace bloom::ui::kit
