#include <QPainter>
#include <QResizeEvent>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
namespace bloom::ui::kit {
KMenuButton::KMenuButton(QWidget* parent) : QToolButton(parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setAutoRaise(true);
    setPopupMode(QToolButton::InstantPopup);
    setProperty("headerMenuButton", true);
    setProperty("kitControl", true);
}
QSize KMenuButton::sizeHint() const {
    auto size = QToolButton::sizeHint();
    size.setHeight(px(Size::Control));
    return size;
}
KIconButton::KIconButton(QWidget* parent) : QToolButton(parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedSize(px(Size::Control), px(Size::Control));
    setIconSize(QSize(px(Size::IconChrome), px(Size::IconChrome)));
    setAutoRaise(true);
    setProperty("kitControl", true);
}
KIconToggle::KIconToggle(IconId id, QWidget* parent) : KIconButton(parent), glyph_(id) {
    setCheckable(true);
    setFixedWidth(px(Size::ToggleCell));
    ensureKeyboardFocusTracking(*this);
}
void KIconToggle::setGlyph(IconId id) {
    glyph_ = id;
    update();
}
QPixmap KIconToggle::glyphPixmap() const {
    return iconPixmap(glyph_, Size::IconControl, Color::Muted,
                      !isEnabled()  ? State::Disabled
                      : isChecked() ? State::Selected
                                    : State::Normal,
                      isChecked() ? IconWeight::Fill : IconWeight::Regular, devicePixelRatioF());
}
void KIconToggle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QRectF box = QRectF(rect()).adjusted(px(Spacing::XXS), px(Spacing::XXS),
                                               -px(Spacing::XXS), -px(Spacing::XXS));
    fillRoundedSurface(
        painter, box, color(Color::ControlSurface),
        color(borderForInteraction(isEnabled(), hasKeyboardFocus(*this), underMouse())),
        Radius::Small);
    const auto glyph = glyphPixmap();
    const auto size = glyph.deviceIndependentSize();
    const qreal dpr = devicePixelRatioF();
    const QPointF origin(std::round((width() - size.width()) * dpr / 2) / dpr,
                         std::round((height() - size.height()) * dpr / 2) / dpr);
    painter.drawPixmap(origin, glyph);
}
KLabel::KLabel(QWidget* parent) : KLabel(QString{}, parent) {}
KLabel::KLabel(const QString& text, QWidget* parent, TypeRole role) : QLabel(text, parent) {
    setTypeRole(role);
    setFixedHeight(px(Size::Control));
}
void KLabel::setTypeRole(TypeRole role) { setFont(kit::font(role)); }
void KLabel::setElidedText(const QString& text) {
    fullText_ = text;
    elides_ = true;
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setToolTip(text);
    setText(fontMetrics().elidedText(fullText_, Qt::ElideRight, width()));
}
void KLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    if (elides_)
        setText(fontMetrics().elidedText(fullText_, Qt::ElideRight, width()));
}
KSearchField::KSearchField(QWidget* parent) : QLineEdit(parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setProperty("kitControl", true);
}
QMenu* makeMenu(QWidget* parent) { return new QMenu(parent); }
QMenu* makeMenu(const QString& title, QWidget* parent) { return new QMenu(title, parent); }
} // namespace bloom::ui::kit
