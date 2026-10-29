#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractButton>
#include <QPointer>
#include <QSize>

class QPropertyAnimation;

namespace bloom::ui::kit {

class QPropertyAnimationHolder;

// A two-state toggle whose thumb slides between the ends with the Fast motion when the artist
// toggles it. A programmatic setChecked() (a projection of document state) places the thumb
// without travel, and under the reduced-motion kill switch every toggle jumps, because a toggle
// that cannot animate must still be a toggle. Hover shows on both states.
class KSwitch : public QAbstractButton {
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

    // The single border this control paints, by the kit-wide focus/hover rule
    // (kit::borderForInteraction). An "on" switch is a filled accent track with no border of its
    // own, so only the Accent result is painted there.
    [[nodiscard]] Color borderToken() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void checkStateSet() override;
    void nextCheckState() override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

    [[nodiscard]] bool isHovered() const noexcept { return hovered_; }

  private:
    void animateThumbTo(qreal target);

    qreal thumbPosition_ = 0.0;
    bool hovered_ = false;
    QPointer<QPropertyAnimation> slide_;
};

// Square checkbox presentation with the same keyboard and checked-state contract. Keyboard focus
// draws an Accent ring outside the box (a checked box is Accent already, so its border could not
// carry focus). An optional label (setLabel) is painted after the box and is part of the click
// target, so a checkbox and its words are one control. setMixed() shows the indeterminate dash of
// a multi-selection whose members differ; a click then checks it.
class KCheckBox final : public KSwitch {
    Q_OBJECT
  public:
    explicit KCheckBox(QWidget* parent = nullptr);
    explicit KCheckBox(const QString& label, QWidget* parent = nullptr);
    void setLabel(const QString& label);
    [[nodiscard]] QString label() const { return text(); }
    void setMixed(bool mixed);
    [[nodiscard]] bool isMixed() const noexcept { return mixed_; }
    [[nodiscard]] QRectF boxRect() const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void nextCheckState() override;
    void checkStateSet() override;

  private:
    bool mixed_ = false;
};

} // namespace bloom::ui::kit
