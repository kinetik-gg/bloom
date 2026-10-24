#include <bloom/ui/kit/thumbnail.hpp>

#include <bloom/ui/kit/tokens.hpp>

#include <QPainter>

namespace bloom::ui::kit {

KThumbnail::KThumbnail(QWidget* parent) : QWidget(parent) {
    setFixedSize(sizeHint());
    setAttribute(Qt::WA_OpaquePaintEvent);
}

QSize KThumbnail::sizeHint() const {
    return {px(Size::ImageThumbnail), px(Size::ImageThumbnail)};
}

void KThumbnail::setPixmap(const QPixmap& pixmap) {
    pixmap_ = pixmap;
    update();
}

void KThumbnail::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), color(Color::SurfaceSunken));
    if (pixmap_.isNull())
        return;
    // Logical extent of the picture; a glyph rendered for the device ratio keeps its own size.
    const auto ratio = pixmap_.devicePixelRatio() > 0 ? pixmap_.devicePixelRatio() : 1.0;
    QSizeF extent(pixmap_.width() / ratio, pixmap_.height() / ratio);
    if (extent.width() > width() || extent.height() > height())
        extent.scale(size(), Qt::KeepAspectRatio);
    const QRectF target((width() - extent.width()) / 2.0, (height() - extent.height()) / 2.0,
                        extent.width(), extent.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(target, pixmap_, QRectF(pixmap_.rect()));
}

} // namespace bloom::ui::kit
