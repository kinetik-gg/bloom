#include <bloom/ui/kit/dropdown_popup.hpp>

#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QAbstractItemModel>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLayout>
#include <QListView>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QRegion>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

// The frame's own hairline, in whole logical pixels: the inner list is inset by exactly this much
// so a full-width row can never paint over the border that defines the frame.
[[nodiscard]] int frameBorderWidth() {
    return std::max(1, static_cast<int>(std::lround(kHairlineWidth)));
}

[[nodiscard]] QString popupStyleSheet() {
    // Item rows carry no corner radius on purpose: the hover state is a full-width accent bar, and
    // a rounded bar is not full width.
    return expandTokens(QStringLiteral(R"(
QFrame#kDropdownSurface {
    background: {color.SurfaceRaised};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Small}px;
}
QListView#kDropdownList {
    background: transparent;
    border: none;
    outline: none;
    padding: 0px;
    color: {color.Foreground};
}
QListView#kDropdownList::item {
    padding: {space.XS}px {space.M}px;
    border: none;
    color: {color.Foreground};
}
QListView#kDropdownList::item:hover, QListView#kDropdownList::item:selected {
    background: {color.Accent};
    color: {color.Foreground};
}
QListView#kDropdownList::item:disabled {
    background: transparent;
    color: {color.DisabledInk};
}
)"));
}

} // namespace

