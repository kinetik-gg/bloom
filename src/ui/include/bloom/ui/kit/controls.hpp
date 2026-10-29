#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStyleOptionMenuItem>
#include <QToolButton>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

class QVBoxLayout;
class QButtonGroup;
namespace bloom::ui::kit {
class KMenuButton : public QToolButton {
    Q_OBJECT
  public:
    explicit KMenuButton(QWidget* parent = nullptr);
    QSize sizeHint() const override;
};
// The body of a flowing kit menu (kit::makeMenu with the columnFlow property): the menu's own
// actions laid out in columns and painted with the SAME primitive every other menu row uses --
// the proxy style's CE_MenuItem (full-width Accent hover bar, Foreground ink, Faint shortcut,
// reserved icon column) -- so a flowed submenu is indistinguishable from its parent. It tracks
// the pointer itself, because a popup's child widgets cannot rely on hover attributes, and
// triggers the row's action on release. (Owner, 2026-09-15: the first attempt used a separate
// button widget and drifted in hover and highlight; this class exists so that cannot recur.)
class KMenuFlow final : public QWidget {
    Q_OBJECT
  public:
    // `heightCap` is the tallest the body may be; the row count follows from the measured row
    // height, so the cap holds whatever the menu-item primitive's height is.
    KMenuFlow(QMenu* menu, QList<QAction*> actions, int heightCap, QWidget* parent = nullptr);
    [[nodiscard]] int itemCount() const noexcept { return static_cast<int>(actions_.size()); }
    [[nodiscard]] QRect itemRect(int index) const;
    [[nodiscard]] int hoveredIndex() const noexcept { return hovered_; }
    [[nodiscard]] int columnCount() const noexcept { return columns_; }
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    [[nodiscard]] int indexAt(QPoint position) const;
    void fillOption(QStyleOptionMenuItem& option, int index) const;
    QMenu* menu_;
    QList<QAction*> actions_;
    int rows_;
    int columns_ = 1;
    QSize cell_;
    int hovered_ = -1;
};
// An icon-only button in a recessed ControlSurface well (header buttons), or plate-less as a
// disclosure / header-row glyph. Colours come from kit::buttonColors(ButtonKind::Recessed); the
// focus border follows keyboard focus only, never a click.
class KIconButton : public QToolButton {
    Q_OBJECT
  public:
    explicit KIconButton(QWidget* parent = nullptr);
    [[nodiscard]] State visualState() const;
    // The single border's state channel (kit::borderForInteraction): keyboard focus only.
    [[nodiscard]] Color borderToken() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    bool hovered_ = false;
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
// TypeRole-bound text. A single-line label is exactly one control tall, so it lines up with the
// controls beside it; a word-wrapped label grows to its wrapped height (never below one control)
// instead of clipping to its first line. setElidedText() is always single-line.
class KLabel : public QLabel {
    Q_OBJECT
  public:
    explicit KLabel(QWidget* parent = nullptr);
    explicit KLabel(const QString& text, QWidget* parent = nullptr, TypeRole role = TypeRole::Ui);
    void setTypeRole(TypeRole role);
    void setElidedText(const QString& text);
    // Hides QLabel::setWordWrap so the height rule follows the wrap mode; a wrap mode set through
    // a QLabel pointer is picked up again when the label is shown.
    void setWordWrap(bool on);
    // The label's ink, by palette role. The one way to colour interface text: disabled text fades
    // by the kit's disabled recipe from this tone rather than from Foreground.
    void setTone(Color tone);
    [[nodiscard]] Color tone() const noexcept { return tone_; }

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void applyHeightRule();
    QString fullText_;
    bool elides_ = false;
    Color tone_ = Color::Foreground;
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
