#include <QButtonGroup>
#include <QGridLayout>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWidgetAction>
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
void KIconButton::paintEvent(QPaintEvent* event) {
    if (!isChecked()) {
        QToolButton::paintEvent(event);
        return;
    }
    QPainter painter(this);
    fillRoundedSurface(painter, rect(), color(isDown() ? Color::AccentPressed : Color::Accent), {},
                       Radius::Small);
    auto glyph = icon().pixmap(iconSize(), devicePixelRatioF(), QIcon::Normal, QIcon::On);
    QPainter ink(&glyph);
    ink.setCompositionMode(QPainter::CompositionMode_SourceIn);
    ink.fillRect(glyph.rect(), isEnabled() ? color(Color::OnAccent)
                                           : withOpacity(color(Color::OnAccent), kDisabledOpacity));
    ink.end();
    const auto extent = glyph.deviceIndependentSize();
    painter.drawPixmap(QPointF((width() - extent.width()) / 2, (height() - extent.height()) / 2),
                       glyph);
    painter.setPen(color(Color::OnAccent));
    if (icon().isNull())
        painter.drawText(rect(), Qt::AlignCenter, text());
}
KIconToggle::KIconToggle(IconId id, QWidget* parent) : KIconButton(parent), glyph_(id) {
    setCheckable(true);
    setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
    ensureKeyboardFocusTracking(*this);
}
void KIconToggle::setGlyph(IconId id) {
    glyph_ = id;
    update();
}
QPixmap KIconToggle::glyphPixmap() const {
    if (property("toolChoice").toBool() && isChecked())
        return iconPixmap(glyph_, Size::IconControl, Color::OnAccent, State::Normal,
                          IconWeight::Fill, devicePixelRatioF());
    return iconPixmap(glyph_, Size::IconControl, Color::Muted,
                      !isEnabled()  ? State::Disabled
                      : isChecked() ? State::Selected
                                    : State::Normal,
                      isChecked() ? IconWeight::Fill : IconWeight::Regular, devicePixelRatioF());
}
void KIconToggle::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QRectF box = rect();
    fillRoundedSurface(
        painter, box,
        color(property("toolChoice").toBool() && isChecked() ? Color::Accent : Color::Surface),
        color(borderForInteraction(isEnabled(), hasKeyboardFocus(*this), underMouse())),
        Radius::Small);
    const auto glyph = glyphPixmap();
    const auto size = glyph.deviceIndependentSize();
    const qreal dpr = devicePixelRatioF();
    const QPointF origin(std::round((width() - size.width()) * dpr / 2) / dpr,
                         std::round((height() - size.height()) * dpr / 2) / dpr);
    painter.drawPixmap(origin, glyph);
}
KToolColumn::KToolColumn(QWidget* parent)
    : QWidget(parent), column_(new QVBoxLayout(this)), group_(new QButtonGroup(this)) {
    setFixedWidth(px(Size::ToolColumnWidth));
    column_->setContentsMargins(px(Spacing::ChromePadding), px(Spacing::ChromePadding),
                                px(Spacing::ChromePadding), px(Spacing::ChromePadding));
    column_->setSpacing(px(Spacing::ChromeGap));
    setAutoFillBackground(true);
    auto colors = palette();
    colors.setColor(QPalette::Window, color(Color::Surface));
    setPalette(colors);
    group_->setExclusive(true);
}
KIconToggle* KToolColumn::addTool(IconId id, const QString& label, const QString& objectName,
                                  bool enabled) {
    auto* button = new KIconToggle(id, this);
    button->setObjectName(objectName);
    button->setAccessibleName(label);
    button->setToolTip(label);
    button->setProperty("toolChoice", true);
    button->setFixedSize(px(Size::ToggleCell), px(Size::ToggleCell));
    button->setEnabled(enabled);
    group_->addButton(button);
    column_->addWidget(button, 0, Qt::AlignHCenter);
    return button;
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
KLineEdit::KLineEdit(QWidget* parent) : KLineEdit(QString{}, parent) {}
KLineEdit::KLineEdit(const QString& text, QWidget* parent) : QLineEdit(text, parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setProperty("kitControl", true);
}
KSearchField::KSearchField(QWidget* parent) : KLineEdit(parent) {}
namespace {
class FlowMenu final : public QMenu {
  public:
    FlowMenu(const QString& title, QWidget* parent) : QMenu(title, parent) {
        connect(this, &QMenu::aboutToShow, this, [this] { buildColumns(); });
        connect(this, &QMenu::aboutToHide, this, [this] { restoreActions(); });
    }

  protected:
    void showEvent(QShowEvent* event) override {
        QMenu::showEvent(event);
        if (!property("columnFlow").toBool())
            return;
        const auto bounds = ownerBounds();
        move(std::clamp(x(), bounds.left(), std::max(bounds.left(), bounds.right() - width() + 1)),
             std::clamp(y(), bounds.top(), std::max(bounds.top(), bounds.bottom() - height() + 1)));
    }

  private:
    QRect ownerBounds() const {
        auto* owner = parentWidget();
        while (owner && qobject_cast<QMenu*>(owner))
            owner = owner->parentWidget();
        if (!owner)
            return geometry();
        owner = owner->window();
        return {owner->mapToGlobal(QPoint()), owner->size()};
    }
    void restoreActions() {
        if (columns_) {
            removeAction(columns_);
            columns_->deleteLater();
            columns_ = nullptr;
        }
        for (auto* action : originals_)
            action->setVisible(true);
        originals_.clear();
    }
    void buildColumns() {
        if (!property("columnFlow").toBool())
            return;
        restoreActions();
        const auto bounds = ownerBounds();
        const int padding = px(Spacing::ChromePadding);
        const int cap = static_cast<int>(bounds.height() * kMenuWindowHeightShare);
        const int rows = std::max(1, (cap - 4 * padding) / px(Size::Control));
        auto* content = new QWidget;
        auto* grid = new QGridLayout(content);
        grid->setContentsMargins(padding, padding, padding, padding);
        grid->setSpacing(0);
        for (auto* action : actions()) {
            if (!action->isVisible() || action->isSeparator())
                continue;
            auto* button = new KMenuButton(content);
            button->setDefaultAction(action);
            button->setFixedWidth(std::max(px(Size::MenuMinWidth), button->sizeHint().width()));
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setFocusPolicy(Qt::StrongFocus);
            const int index = static_cast<int>(originals_.size());
            grid->addWidget(button, index % rows, index / rows);
            connect(button, &QToolButton::clicked, this, [this] {
                for (auto* menu = static_cast<QMenu*>(this); menu;
                     menu = qobject_cast<QMenu*>(menu->parentWidget()))
                    menu->close();
            });
            originals_.push_back(action);
        }
        for (auto* action : originals_)
            action->setVisible(false);
        // Buttons hold the original actions and retain their enabled state and shortcuts.
        for (auto* button : content->findChildren<KMenuButton*>())
            button->show();
        columns_ = new QWidgetAction(this);
        columns_->setDefaultWidget(content);
        addAction(columns_);
        setProperty("flowColumns", grid->columnCount());
        setProperty("flowHeightCap", cap);
    }
    QList<QAction*> originals_;
    QWidgetAction* columns_ = nullptr;
};
} // namespace
QMenu* makeMenu(QWidget* parent) { return new FlowMenu({}, parent); }
QMenu* makeMenu(const QString& title, QWidget* parent) { return new FlowMenu(title, parent); }
} // namespace bloom::ui::kit
