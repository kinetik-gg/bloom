#include <bloom/ui/kit/tab_bar.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui::kit {

KTabBar::KTabBar(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    ensureKeyboardFocusTracking(*this);
    setObjectName(QStringLiteral("kTabBar"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setFont(kit::font(TypeRole::Ui));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

int KTabBar::addTab(const QString& text) {
    tabs_.push_back({text, false});
    if (current_ < 0)
        current_ = 0;
    updateGeometry();
    update();
    return count() - 1;
}

QString KTabBar::tabText(const int index) const {
    return index >= 0 && index < count() ? tabs_[index].text : QString{};
}

void KTabBar::setCurrentIndex(const int index) {
    if (index < 0 || index >= count() || index == current_)
        return;
    current_ = index;
    setAccessibleDescription(tabText(index));
    update();
    emit currentChanged(index);
}

void KTabBar::setTabAlert(const int index, const bool alert) {
    if (index < 0 || index >= count() || tabs_[index].alert == alert)
        return;
    tabs_[index].alert = alert;
    updateGeometry();
    update();
}

bool KTabBar::tabAlert(const int index) const {
    return index >= 0 && index < count() && tabs_[index].alert;
}

int KTabBar::tabWidth(const int index) const {
    const QFontMetrics metrics(font());
    int width = metrics.horizontalAdvance(tabs_[index].text) + 2 * px(Spacing::M);
    // The alert dot always has its room, so raising one never moves the next tab.
    width += px(Size::NonDefaultMark) + px(Spacing::XS);
    return width;
}

QRect KTabBar::tabRect(const int index) const {
    if (index < 0 || index >= count())
        return {};
    int x = 0;
    for (int before = 0; before < index; ++before)
        x += tabWidth(before);
    return {x, 0, tabWidth(index), height()};
}

int KTabBar::tabAt(const QPoint point) const {
    for (int index = 0; index < count(); ++index)
        if (tabRect(index).contains(point))
            return index;
    return -1;
}

QSize KTabBar::sizeHint() const {
    int width = 0;
    for (int index = 0; index < count(); ++index)
        width += tabWidth(index);
    return {width, px(Size::HeaderRow)};
}

QSize KTabBar::minimumSizeHint() const { return sizeHint(); }

void KTabBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // The hairline the tabs stand on.
    applyHairlinePen(painter, color(Color::Border));
    painter.drawLine(QPointF(0, height() - 0.5), QPointF(width(), height() - 0.5));
    const QFontMetrics metrics(font());
    for (int index = 0; index < count(); ++index) {
        const QRect tab = tabRect(index);
        const bool current = index == current_;
        QColor ink = color(current || index == hovered_ ? Color::Foreground : Color::Muted);
        if (!isEnabled())
            ink = disabledInk(ink);
        painter.setPen(ink);
        const QRect text = tab.adjusted(
            px(Spacing::M), 0, -(px(Spacing::M) + px(Size::NonDefaultMark) + px(Spacing::XS)), 0);
        painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, tabs_[index].text);
        if (tabs_[index].alert) {
            const int dot = px(Size::NonDefaultMark);
            const QRectF mark(text.left() + metrics.horizontalAdvance(tabs_[index].text) +
                                  px(Spacing::XS),
                              tab.center().y() - dot / 2.0, dot, dot);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color(Color::Error));
            painter.drawEllipse(mark);
        }
        if (current) {
            const int edge = px(Size::SelectionEdge);
            painter.fillRect(QRect(tab.left(), height() - edge, tab.width(), edge),
                             color(Color::Accent));
            if (hasKeyboardFocus(*this))
                fillRoundedSurface(painter, controlBounds(*this).toRect().intersected(tab),
                                   Qt::transparent, color(Color::Accent), Radius::Small);
        }
    }
}

void KTabBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    if (const int index = tabAt(event->position().toPoint()); index >= 0)
        setCurrentIndex(index);
    event->accept();
}

void KTabBar::mouseMoveEvent(QMouseEvent* event) {
    const int index = tabAt(event->position().toPoint());
    if (index != hovered_) {
        hovered_ = index;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void KTabBar::leaveEvent(QEvent* event) {
    hovered_ = -1;
    update();
    QWidget::leaveEvent(event);
}

void KTabBar::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Left:
        setCurrentIndex(std::max(0, current_ - 1));
        break;
    case Qt::Key_Right:
        setCurrentIndex(std::min(count() - 1, current_ + 1));
        break;
    case Qt::Key_Home:
        setCurrentIndex(0);
        break;
    case Qt::Key_End:
        setCurrentIndex(count() - 1);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

KTabs::KTabs(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kTabs"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(px(Spacing::M));
    bar_ = new KTabBar(this);
    layout->addWidget(bar_);
    // A stacked widget's size hint is its largest page's, hidden pages included: the tallest tab
    // sets the size, so switching never resizes the dialog.
    stack_ = new QStackedWidget(this);
    layout->addWidget(stack_, 1);
    connect(bar_, &KTabBar::currentChanged, this, [this](const int index) {
        stack_->setCurrentIndex(index);
        emit currentChanged(index);
    });
}

int KTabs::addPage(const QString& title, QWidget* page) {
    stack_->addWidget(page);
    return bar_->addTab(title);
}

int KTabs::currentIndex() const { return bar_->currentIndex(); }

void KTabs::setCurrentIndex(const int index) { bar_->setCurrentIndex(index); }

int KTabs::pageOf(const QWidget* widget) const {
    for (int index = 0; index < stack_->count(); ++index) {
        const auto* page = stack_->widget(index);
        if (page == widget || page->isAncestorOf(widget))
            return index;
    }
    return -1;
}

void KTabs::keyPressEvent(QKeyEvent* event) {
    // Ctrl+Tab reaches here from any field on a page: fields leave it unhandled.
    const bool control = (event->modifiers() & Qt::ControlModifier) != 0;
    if (control && (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab)) {
        const bool back =
            event->key() == Qt::Key_Backtab || (event->modifiers() & Qt::ShiftModifier) != 0;
        const int count = bar_->count();
        if (count > 0)
            bar_->setCurrentIndex((bar_->currentIndex() + (back ? count - 1 : 1)) % count);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace bloom::ui::kit
