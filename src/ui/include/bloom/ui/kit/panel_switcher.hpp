#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QIcon>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QWidget>

class QStandardItemModel;

namespace bloom::ui::kit {

class KDropdownPopup;

// task U8, issue 131, formal amendment 2, A7: the panel header's own switcher, purpose-built
// after the QSS-on-QComboBox approach was abandoned (it could not render the design: no chevron,
// uppercase leaked from the shared QComboBox rule, and the field stretched to fill the header
// row instead of hugging its content). Composed from KDropdownPopup -- the SAME bordered
// SurfaceRaised popup machinery KDropdown itself opens -- with its own compact, content-hugging
// closed field: [item icon][label][CaretUpDown glyph], sized to exactly what it needs rather
// than expanding to fill its layout.
//
// Not KDropdown itself: KDropdown's own closed-field painting has no icon slot, and this
// control's label is deliberately Type::UI at natural case (A8), never KDropdown's Type::UI
// role either -- reusing KDropdownPopup's already-styled list is the shared machinery, not the
// whole widget.
class KPanelSwitcher final : public QWidget {
    Q_OBJECT

  public:
    explicit KPanelSwitcher(QWidget* parent = nullptr);
    ~KPanelSwitcher() override;

    // `icon` may be a null QIcon: an item with no documented glyph (an "unavailable editor"
    // placeholder) simply omits the icon block and its trailing gap, both in the closed field
    // and in the popup row.
    int addItem(const QIcon& icon, const QString& text, const QVariant& data = {});
    [[nodiscard]] int count() const;
    [[nodiscard]] QString itemText(int index) const;
    [[nodiscard]] QVariant itemData(int index) const;
    [[nodiscard]] QIcon itemIcon(int index) const;
    // Mirrors QComboBox's own tooltip-role convention: the CLOSED field's tooltip tracks the
    // current item's stored tooltip automatically, the same behavior EditorArea's
    // "unavailable editor" placeholder relied on when this control was a QComboBox.
    void setItemToolTip(int index, const QString& toolTip);
    [[nodiscard]] int findData(const QVariant& data) const;

    [[nodiscard]] int currentIndex() const;
    void setCurrentIndex(int index);
    [[nodiscard]] QVariant currentData() const;

    void showPopup();
    void hidePopup();
    [[nodiscard]] bool isPopupVisible() const;

    [[nodiscard]] State visualState() const;

    // The single border this control paints, by the kit-wide focus/hover rule
    // (kit::borderForInteraction). An open popup counts as active, exactly as focus does.
    [[nodiscard]] Color borderToken() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void currentIndexChanged(int index);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] int controlExtent() const;
    void commitIndex(int index);
    [[nodiscard]] QIcon currentIcon() const;

    QStandardItemModel* model_ = nullptr;
    KDropdownPopup* popup_ = nullptr;
    int currentIndex_ = -1;
    bool hovered_ = false;
};

} // namespace bloom::ui::kit
