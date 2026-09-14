#include <bloom/ui/kit/panel_switcher.hpp>

#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardItem>
#include <QStandardItemModel>

#include <algorithm>

namespace bloom::ui::kit {
namespace {

// task U8, issue 131, formal amendment 2, A7: the same left/right inset and label-to-glyph gap
// convention KDropdown's own closed field already established (Spacing::M ends, Spacing::S
// between the label and the trailing glyph) -- A7's crop asks for an identical "gap 8" between
// the icon and the label, so the icon block reuses that same convention on its own leading edge.
[[nodiscard]] int fieldEndInset() { return px(Spacing::M); }
[[nodiscard]] int contentGap() { return px(Spacing::S); }
[[nodiscard]] int chevronBoxWidth() { return px(Size::IconSmall); }
[[nodiscard]] int panelIconBoxWidth() { return px(Size::IconMedium); }

} // namespace

KPanelSwitcher::KPanelSwitcher(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kPanelSwitcher"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    // task U8, formal amendment 2, A8: Type::UI at natural case -- the switcher never takes
    // UiSmall's uppercase transform.
    setFont(kit::font(TypeRole::Ui));

    model_ = new QStandardItemModel(this);
    popup_ = new KDropdownPopup(this);
    popup_->setModel(model_);
    popup_->view()->setIconSize(QSize(panelIconBoxWidth(), panelIconBoxWidth()));
    connect(popup_, &KDropdownPopup::itemChosen, this, [this](const int index) {
        commitIndex(index);
        update();
    });
}

KPanelSwitcher::~KPanelSwitcher() = default;

int KPanelSwitcher::addItem(const QIcon& icon, const QString& text, const QVariant& data) {
    auto* item = new QStandardItem(text);
    item->setIcon(icon);
    item->setData(data, Qt::UserRole + 1);
    item->setEditable(false);
    model_->appendRow(item);
    const int index = model_->rowCount() - 1;
    if (currentIndex_ < 0) {
        commitIndex(index);
    }
    updateGeometry();
    return index;
}

int KPanelSwitcher::count() const { return model_->rowCount(); }

QString KPanelSwitcher::itemText(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QString{} : item->text();
}

QVariant KPanelSwitcher::itemData(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QVariant{} : item->data(Qt::UserRole + 1);
}

QIcon KPanelSwitcher::itemIcon(const int index) const {
    const auto* item = model_->item(index);
    return item == nullptr ? QIcon{} : item->icon();
}

void KPanelSwitcher::setItemToolTip(const int index, const QString& toolTip) {
    auto* item = model_->item(index);
    if (item == nullptr) {
        return;
    }
    item->setToolTip(toolTip);
    if (index == currentIndex_) {
        setToolTip(toolTip);
    }
}

int KPanelSwitcher::findData(const QVariant& data) const {
    for (int index = 0; index < model_->rowCount(); ++index) {
        if (itemData(index) == data) {
            return index;
        }
    }
    return -1;
}

int KPanelSwitcher::currentIndex() const { return currentIndex_; }

void KPanelSwitcher::setCurrentIndex(const int index) { commitIndex(index); }

void KPanelSwitcher::commitIndex(const int index) {
    if (index < 0 || index >= model_->rowCount() || currentIndex_ == index) {
        return;
    }
    currentIndex_ = index;
    // Mirrors QComboBox's own current-item tooltip forwarding (the behavior EditorArea's
    // "unavailable editor" placeholder relied on): the closed field's own tooltip tracks
    // whatever the newly current item's stored tooltip is, blank or not.
    setToolTip(model_->item(index)->toolTip());
    updateGeometry();
    update();
    Q_EMIT currentIndexChanged(index);
}

QVariant KPanelSwitcher::currentData() const { return itemData(currentIndex_); }

void KPanelSwitcher::showPopup() {
    if (model_->rowCount() == 0) {
        return;
    }
    popup_->openBelow(*this, currentIndex_);
    update();
}

void KPanelSwitcher::hidePopup() { popup_->close(); }

bool KPanelSwitcher::isPopupVisible() const { return popup_->isVisible(); }

State KPanelSwitcher::visualState() const {
    if (!isEnabled()) {
        return State::Disabled;
    }
    if (isPopupVisible()) {
        return State::Pressed;
    }
    if (hovered_) {
        return State::Hover;
    }
    if (hasFocus()) {
        return State::Focused;
    }
    return State::Normal;
}

Color KPanelSwitcher::borderToken() const {
    return borderForInteraction(isEnabled(), isPopupVisible() || hasFocus(), hovered_);
}

QIcon KPanelSwitcher::currentIcon() const {
    const auto* item = model_->item(currentIndex_);
    return item == nullptr ? QIcon{} : item->icon();
}

int KPanelSwitcher::controlExtent() const {
    // task U8, formal amendment 2, A10: the switcher field is ControlRoomy (32), not the dense
    // chrome height every other header control uses.
    return px(Size::ControlRoomy);
}

QSize KPanelSwitcher::sizeHint() const {
    const QFontMetrics metrics(font());
    int widest = 0;
    bool anyIcon = false;
    for (int index = 0; index < model_->rowCount(); ++index) {
        widest = std::max(widest, metrics.horizontalAdvance(itemText(index)));
        anyIcon = anyIcon || !model_->item(index)->icon().isNull();
    }
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    // task U8, formal amendment 2, A7: hugs its content -- [icon][gap][label][gap][chevron] --
    // rather than expanding to fill the header row (that expansion was the "field stretched"
    // defect the owner rejected).
    int width = fieldEndInset() + widest + contentGap() + chevronBoxWidth() + fieldEndInset();
    if (anyIcon) {
        width += panelIconBoxWidth() + contentGap();
    }
    return {width + ringMargin, controlExtent() + ringMargin};
}

QSize KPanelSwitcher::minimumSizeHint() const {
    const auto ringMargin = static_cast<int>(std::lround(kFocusRingWidth)) * 2;
    return {fieldEndInset() * 2 + chevronBoxWidth() + ringMargin, controlExtent() + ringMargin};
}

void KPanelSwitcher::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        isPopupVisible() ? hidePopup() : showPopup();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KPanelSwitcher::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Down:
        showPopup();
        event->accept();
        return;
    case Qt::Key_Escape:
        if (isPopupVisible()) {
            hidePopup();
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void KPanelSwitcher::enterEvent(QEnterEvent* event) {
    if (isEnabled()) {
        hovered_ = true;
        update();
    }
    QWidget::enterEvent(event);
}

void KPanelSwitcher::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void KPanelSwitcher::changeEvent(QEvent* event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hovered_ = false;
    }
    QWidget::changeEvent(event);
}

void KPanelSwitcher::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const State state = visualState();
    const auto ringMargin = kFocusRingWidth;
    const QRectF bounds = QRectF(rect()).adjusted(ringMargin, ringMargin, -ringMargin, -ringMargin);

