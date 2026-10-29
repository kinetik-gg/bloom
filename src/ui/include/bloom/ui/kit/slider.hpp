#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QRectF>
#include <QSize>
#include <QWidget>

namespace bloom::ui::kit {

// A single-value slider with an accent fill from the track's start to the handle.
//
// The handle follows the pointer with no easing at all. Scrubbing is direct editor feedback, and
// an eased handle lies about where the value is -- the same reason the playhead is never eased
// (Motion::None in the token vocabulary).
//
// It keeps KValueField's edit-session contract, so properties::bindValueEdit() binds it unchanged:
// a drag is one edit (editStarted() on the press, editFinished() on the release, or when the
// pointer grab or the window is lost), and a run of arrow/Page/Home/End presses is one edit that
// ends on the key release, Escape (which confirms) or focus loss. Pressing the handle keeps the
// pointer's offset from its centre, so grabbing it never jumps the value; pressing the bare track
// moves the handle there and drags on from it. A projection (setValue) arriving mid-drag is refused.
class KSlider final : public QWidget {
    Q_OBJECT

  public:
    explicit KSlider(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept;
    [[nodiscard]] double maximum() const noexcept;

    [[nodiscard]] double value() const noexcept;
    void setValue(double value);

    [[nodiscard]] bool hasLiveEdit() const noexcept { return interactionActive_; }
    // Programmatic cancellation: the value the edit started from comes back.
    void cancelEdit();

    // Where the value sits in the range, 0..1. The geometry below is derived from this, so a test
    // can assert position without measuring pixels.
    [[nodiscard]] double normalizedValue() const;

    [[nodiscard]] QRectF trackRect() const;
    [[nodiscard]] QRectF fillRect() const;
    [[nodiscard]] QRectF handleRect() const;

    [[nodiscard]] State visualState() const;

    // The single border the handle paints, by the kit-wide focus/hover rule
    // (kit::borderForInteraction).
    [[nodiscard]] Color borderToken() const;
    [[nodiscard]] bool isDragging() const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void valueChanged(double value);
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
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] double valueForPosition(double x) const;
    void commitValue(double value);
    void beginInteraction();
    void finishInteraction(bool keep);

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double value_ = 0.0;
    bool dragging_ = false;
    bool keyStepping_ = false;
    bool hovered_ = false;
    bool interactionActive_ = false;
    double interactionBase_ = 0.0;
    // The press's distance from the handle centre, kept for the whole drag.
    double grabOffset_ = 0.0;
};

} // namespace bloom::ui::kit
