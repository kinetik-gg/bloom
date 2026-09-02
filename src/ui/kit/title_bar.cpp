#include <bloom/ui/kit/title_bar.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QWindow>

namespace bloom::ui::kit {
namespace {

KButton* makeChromeButton(const IconId iconId, const QString& toolTip, const QString& objectName,
                          QWidget* parent) {
    auto* button = new KButton(iconId, QString{}, parent);
    button->setObjectName(objectName);
    button->setVariant(KButton::Variant::Ghost);
    button->setControlSize(KButton::ControlSize::Default);
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kinetikTitleBar"));
    setFixedHeight(px(Size::TitleBar));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    titleLabel_ = new QLabel(this);
    titleLabel_->setObjectName(QStringLiteral("titleBarTitleLabel"));
    titleLabel_->setFont(kit::font(TypeRole::Title));

    minimizeButton_ = makeChromeButton(IconId::Minimize, QStringLiteral("Minimize"),
                                       QStringLiteral("titleBarMinimizeButton"), this);
    maximizeButton_ = makeChromeButton(IconId::Maximize, QStringLiteral("Maximize"),
                                       QStringLiteral("titleBarMaximizeButton"), this);
    closeButton_ = makeChromeButton(IconId::Close, QStringLiteral("Close"),
                                    QStringLiteral("titleBarCloseButton"), this);
    // Standard convention (decision 1, issue #118): the close control alone commits to Error fill
    // once hovered, while resting flush with its ghost siblings.
    closeButton_->setDangerOnHover(true);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(px(Spacing::M), 0, px(Spacing::XS), 0);
    layout_->setSpacing(px(Spacing::S));
    layout_->addWidget(titleLabel_);
    menuBarSlot_ = layout_->count();
    layout_->addStretch(1);
    layout_->addWidget(minimizeButton_);
    layout_->addWidget(maximizeButton_);
    layout_->addWidget(closeButton_);

    connect(minimizeButton_, &KButton::clicked, this, &TitleBar::minimizeRequested);
    connect(maximizeButton_, &KButton::clicked, this, &TitleBar::maximizeOrRestoreRequested);
    connect(closeButton_, &KButton::clicked, this, &TitleBar::closeRequested);
}

void TitleBar::setTitle(const QString& title) { titleLabel_->setText(title); }

void TitleBar::setMenuBar(QMenuBar* menuBar) {
    if (menuBar_ == menuBar) {
        return;
    }
    if (menuBar_ != nullptr) {
        layout_->removeWidget(menuBar_);
    }
    menuBar_ = menuBar;
    if (menuBar_ != nullptr) {
        layout_->insertWidget(menuBarSlot_, menuBar_);
    }
}

void TitleBar::setMaximized(const bool maximized) {
    if (maximized_ == maximized) {
        return;
    }
    maximized_ = maximized;
    maximizeButton_->setIconId(maximized_ ? IconId::Restore : IconId::Maximize);
    const QString toolTip =
        maximized_ ? QStringLiteral("Restore") : QStringLiteral("Maximize");
    maximizeButton_->setToolTip(toolTip);
    maximizeButton_->setAccessibleName(toolTip);
}

bool TitleBar::maximizedAppearance() const noexcept { return maximized_; }

void TitleBar::mousePressEvent(QMouseEvent* event) {
    // Only ever reached for the empty bar area: the label, the embedded menu bar, and the three
    // buttons all claim their own mouse events first.
    if (event->button() == Qt::LeftButton) {
        if (auto* handle = window()->windowHandle(); handle != nullptr) {
            handle->startSystemMove();
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        Q_EMIT maximizeOrRestoreRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace bloom::ui::kit