    // task U8, formal amendment 2, A7/A9: bordered ControlSurface field, Radius::Small -- the
    // SAME state recipe every kit control resolves through (Border at rest, BorderHover on
    // hover), never QSS.
    fillRoundedSurface(painter, bounds, color(surfaceForState(Color::ControlSurface, state)),
                       color(borderToken()), Radius::Small);

    const QColor ink = inkForState(Color::Foreground, state);
    const auto chevronWidth = static_cast<qreal>(chevronBoxWidth());
    const QRectF chevronColumn(bounds.right() - fieldEndInset() - chevronWidth, bounds.top(),
                               chevronWidth, bounds.height());
    painter.drawPixmap(QRectF(chevronColumn.left(), chevronColumn.center().y() - chevronWidth / 2.0,
                              chevronWidth, chevronWidth)
                           .toRect(),
                       iconPixmap(IconId::CaretUpDown, Size::IconSmall, ink, devicePixelRatioF(),
                                  iconWeight(IconRole::Chrome)));

    qreal contentLeft = bounds.left() + fieldEndInset();
    const QIcon icon = currentIcon();
    if (!icon.isNull()) {
        const auto iconBox = static_cast<qreal>(panelIconBoxWidth());
        const QRectF iconRect(contentLeft, bounds.center().y() - iconBox / 2.0, iconBox, iconBox);
        painter.drawPixmap(iconRect.toRect(),
                           icon.pixmap(QSize(panelIconBoxWidth(), panelIconBoxWidth())));
        contentLeft = iconRect.right() + contentGap();
    }

    painter.setPen(ink);
    painter.setFont(font());
    const QRectF label(contentLeft, bounds.top(), chevronColumn.left() - contentGap() - contentLeft,
                       bounds.height());
    const QFontMetrics metrics(font());
    const QString elided = metrics.elidedText(itemText(currentIndex_), Qt::ElideRight,
                                              static_cast<int>(label.width()));
    painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft, elided);
}

} // namespace bloom::ui::kit
