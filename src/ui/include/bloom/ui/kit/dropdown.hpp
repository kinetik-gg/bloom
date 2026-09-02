#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QSize>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <cstdint>

class QListView;
class QStandardItemModel;

namespace bloom::ui::kit {

class KDropdownPopup;

// The Kinetik dropdown: a closed field carrying the current value and a caret pair, and a popup on
// a raised surface with full-width accent hover bars.
//
// Not a styled QComboBox. The popup has to be a raised surface with Popup elevation, Radius::Medium
// corners, and hover bars that span the full row -- a QComboBox's popup is a platform-dependent
// view whose frame, shadow, and hover geometry cannot all be reached from a stylesheet on every
// platform Bloom ships to.
class KDropdown final : public QWidget {
    Q_OBJECT

  public:
    enum class ControlSize : std::uint8_t {
        Compact,
        Default,
        Roomy,
    };

    explicit KDropdown(QWidget* parent = nullptr);
    ~KDropdown() override;

    int addItem(const QString& text, const QVariant& data = {});
    [[nodiscard]] int count() const;
    [[nodiscard]] QString itemText(int index) const;
    [[nodiscard]] QVariant itemData(int index) const;

    // A disabled item stays visible and readable but cannot be chosen -- neither by click nor by
    // setCurrentIndex(). Communicating "not available here" by hiding the row would leave the
    // artist wondering where it went.
    void setItemEnabled(int index, bool enabled);
    [[nodiscard]] bool isItemEnabled(int index) const;

    [[nodiscard]] int currentIndex() const;
    void setCurrentIndex(int index);
    [[nodiscard]] QString currentText() const;

    // The text actually painted in the closed field: the current value, ellipsized to whatever
    // width the field has. Exposed so a test can prove an oversized value elides rather than
    // overflowing or resizing the control.
    [[nodiscard]] QString displayedText() const;

    void setControlSize(ControlSize size);
    [[nodiscard]] ControlSize controlSize() const noexcept;

    void showPopup();
    void hidePopup();
    [[nodiscard]] bool isPopupVisible() const;
    [[nodiscard]] KDropdownPopup* popup() const noexcept;
    [[nodiscard]] QListView* popupView() const noexcept;

    [[nodiscard]] State visualState() const;

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

    QStandardItemModel* model_ = nullptr;
    KDropdownPopup* popup_ = nullptr;
    int currentIndex_ = -1;
    ControlSize controlSize_ = ControlSize::Default;
    bool hovered_ = false;
};

} // namespace bloom::ui::kit
