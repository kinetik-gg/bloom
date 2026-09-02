#include <bloom/ui/kit/dropdown_popup.hpp>

#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QAbstractItemModel>
#include <QEvent>
#include <QFrame>
#include <QListView>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

[[nodiscard]] QString popupStyleSheet() {
    // Item rows carry no corner radius on purpose: the hover state is a full-width accent bar, and
    // a rounded bar is not full width.
    return expandTokens(QStringLiteral(R"(
QFrame#kDropdownSurface {
    background: {color.SurfaceRaised};
    border: {border.Hairline}px solid {color.Border};
    border-radius: {radius.Medium}px;
}
QListView#kDropdownList {
    background: transparent;
    border: none;
    outline: none;
    padding: {space.XXS}px 0px;
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

KDropdownPopup::KDropdownPopup(QWidget* parent) : QWidget(parent, Qt::Popup) {
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
    inner->addWidget(view_);

    setStyleSheet(popupStyleSheet());
    view_->viewport()->installEventFilter(this);
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
    const int listHeight = std::max(rowHeight, rows * rowHeight) + px(Spacing::XS);

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

bool KDropdownPopup::eventFilter(QObject* watched, QEvent* event) {
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