// Qt::BypassGraphicsProxyWidget: when the anchor lives inside a QGraphicsProxyWidget (a node
// card row), Qt would otherwise embed this popup into the scene as a sub-proxy, where it lands
// behind the card and never receives the pointer (owner, 2026-09-15: "we can't open dropdown
// inside a node"). The flag keeps it a real top-level popup; mapToGlobal() already maps through
// the proxy and its view.
KDropdownPopup::KDropdownPopup(QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::BypassGraphicsProxyWidget) {
    setObjectName(QStringLiteral("kDropdownPopup"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);

    // The margin is where the elevation's blur lives; without it a drop shadow would be clipped
    // away by the popup's own rectangle.
    const Shadow elevation = shadow(Elevation::Popup);
    const int margin = elevation.blurRadius;
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(margin, margin, margin, margin + elevation.offsetY);
    outer->setSpacing(0);

    surface_ = new QFrame(this);
    surface_->setObjectName(QStringLiteral("kDropdownSurface"));
    surface_->setFrameShape(QFrame::NoFrame);
    applyElevation(*surface_, Elevation::Popup);
    outer->addWidget(surface_);

    auto* inner = new QVBoxLayout(surface_);
    // No margin of its own: QStyleSheetStyle already insets a styled frame's contents by its own
    // border, so the rows live INSIDE the hairline rather than on top of it -- which is what makes
    // the frame, not the row, the rounded container. Adding a margin here would inset them twice.
    inner->setContentsMargins(0, 0, 0, 0);
    inner->setSpacing(0);

    view_ = new QListView(surface_);
    view_->setObjectName(QStringLiteral("kDropdownList"));
    view_->setFrameShape(QFrame::NoFrame);
    view_->setMouseTracking(true);
    view_->setUniformItemSizes(true);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // A value too long for the popup is ellipsized, never allowed to widen the popup past the
    // control it belongs to.
    view_->setTextElideMode(Qt::ElideRight);
    view_->setFont(kit::font(TypeRole::Ui));
    // QAbstractScrollArea's own minimum size hint (56x56 on this Qt) would hold the frame open
    // below its rows, leaving a dead band of SurfaceRaised under the last one -- and a last row
    // that never reaches the bottom edge cannot have its hover bar clipped by the frame's bottom
    // corners at all, which is exactly what this popup's shape is supposed to do. The frame is a
    // wrapper around its items; it has no size of its own to defend.
    view_->setMinimumSize(0, 0);
    inner->addWidget(view_);

    setStyleSheet(popupStyleSheet());
    view_->viewport()->installEventFilter(this);
    // The list is clipped to the frame's inner rounded rectangle every time it is resized, so the
    // accent hover bar on the first and last rows follows the frame's corners instead of squaring
    // them off.
    view_->installEventFilter(this);
}

QFrame* KDropdownPopup::surface() const noexcept { return surface_; }

void KDropdownPopup::applyRoundedListMask() {
    const QSize size = view_->size();
    if (size.isEmpty()) {
        return;
    }
    const auto extent = std::min(size.width(), size.height());
    // The frame's radius, stepped in by the border the list already sits inside: concentric with
    // the frame's own corner rather than a second, differently-curved one.
    const auto corner =
        static_cast<qreal>(std::max(0, radiusPx(Radius::Small, extent) - frameBorderWidth()));
    // Only the edges the list actually shares with the frame are rounded (task S1, item 4). A
    // search popup puts its filter field above the list, so the list's top edge is in the MIDDLE of
    // the surface; rounding it there carved two notches under the field -- a second rounding with
    // nothing behind it to curve around. An edge that must stay square is handled by extending the
    // rounded rectangle past it, so its corners fall outside the widget and only straight sides
    // remain inside.
    const auto* surfaceLayout = surface_->layout();
    const int index = surfaceLayout == nullptr ? 0 : surfaceLayout->indexOf(view_);
    const int last = surfaceLayout == nullptr ? 0 : surfaceLayout->count() - 1;
    const qreal top = index <= 0 ? 0.0 : -corner;
    const qreal bottom = index >= last ? 0.0 : corner;
    QPainterPath path;
    path.addRoundedRect(QRectF(QPointF(0.0, 0.0), QSizeF(size)).adjusted(0.0, top, 0.0, bottom),
                        corner, corner);
    view_->setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void KDropdownPopup::setModel(QAbstractItemModel* model) { view_->setModel(model); }

QListView* KDropdownPopup::view() const noexcept { return view_; }

void KDropdownPopup::openBelow(const QWidget& anchor, const int currentIndex) {
    if (view_->model() != nullptr && currentIndex >= 0 &&
        currentIndex < view_->model()->rowCount()) {
        view_->setCurrentIndex(view_->model()->index(currentIndex, 0));
    }

    const Shadow elevation = shadow(Elevation::Popup);
    const int margin = elevation.blurRadius;
    const int rows = view_->model() == nullptr ? 0 : view_->model()->rowCount();
    const int rowHeight = std::max(px(Size::ControlCompact), view_->sizeHintForRow(0));
    // Exactly the rows, plus the frame's own hairline on each side. No list padding: a row that
    // stopped short of the frame edge would leave a sliver of SurfaceRaised above the first hover
    // bar, and the bar would no longer read as reaching the frame's rounded corner.
    //
    // The height is pinned on the view rather than merely requested, because
    // QAbstractScrollArea's own minimum size hint (56x56 on this Qt) would otherwise hold the
    // frame open below its rows: a dead band of SurfaceRaised under the last one, whose hover bar
    // could then never reach -- let alone be clipped by -- the frame's bottom corners.
    const int rowsHeight = std::max(rowHeight, rows * rowHeight);
    view_->setFixedHeight(rowsHeight);
    const int listHeight = rowsHeight + frameBorderWidth() * 2;

    const QPoint anchorBottomLeft = anchor.mapToGlobal(QPoint(0, anchor.height()));
    const QSize outerSize(anchor.width() + margin * 2, listHeight + margin * 2 + elevation.offsetY);
    resize(outerSize);

    const QPoint finalPosition(anchorBottomLeft.x() - margin,
                               anchorBottomLeft.y() - margin + px(Spacing::XXS));
    const int riseMs = durationMs(Motion::Pop);
    if (riseMs <= 0) {
        // Reduced motion: straight to the end state, no rise.
        move(finalPosition);
        show();
        return;
    }
    move(finalPosition + QPoint(0, kPopRisePx));
    show();
    auto* rise = new QPropertyAnimation(this, "pos", this);
    rise->setDuration(riseMs);
    rise->setEasingCurve(easing(Motion::Pop));
    rise->setStartValue(finalPosition + QPoint(0, kPopRisePx));
    rise->setEndValue(finalPosition);
    rise->start(QAbstractAnimation::DeleteWhenStopped);
}

void KDropdownPopup::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KDropdownPopup::mousePressEvent(QMouseEvent* event) {
    // Qt delivers a click anywhere on screen to the active popup, with a position that may fall
    // outside this widget entirely. A press outside the FRAME -- including the transparent shadow
    // gutter around it -- dismisses; a press on the frame is the list's own business.
    if (!surface_->geometry().contains(event->position().toPoint())) {
        close();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KDropdownPopup::leaveEvent(QEvent* event) {
    // Deliberately does NOT close. A detached popup stays open until the artist chooses an item,
    // clicks outside it, or presses Esc; moving the pointer away is none of those. This override
    // exists so that intent is written down where someone would otherwise add a close() call.
    QWidget::leaveEvent(event);
}

bool KDropdownPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_ && event->type() == QEvent::Resize) {
        applyRoundedListMask();
        return false;
    }
    if (watched == view_ && event->type() == QEvent::KeyPress) {
        // The list keeps keyboard focus while the popup is open, so Esc arrives here rather than at
        // the popup itself on some platforms.
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        return false;
    }
    if (watched == view_->viewport() && event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QModelIndex index = view_->indexAt(mouse->pos());
        // A disabled row swallows the click rather than closing the popup on a value the artist
        // cannot have.
        if (index.isValid() && (index.flags() & Qt::ItemIsEnabled) != 0) {
            close();
            Q_EMIT itemChosen(index.row());
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace bloom::ui::kit
