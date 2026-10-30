#include <bloom/ui/kit/swatch_button.hpp>

#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QIconEngine>
#include <QPainter>
#include <QPixmap>

#include <cmath>

namespace bloom::ui::kit {
namespace {

class SwatchIconEngine final : public QIconEngine {
  public:
    explicit SwatchIconEngine(QColor color) : color_(std::move(color)) {}
    QIconEngine* clone() const override { return new SwatchIconEngine(color_); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override {
        paintSwatch(*painter, rect, color_);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         const qreal scale) override {
        QPixmap result(QSize(static_cast<int>(std::ceil(size.width() * scale)),
                             static_cast<int>(std::ceil(size.height() * scale))));
        result.setDevicePixelRatio(scale);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }

  private:
    QColor color_;
};

} // namespace

void paintSwatch(QPainter& painter, const QRectF& bounds, const QColor& color) {
    const qreal extent = px(Size::IconSmall);
    const QRectF square(bounds.center().x() - extent / 2, bounds.center().y() - extent / 2, extent,
                        extent);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    fillRoundedSurface(painter, square, color, edgeInkFor(color), Radius::Small);
    painter.restore();
}

QIcon swatchIcon(const QColor& color) { return QIcon(new SwatchIconEngine(color)); }

KSwatchButton::KSwatchButton(QWidget* parent) : KIconButton(parent) {
    setObjectName(QStringLiteral("kSwatchButton"));
    ensureKeyboardFocusTracking(*this);
}

void KSwatchButton::setSwatch(const QColor& color) {
    if (swatch_ == color)
        return;
    swatch_ = color;
    update();
}

void KSwatchButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto colors = buttonColors(ButtonKind::Secondary, visualState(), hasKeyboardFocus(*this));
    fillRoundedSurface(painter, controlBody(*this), colors.fill, colors.border, Radius::Small);
    if (swatch_.isValid())
        paintSwatch(painter, controlBody(*this), isEnabled() ? swatch_ : disabledInk(swatch_));
}

} // namespace bloom::ui::kit
