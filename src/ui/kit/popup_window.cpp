#include <bloom/ui/kit/popup_window.hpp>

#include <bloom/ui/kit/painting.hpp>

#include <QApplication>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QVariantAnimation>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

// The frame's hairline in whole logical pixels: the content sits exactly this far inside the
// frame, so a full-width row never paints over the border that defines it.
[[nodiscard]] int frameBorder() { return std::max(1, static_cast<int>(std::lround(kHairlineWidth))); }

[[nodiscard]] QRect availableScreenFor(const QRect& anchor, const QWidget* fallback) {
    const QScreen* screen = QGuiApplication::screenAt(anchor.center());
    if (screen == nullptr && fallback != nullptr)
        screen = fallback->screen();
    if (screen == nullptr)
        screen = QGuiApplication::primaryScreen();
    return screen == nullptr ? QRect(anchor.topLeft(), QSize(1 << 15, 1 << 15))
                             : screen->availableGeometry();
}

} // namespace

bool popupTranslucencySupported() {
    const QString platform = QGuiApplication::platformName();
    return platform.startsWith(QStringLiteral("wayland")) ||
           platform == QStringLiteral("windows") || platform == QStringLiteral("cocoa") ||
           platform == QStringLiteral("offscreen");
}

QMargins popupGutter(const Elevation elevation) {
    const Shadow token = shadow(elevation);
    if (token.isFlat() || !popupTranslucencySupported())
        return {};
    const int blur = token.blurRadius;
    return {std::max(0, blur - token.offsetX), std::max(0, blur - token.offsetY),
            blur + std::max(0, token.offsetX), blur + std::max(0, token.offsetY)};
}

QRect visualGlobalRect(const QWidget& anchor) {
    const QWidget* top = anchor.window();
    if (const auto* proxy = top->graphicsProxyWidget(); proxy != nullptr && proxy->scene()) {
        const QRectF local(QPointF(anchor.mapTo(top, QPoint(0, 0))), QSizeF(anchor.size()));
        const QPolygonF scene = proxy->mapToScene(local);
        for (const auto* view : proxy->scene()->views()) {
            if (!view->isVisible())
                continue;
            const QRect inView = view->mapFromScene(scene).boundingRect();
            return {view->viewport()->mapToGlobal(inView.topLeft()), inView.size()};
        }
    }
    return {anchor.mapToGlobal(QPoint(0, 0)), anchor.size()};
}

PopupPlacement placePopup(const PopupPlacementRequest& request) {
    const QRect& screen = request.screen;
    const int width = std::min(request.frame.width(), screen.width());
    const int x = std::clamp(request.anchor.left() - request.leadingShift, screen.left(),
                             std::max(screen.left(), screen.right() + 1 - width));
    const int belowTop = request.anchor.bottom() + 1 + request.gap;
    const int roomBelow = screen.bottom() + 1 - belowTop;
    const int aboveBottom = request.anchor.top() - request.gap;
    const int roomAbove = aboveBottom - screen.top();
    const int wanted = request.frame.height();
    if (wanted <= roomBelow)
        return {{x, belowTop, width, wanted}, false};
    if (roomAbove > roomBelow) {
        const int height = std::min(wanted, roomAbove);
        return {{x, aboveBottom - height, width, height}, true};
    }
    // Neither side fits: take the roomier one -- below -- and let the content scroll. A popup whose
    // anchor is itself off the bottom of the screen still opens on screen.
    const int height = std::min(wanted, std::max(0, roomBelow));
    if (height <= 0)
        return {{x, std::max(screen.top(), screen.bottom() + 1 - wanted), width,
                 std::min(wanted, screen.height())},
                false};
    return {{x, belowTop, width, height}, false};
}

// Qt::BypassGraphicsProxyWidget: when the anchor lives inside a QGraphicsProxyWidget (a node card
// row), Qt would otherwise embed this popup into the scene as a sub-proxy, where it lands behind
// the card and never receives the pointer (owner, 2026-09-15). The flag keeps it a real top-level
// popup, still parented to the anchor so a Wayland compositor can position it against its parent.
KPopupWindow::KPopupWindow(QWidget* parent, const Elevation elevation)
    : QWidget(parent, Qt::Popup | Qt::BypassGraphicsProxyWidget), elevation_(elevation),
      gutter_(popupGutter(elevation)) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    translucent_ = popupTranslucencySupported();
    if (translucent_)
        setAttribute(Qt::WA_TranslucentBackground, true);
    rise_ = new QVariantAnimation(this);
    rise_->setEasingCurve(easing(Motion::Pop));
    connect(rise_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        riseOffset_ = value.toInt();
        layoutContent();
        update();
    });
}

void KPopupWindow::setContent(QWidget* content, QWidget* focusTarget) {
    content_ = content;
    focusTarget_ = focusTarget;
    if (content_ != nullptr) {
        content_->setParent(this);
        content_->show();
    }
    layoutContent();
}

QRect KPopupWindow::frameRect() const {
    return rect().marginsRemoved(gutter_).translated(0, riseOffset_);
}

qreal KPopupWindow::frameCorner() const {
    // No gutter means no guaranteed transparency, so no rounded corners either: their outside
    // pixels would be opaque.
    if (!translucent_)
        return 0.0;
    const QRect frame = frameRect();
    return radiusPx(Radius::Small, std::min(frame.width(), frame.height()));
}

