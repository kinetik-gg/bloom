#include <bloom/ui/kit/section.hpp>

#include <QPainter>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QSettings>
#include <QVBoxLayout>

namespace bloom::ui::kit {
namespace {

// The header's own chrome buttons are Ghost/Compact: a section header is chrome, so nothing in it
// may carry a resting surface that would compete with the rows below it.
KButton* makeHeaderButton(QWidget* parent, const QString& objectName) {
    auto* button = new KButton(parent);
    button->setObjectName(objectName);
    button->setVariant(KButton::Variant::Ghost);
    button->setFixedSize(px(Size::Control), px(Size::Control));
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

KSection::KSection(const QString& title, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kSection"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(static_cast<int>(kHairlineWidth), static_cast<int>(kHairlineWidth),
                               static_cast<int>(kHairlineWidth), static_cast<int>(kHairlineWidth));
    layout->setSpacing(0);

    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("kSectionHeader"));
    header_->setMinimumHeight(px(Size::Control));
    header_->setCursor(Qt::PointingHandCursor);
    // The header is the keyboard target too: Tab reaches it, Space or Enter toggles it, and its
    // keyboard focus shows as the section's Accent header border.
    ensureKeyboardFocusTracking(*header_);
    header_->setFocusPolicy(Qt::StrongFocus);
    header_->setAccessibleName(title);
    // The whole header toggles, not just the chevron: the chevron is the affordance, the header is
    // the target, which is how every twirl-down in the application already behaves.
    header_->installEventFilter(this);
    auto* headerLayout = new QHBoxLayout(header_);
    headerLayout->setContentsMargins(px(Spacing::XS), 0, px(Spacing::XS), 0);
    headerLayout->setSpacing(px(Spacing::XS));

    chevron_ = makeHeaderButton(header_, QStringLiteral("kSectionChevron"));
    chevron_->setIconId(IconId::CaretDown);
    connect(chevron_, &KButton::clicked, this, [this] { setCollapsed(!collapsed_); });
    headerLayout->addWidget(chevron_);

    title_ = new QLabel(title, header_);
    title_->setObjectName(QStringLiteral("kSectionTitle"));
    title_->setToolTip(title);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    title_->installEventFilter(this);
    title_->setTextFormat(Qt::PlainText);
    title_->setFont(kit::font(TypeRole::UiSmallStrong));
    QPalette titlePalette = title_->palette();
    titlePalette.setColor(QPalette::WindowText, color(Color::Foreground));
    title_->setPalette(titlePalette);
    headerLayout->addWidget(title_, 1);

    reset_ = makeHeaderButton(header_, QStringLiteral("kSectionReset"));
    reset_->setIconId(IconId::Reset);
    reset_->setToolTip(tr("Reset"));
    reset_->setAccessibleName(tr("Reset"));
    connect(reset_, &KButton::clicked, this, &KSection::resetRequested);
    headerLayout->addWidget(reset_);

    layout->addWidget(header_);

    body_ = new QWidget(this);
    body_->setObjectName(QStringLiteral("kSectionBody"));
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(px(Spacing::SectionPadding), px(Spacing::SectionPadding),
                                    px(Spacing::SectionPadding), px(Spacing::SectionPadding));
    bodyLayout_->setSpacing(0); // KPropertyRow already owns the complete row pitch.
    layout->addWidget(body_);
}

void KSection::addHeaderAction(QWidget* action) {
    auto* layout = qobject_cast<QHBoxLayout*>(header_->layout());
    layout->insertWidget(layout->indexOf(reset_), action);
}

void KSection::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    fillRoundedSurface(painter, QRectF(rect()), color(Color::Surface), color(Color::Border),
                       Radius::Panel);
    if (hasKeyboardFocus(*header_)) {
        fillRoundedSurface(painter, QRectF(header_->geometry()), {}, color(Color::Accent),
                           Radius::Small);
    }
}

bool KSection::headerHasKeyboardFocus() const { return hasKeyboardFocus(*header_); }

QWidget* KSection::header() const noexcept { return header_; }

QString KSection::title() const { return title_->toolTip(); }

void KSection::setTitle(const QString& title) {
    title_->setToolTip(title);
    header_->setAccessibleName(title);
    title_->setText(
        QFontMetrics(title_->font()).elidedText(title, Qt::ElideRight, title_->width()));
}

QWidget* KSection::body() const noexcept { return body_; }

QVBoxLayout* KSection::bodyLayout() const noexcept { return bodyLayout_; }

bool KSection::isCollapsed() const noexcept { return collapsed_; }

void KSection::setCollapsed(const bool collapsed) {
    if (collapsed_ == collapsed) {
        return;
    }
    collapsed_ = collapsed;
    applyCollapsedState();
    if (!persistenceKey_.isEmpty()) {
        QSettings().setValue(persistenceKey_, collapsed_);
    }
    Q_EMIT collapsedChanged(collapsed_);
}

void KSection::setPersistenceKey(const QString& key) {
    persistenceKey_ = key;
    if (persistenceKey_.isEmpty()) {
        return;
    }
    const bool stored = QSettings().value(persistenceKey_, false).toBool();
    if (stored != collapsed_) {
        collapsed_ = stored;
        applyCollapsedState();
        Q_EMIT collapsedChanged(collapsed_);
    }
}

QString KSection::persistenceKey() const { return persistenceKey_; }

void KSection::setResetEnabled(const bool enabled) {
    resetEnabled_ = enabled;
    reset_->setVisible(enabled);
}

bool KSection::isResetEnabled() const noexcept { return resetEnabled_; }

void KSection::setBodyPadding(const int padding) {
    bodyLayout_->setContentsMargins(padding, padding, padding, padding);
}

void KSection::applyCollapsedState() {
    body_->setVisible(!collapsed_);
    chevron_->setIconId(collapsed_ ? IconId::CaretRight : IconId::CaretDown);
}

bool KSection::eventFilter(QObject* watched, QEvent* event) {
    if (watched == title_ && event->type() == QEvent::Resize)
        setTitle(title());
    if (watched == header_ &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        update();
    }
    if (watched == header_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter) {
            setCollapsed(!collapsed_);
            return true;
        }
    }
    if (watched == header_ && event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        // Only a plain left click on the header's own background toggles; a click that landed on
        // one of the header's buttons was already consumed by that button and never reaches here.
        if (mouse->button() == Qt::LeftButton) {
            setCollapsed(!collapsed_);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace bloom::ui::kit
