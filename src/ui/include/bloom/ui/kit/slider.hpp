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
class KSlider final : public QWidget {
    Q_OBJECT

  public:
    explicit KSlider(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept;
    [[nodiscard]] double maximum() const noexcept;

    [[nodiscard]] double value() const noexcept;
    void setValue(double value);

    // Where the value sits in the range, 0..1. The geometry below is derived from this, so a test
    // can assert position without measuring pixels.
    [[nodiscard]] double normalizedValue() const;

    [[nodiscard]] QRectF trackRect() const;
    [[nodiscard]] QRectF fillRect() const;
    [[nodiscard]] QRectF handleRect() const;

    [[nodiscard]] State visualState() const;
    [[nodiscard]] bool isDragging() const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void valueChanged(double value);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] double valueForPosition(double x) const;
    void commitValue(double value);

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double value_ = 0.0;
    bool dragging_ = false;
    bool hovered_ = false;
};

} // namespace bloom::ui::kit
