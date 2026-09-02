#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractButton>
#include <QSize>

namespace bloom::ui::kit {

class QPropertyAnimationHolder;

// A two-state toggle whose thumb slides between the ends with the Fast motion. Under the
// reduced-motion kill switch it jumps to the end state instead, because a toggle that cannot
// animate must still be a toggle.
class KSwitch final : public QAbstractButton {
    Q_OBJECT
    // The animated quantity, exposed as a property so QPropertyAnimation can drive it and a test
    // can read where the thumb actually is rather than inferring it from the checked state.
    Q_PROPERTY(qreal thumbPosition READ thumbPosition WRITE setThumbPosition)

  public:
    explicit KSwitch(QWidget* parent = nullptr);

    // 0.0 at the off end, 1.0 at the on end.
    [[nodiscard]] qreal thumbPosition() const noexcept;
    void setThumbPosition(qreal position);

    [[nodiscard]] State visualState() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void checkStateSet() override;
    void nextCheckState() override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void animateThumbTo(qreal target);

    qreal thumbPosition_ = 0.0;
    bool hovered_ = false;
};

} // namespace bloom::ui::kit
