#pragma once

#include <QIcon>
#include <bloom/ui/kit/tokens.hpp>

#include <QElapsedTimer>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <cstdint>

class QListView;
class QStandardItemModel;

namespace bloom::ui::kit {

class KDropdownPopup;

// A dropdown shows at least this many average characters of its value before it elides.
inline constexpr int kDropdownMinimumCharacters = 6;

// The Kinetik dropdown: a closed field carrying the current value and a caret pair, and a
// KDropdownPopup -- a KPopupWindow holding a KPopupList -- with the committed value checked and
// one Accent bar for the current row.
//
// Not a styled QComboBox. The popup has to be a raised surface with Popup elevation, Radius::Small
// corners, and hover bars that span the full row -- a QComboBox's popup is a platform-dependent
// view whose frame, shadow, and hover geometry cannot all be reached from a stylesheet on every
// platform Bloom ships to.
//
// Two signals, with QComboBox's split: currentIndexChanged() for every change of the value, and
// activated() only for a value the artist chose -- in the popup, by the closed field's arrow keys
// or type-ahead, or through the accessibility action. Building, refreshing or setting the list from
// code never emits activated(), so a control that commits to the document listens to activated()
// and a document refresh cannot write back. Unlike QComboBox, choosing the value the dropdown
// already holds emits nothing: a dropdown holds a value, and re-choosing it is not an edit (a list
// that must react to a repeat is a menu).
class KDropdown final : public QWidget {
    Q_OBJECT

  public:
    // The field's horizontal padding. Every size is one Control tall except Roomy.
    enum class ControlSize : std::uint8_t {
        // The tighter XS padding a dense row (Properties, a node card, the timeline) needs, at the
        // same Control height as Default.
        Dense,
        Default,
        Roomy,
    };

    explicit KDropdown(QWidget* parent = nullptr);
    ~KDropdown() override;

    // Adding an item never changes the value through a signal. The first item added to an empty
    // dropdown becomes its value silently, so a freshly built list shows something.
    int addItem(const QString& text, const QVariant& data = {});
    int addItem(const QIcon& icon, const QString& text, const QVariant& data = {});
    void setItemText(int index, const QString& text);
    [[nodiscard]] QIcon itemIcon(int index) const;
    void setItemToolTip(int index, const QString& toolTip);
    [[nodiscard]] int findData(const QVariant& data) const;
    [[nodiscard]] QVariant currentData() const;
    void clearItems();
    void setMutedValue(bool muted) {
        mutedValue_ = muted;
        update();
    }
    [[nodiscard]] bool mutedValue() const noexcept { return mutedValue_; }
    [[nodiscard]] int count() const;
    [[nodiscard]] QString itemText(int index) const;
    [[nodiscard]] QVariant itemData(int index) const;

    // A disabled item stays visible and readable but cannot be chosen -- neither by click nor by
    // setCurrentIndex(). Communicating "not available here" by hiding the row would leave the
    // artist wondering where it went. Disabling the CURRENT item keeps it the value, shown muted
    // with that item's own tooltip, because it is still what the document holds.
    void setItemEnabled(int index, bool enabled);
    [[nodiscard]] bool isItemEnabled(int index) const;

    [[nodiscard]] int currentIndex() const;
    // Programmatic: emits currentIndexChanged() when the value changes, never activated().
    void setCurrentIndex(int index);
    // The artist's choice: everything setCurrentIndex() does, then activated() when the value
    // changed. The popup, the closed-field keyboard and the accessibility action all come here.
    void chooseIndex(int index);
    [[nodiscard]] QString currentText() const;

    // The text actually painted in the closed field: the current value, ellipsized to whatever
    // width the field has. Exposed so a test can prove an oversized value elides rather than
    // overflowing or resizing the control.
    [[nodiscard]] QString displayedText() const;

    void setControlSize(ControlSize size);
    [[nodiscard]] ControlSize controlSize() const noexcept;

    void showPopup();
    void hidePopup();
    void setSearchable(bool searchable);
    [[nodiscard]] bool searchable() const noexcept;
    [[nodiscard]] bool isPopupVisible() const;
    [[nodiscard]] KDropdownPopup* popup() const noexcept;
    [[nodiscard]] QListView* popupView() const noexcept;

    [[nodiscard]] State visualState() const;

    // The single border this control paints, by the kit-wide focus/hover rule
    // (kit::borderForInteraction). An open popup counts as active, exactly as focus does.
    [[nodiscard]] Color borderToken() const;

    // sizeHint() is wide enough for the widest item; minimumSizeHint() for about
    // kDropdownMinimumCharacters of it, so a layout short of room elides the value instead of
    // overflowing. setWidthFloor() is a real floor: the field hugs its widest item when there is
    // room and never shrinks below `width`.
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    void setWidthFloor(int width);

    // Where the value's text starts, from the widget's left edge -- what the popup lines its rows up
    // with.
    [[nodiscard]] int textOffset() const;

  Q_SIGNALS:
    void currentIndexChanged(int index);
    void activated(int index);
    // Items were added, removed, renamed, enabled or disabled, or the dropdown itself was enabled
    // or disabled: a mirror of this list (a menu that offers the same choices) rebuilds.
    void itemsChanged();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] int controlExtent() const;
    [[nodiscard]] int horizontalPadding() const;
    [[nodiscard]] int caretGap() const;
    [[nodiscard]] QRectF labelRect() const;
    [[nodiscard]] int widestItemWidth() const;
    void invalidateWidest();
    bool commitIndex(int index);
    void stepValue(int direction);
    void typeAhead(const QString& text);

    QStandardItemModel* model_ = nullptr;
    KDropdownPopup* popup_ = nullptr;
    int currentIndex_ = -1;
    ControlSize controlSize_ = ControlSize::Default;
    bool hovered_ = false;
    bool mutedValue_ = false;
    mutable int widest_ = -1;
    QString typed_;
    QElapsedTimer typedClock_;
};

} // namespace bloom::ui::kit
