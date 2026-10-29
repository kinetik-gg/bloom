#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QStyleOptionMenuItem>
#include <QToolButton>
#include <vector>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

class QVBoxLayout;
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
//
// Keyboard: Up/Down move within a column and Left/Right across columns (Left from the first column
// closes the submenu, as Left does in any submenu), Home/End jump to the first/last row,
// Enter/Return/Space trigger, Esc closes. The current row is the pointer's or the keyboard's -- one
// row, painted State_Selected -- and disabled rows are stepped over.
class KMenuFlow final : public QWidget {
    Q_OBJECT
  public:
    // `heightCap` is the tallest the body may be; the row count follows from the measured row
    // height, so the cap holds whatever the menu-item primitive's height is.
    KMenuFlow(QMenu* menu, QList<QAction*> actions, int heightCap, QWidget* parent = nullptr);
    [[nodiscard]] int itemCount() const noexcept { return static_cast<int>(actions_.size()); }
    [[nodiscard]] QRect itemRect(int index) const;
    [[nodiscard]] int hoveredIndex() const noexcept { return hovered_; }
    [[nodiscard]] int currentIndex() const noexcept { return hovered_; }
    void setCurrentIndex(int index);
    [[nodiscard]] int columnCount() const noexcept { return columns_; }
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

  private:
    [[nodiscard]] int indexAt(QPoint position) const;
    [[nodiscard]] int enabledFrom(int index, int step) const;
    void trigger(int index);
    void fillOption(QStyleOptionMenuItem& option, int index) const;
    QMenu* menu_;
    QList<QAction*> actions_;
    int rows_;
    int columns_ = 1;
    QSize cell_;
    int hovered_ = -1;
};
// An icon-only button, one Size::Control square, wearing the one control look
// (kit::buttonColors(ButtonKind::Secondary)) with its glyph in the recipe's ink -- or plate-less as a
// disclosure caret ("disclosure" property) or a column heading's glyph (a "headerRow" parent). The
// focus border follows keyboard focus only, never a click.
class KIconButton : public QToolButton {
    Q_OBJECT
  public:
    explicit KIconButton(QWidget* parent = nullptr);
    [[nodiscard]] virtual State visualState() const;
    // The single border's state channel (kit::borderForInteraction): keyboard focus only.
    [[nodiscard]] Color borderToken() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    bool hovered_ = false;
};
// A checkable icon button. Its "flyoutMark" property draws a small corner mark: the slot holds more
// choices behind a press and hold or a right-click (the context bar's shape slot).
class KIconToggle final : public KIconButton {
    Q_OBJECT
  public:
    explicit KIconToggle(IconId id, QWidget* parent = nullptr);
    [[nodiscard]] State visualState() const override;
    void setGlyph(IconId id);
    [[nodiscard]] IconId glyph() const noexcept { return glyph_; }
    [[nodiscard]] QPixmap glyphPixmap() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    IconId glyph_;
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
// A read-only value in the one text-entry box (kit::fieldColors): the Field well in the control body,
// Border at rest, BorderHover under the pointer -- for a composite field that lays its own labels out
// inside and swaps in a KLineEdit to edit (the Viewer's frame field). While setEditing(true) it
// paints nothing, because the line edit draws the same box, focused, in the same place.
class KFieldFrame : public QWidget {
    Q_OBJECT
  public:
    explicit KFieldFrame(QWidget* parent = nullptr);
    void setEditing(bool editing);
    [[nodiscard]] bool editing() const noexcept { return editing_; }
    // The horizontal room between the widget's edge and the text inside the box: the control inset
    // plus the line edit's own text padding, so a label and the editor line their text up.
    [[nodiscard]] static int textInset();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    bool hovered_ = false;
    bool editing_ = false;
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
