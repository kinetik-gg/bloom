#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

namespace bloom::ui::kit {

// A labelled numeric cell: the parameter's name, a bordered field carrying the number in the
// monospaced Value role, an optional unit suffix, and a stepper pair.
//
// The number is monospaced on purpose. Every numeric, unit, hex, and timecode surface in Bloom
// uses the Value role, so a column of them stays aligned and a digit changing does not reflow the
// text beside it.
class KValueField final : public QWidget {
    Q_OBJECT

  public:
    explicit KValueField(QWidget* parent = nullptr);

    void setLabel(const QString& label);
    [[nodiscard]] QString label() const;

    // A short suffix such as "px", "%", or "fps", drawn inside the cell after the number in the
    // muted ink so it reads as a unit rather than as part of the value.
    void setUnit(const QString& unit);
    [[nodiscard]] QString unit() const;

    void setRange(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept;
    [[nodiscard]] double maximum() const noexcept;

    void setSingleStep(double step);
    [[nodiscard]] double singleStep() const noexcept;

    void setDecimals(int decimals);
    [[nodiscard]] int decimals() const noexcept;

    [[nodiscard]] double value() const noexcept;
    void setValue(double value);

    void stepBy(int steps);

    // The number exactly as painted, at the configured precision, without the unit.
    [[nodiscard]] QString displayedValue() const;

    [[nodiscard]] QRectF labelRect() const;
    [[nodiscard]] QRectF cellRect() const;
    [[nodiscard]] QRectF stepUpRect() const;
    [[nodiscard]] QRectF stepDownRect() const;

    [[nodiscard]] State visualState() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void valueChanged(double value);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void commitValue(double value);

    QString label_;
    QString unit_;
    double minimum_ = 0.0;
    double maximum_ = 100.0;
    double step_ = 1.0;
    double value_ = 0.0;
    int decimals_ = 2;
    bool hovered_ = false;
};

} // namespace bloom::ui::kit