void KPopupWindow::layoutContent() {
    if (content_ == nullptr)
        return;
    const int border = frameBorder();
    content_->setGeometry(frameRect().adjusted(border, border, -border, -border));
}

void KPopupWindow::openFor(const QWidget& anchor, const QSize frameSize, const int leadingShift) {
    anchorRect_ = visualGlobalRect(anchor);
    open({anchorRect_, frameSize, availableScreenFor(anchorRect_, &anchor), px(Spacing::XXS),
          leadingShift});
}

void KPopupWindow::openAt(const QPoint globalPosition, const QSize frameSize) {
    anchorRect_ = QRect(globalPosition, QSize(0, 0));
    open({anchorRect_, frameSize, availableScreenFor(QRect(globalPosition, QSize(1, 1)), this), 0,
          0});
}

void KPopupWindow::refit(const QSize frameSize) {
    if (request_.frame == frameSize)
        return;
    request_.frame = frameSize;
    const PopupPlacement placement = placePopup(request_);
    above_ = placement.above;
    setGeometry(placement.frame.marginsAdded(gutter_));
    layoutContent();
    update();
}

void KPopupWindow::open(const PopupPlacementRequest& request) {
    if (!formatChecked_) {
        formatChecked_ = true;
        create();
        // The platform promised transparency; the window's surface must actually carry alpha, or
        // the gutter would be an opaque band. Without it the popup is flat.
        if (translucent_ && windowHandle() != nullptr &&
            windowHandle()->format().alphaBufferSize() <= 0) {
            translucent_ = false;
            gutter_ = {};
        }
    }
    request_ = request;
    const PopupPlacement placement = placePopup(request);
    above_ = placement.above;
    rise_->stop();
    const int riseMs = durationMs(Motion::Pop);
    // The entrance rises toward the anchor from inside the gutter, so it needs a gutter to move in.
    const bool animate = riseMs > 0 && gutter_.bottom() >= kPopRisePx && gutter_.top() >= kPopRisePx;
    riseOffset_ = animate ? (above_ ? -kPopRisePx : kPopRisePx) : 0;
    setGeometry(placement.frame.marginsAdded(gutter_));
    layoutContent();
    if (!isVisible())
        show();
    else
        focusContent();
    raise();
    if (animate) {
        rise_->setDuration(riseMs);
        rise_->setStartValue(riseOffset_);
        rise_->setEndValue(0);
        rise_->start();
    }
}

void KPopupWindow::focusContent() {
    QWidget* target = focusTarget_;
    if (target == nullptr && content_ != nullptr) {
        // The content's first focusable descendant, in focus-chain order.
        for (QWidget* candidate = content_; candidate != nullptr;) {
            if (candidate->isVisibleTo(this) && candidate->isEnabled() &&
                (candidate->focusPolicy() & Qt::TabFocus) != 0) {
                target = candidate;
                break;
            }
            candidate = candidate->nextInFocusChain();
            if (candidate == content_ || candidate == nullptr || !content_->isAncestorOf(candidate))
                break;
        }
    }
    if (target != nullptr)
        target->setFocus(Qt::PopupFocusReason);
}

void KPopupWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF frame(frameRect());
    if (!gutter_.isNull())
        drawElevationShadow(painter, frame, Radius::Small, elevation_);
    fillRoundedSurface(painter, frame, color(Color::SurfaceRaised), color(Color::Border),
                       frameCorner());
}

void KPopupWindow::mousePressEvent(QMouseEvent* event) {
    const QPoint local = event->position().toPoint();
    if (frameRect().contains(local)) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint global = event->globalPosition().toPoint();
    // A press on the anchor closes the popup and ends there: replaying it would reach the anchor
    // after the popup is gone, and a dropdown field would open its popup again.
    const bool onAnchor = anchorRect_.contains(global);
    setAttribute(Qt::WA_NoMouseReplay, onAnchor);
    const bool inGutter = rect().contains(local);
    close();
    event->accept();
#if !defined(Q_OS_MACOS)
    // Qt replays a press outside the popup's window to the widget under it (except on macOS, where
    // the press only dismisses). The gutter is inside the window but outside what the artist sees,
    // so a press there is passed on the same way.
    if (inGutter && !onAnchor) {
        if (QWidget* under = QApplication::widgetAt(global); under != nullptr) {
            if (QWindow* handle = under->window()->windowHandle(); handle != nullptr) {
                const QPointF windowLocal = handle->mapFromGlobal(QPointF(global));
                QCoreApplication::postEvent(
                    handle, new QMouseEvent(QEvent::MouseButtonPress, windowLocal, windowLocal,
                                            QPointF(global), event->button(), event->buttons(),
                                            event->modifiers()));
            }
        }
    }
#else
    Q_UNUSED(inGutter)
#endif
}

void KPopupWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KPopupWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    focusContent();
}

void KPopupWindow::hideEvent(QHideEvent* event) {
    rise_->stop();
    riseOffset_ = 0;
    QWidget::hideEvent(event);
    Q_EMIT closed();
}

void KPopupWindow::leaveEvent(QEvent* event) {
    // Deliberately does NOT close: a detached popup stays open until a choice, an outside press or
    // Esc. This override exists so that intent is written down where a close() might be added.
    QWidget::leaveEvent(event);
}

void KPopupWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutContent();
}

} // namespace bloom::ui::kit
