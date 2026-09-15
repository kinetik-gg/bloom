#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>
#include <bloom/ui/kit/icons.hpp>

class QVBoxLayout;
class QButtonGroup;
namespace bloom::ui::kit {
class KMenuButton : public QToolButton {
    Q_OBJECT
  public:
    explicit KMenuButton(QWidget* parent = nullptr);
    QSize sizeHint() const override;
};
// One row inside a flowing kit menu (kit::makeMenu with the columnFlow property): the action's
// text left-aligned at the menu-item inset, its shortcut right-aligned and muted, a raised
// surface under the pointer, muted ink when disabled. It is the ONLY item widget the flow uses;
// a header menu title (KMenuButton) is a different control and never appears inside a menu --
// that mix-up is what made flowed Add entries look centred and greyed (owner, 2026-09-15).
class KMenuItem final : public QToolButton {
    Q_OBJECT
  public:
    explicit KMenuItem(QWidget* parent = nullptr);
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
};
class KIconButton : public QToolButton {
    Q_OBJECT
  public:
    explicit KIconButton(QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
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
class KToolColumn final : public QWidget {
    Q_OBJECT
  public:
    explicit KToolColumn(QWidget* parent = nullptr);
    KIconToggle* addTool(IconId id, const QString& label, const QString& objectName,
                         bool enabled = true);

  private:
    QVBoxLayout* column_;
    QButtonGroup* group_;
};
class KLabel : public QLabel {
    Q_OBJECT
  public:
    explicit KLabel(QWidget* parent = nullptr);
    explicit KLabel(const QString& text, QWidget* parent = nullptr, TypeRole role = TypeRole::Ui);
    void setTypeRole(TypeRole role);
    void setElidedText(const QString& text);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    QString fullText_;
    bool elides_ = false;
};
class KLineEdit : public QLineEdit {
    Q_OBJECT
  public:
    explicit KLineEdit(QWidget* parent = nullptr);
    explicit KLineEdit(const QString& text, QWidget* parent = nullptr);
};
class KSearchField : public KLineEdit {
    Q_OBJECT
  public:
    explicit KSearchField(QWidget* parent = nullptr);
};
[[nodiscard]] QMenu* makeMenu(QWidget* parent = nullptr);
[[nodiscard]] QMenu* makeMenu(const QString& title, QWidget* parent = nullptr);
} // namespace bloom::ui::kit
