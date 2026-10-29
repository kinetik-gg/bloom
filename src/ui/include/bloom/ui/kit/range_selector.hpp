#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QRectF>
#include <QSize>
#include <QWidget>

#include <cstdint>

namespace bloom::ui::kit {

// A two-handle range: a work area, a trim, an in/out pair. The selected span is filled with accent
// between the handles; everything outside it stays field.
//
// Like KSlider, the handles follow the pointer with no easing: a range being dragged is direct
// editor feedback, and an eased handle would show a span that is not the span.
//
// Keyboard: one handle is active (the Lower one first) and carries the focus ring. Left/Down and
// Right/Up step it by kKeyStepFraction of the range, Page Up/Page Down by kCoarseStepMultiple times
// that, Home/End send it to its limit, and Tab moves to the Upper handle (Shift+Tab back) before
// focus leaves the control. Handles that sit on top of each other are told apart by the first
// move: a drag to the left takes Lower, to the right Upper. A drag or a run of keys is one edit
// (editStarted/editFinished, the same contract as KSlider).
class KRangeSelector final : public QWidget {
    Q_OBJECT

  public:
    enum class Handle : std::uint8_t {
        None,
        Lower,
        Upper,
    };

    explicit KRangeSelector(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept;
    [[nodiscard]] double maximum() const noexcept;

    [[nodiscard]] double lowerValue() const noexcept;
    [[nodiscard]] double upperValue() const noexcept;

    // The handles cannot cross: setting one past the other clamps it to the other rather than
    // swapping them, because a swap silently reassigns which handle the artist is holding.
    void setLowerValue(double value);
    void setUpperValue(double value);
    void setValues(double lower, double upper);

    [[nodiscard]] QRectF trackRect() const;
    [[nodiscard]] QRectF spanRect() const;
    [[nodiscard]] QRectF handleRect(Handle handle) const;

    [[nodiscard]] Handle grabbedHandle() const noexcept;
    // The handle the keyboard moves.
    [[nodiscard]] Handle activeHandle() const noexcept { return active_; }
    [[nodiscard]] bool hasLiveEdit() const noexcept { return interactionActive_; }
    void cancelEdit();
    [[nodiscard]] State visualState() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void lowerValueChanged(double value);
    void upperValueChanged(double value);
    void editStarted();
    void editFinished();
    void editCancelled();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    bool event(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] double valueForPosition(double x) const;
    [[nodiscard]] double normalized(double value) const;
    [[nodiscard]] Handle handleAt(const QPointF& point) const;
    void moveHandle(Handle handle, double value);
    void beginInteraction();
    void finishInteraction(bool keep);

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double lower_ = 0.0;
    double upper_ = 1.0;
    Handle grabbed_ = Handle::None;
    Handle active_ = Handle::Lower;
    // A press on coincident handles: which one it takes waits for the first move's direction.
    bool undecided_ = false;
    double pressX_ = 0.0;
    bool hovered_ = false;
    bool keyStepping_ = false;
    bool interactionActive_ = false;
    double lowerBase_ = 0.0;
    double upperBase_ = 0.0;
};

} // namespace bloom::ui::kit
