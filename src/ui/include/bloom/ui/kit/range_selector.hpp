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
    [[nodiscard]] State visualState() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void lowerValueChanged(double value);
    void upperValueChanged(double value);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] double valueForPosition(double x) const;
    [[nodiscard]] double normalized(double value) const;
    [[nodiscard]] Handle handleAt(const QPointF& point) const;

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double lower_ = 0.0;
    double upper_ = 1.0;
    Handle grabbed_ = Handle::None;
    bool hovered_ = false;
};

} // namespace bloom::ui::kit
