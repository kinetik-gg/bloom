#include <bloom/ui/kit/section.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
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
    button->setControlSize(KButton::ControlSize::Compact);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

KSection::KSection(const QString& title, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kSection"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(px(Spacing::XXS));

    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("kSectionHeader"));
    header_->setMinimumHeight(px(Size::ControlCompact));
    header_->setCursor(Qt::PointingHandCursor);
    // The whole header toggles, not just the chevron: the chevron is the affordance, the header is
    // the target, which is how every twirl-down in the application already behaves.
    header_->installEventFilter(this);
    auto* headerLayout = new QHBoxLayout(header_);
    headerLayout->setContentsMargins(0, 0, 0, 0);
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
    title_->setFont(kit::font(TypeRole::Ui));
    QPalette titlePalette = title_->palette();
    titlePalette.setColor(QPalette::WindowText, color(Color::Foreground));
    title_->setPalette(titlePalette);
    headerLayout->addWidget(title_, 1);

    reset_ = makeHeaderButton(header_, QStringLiteral("kSectionReset"));
    reset_->setText(tr("Reset"));
    connect(reset_, &KButton::clicked, this, &KSection::resetRequested);
    headerLayout->addWidget(reset_);

    menu_ = makeHeaderButton(header_, QStringLiteral("kSectionMenu"));
    menu_->setIconId(IconId::ContextMenu);
    connect(menu_, &KButton::clicked, this, &KSection::showSectionMenu);
    headerLayout->addWidget(menu_);

    layout->addWidget(header_);

    body_ = new QWidget(this);
    body_->setObjectName(QStringLiteral("kSectionBody"));
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(0, 0, 0, 0);
    bodyLayout_->setSpacing(px(Spacing::XS));
    layout->addWidget(body_);
}

QString KSection::title() const { return title_->toolTip(); }

void KSection::setTitle(const QString& title) {
    title_->setToolTip(title);
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

void KSection::applyCollapsedState() {
    body_->setVisible(!collapsed_);
    chevron_->setIconId(collapsed_ ? IconId::CaretRight : IconId::CaretDown);
}

void KSection::showSectionMenu() {
    QMenu menu(this);
    auto* reset = menu.addAction(tr("Reset"));
    connect(reset, &QAction::triggered, this, &KSection::resetRequested);
    menu.addSeparator();
    auto* collapseAll = menu.addAction(tr("Collapse all"));
    connect(collapseAll, &QAction::triggered, this, &KSection::collapseAllRequested);
    auto* expandAll = menu.addAction(tr("Expand all"));
    connect(expandAll, &QAction::triggered, this, &KSection::expandAllRequested);
    menu.exec(menu_->mapToGlobal(QPoint(0, menu_->height())));
}

bool KSection::eventFilter(QObject* watched, QEvent* event) {
    if (watched == title_ && event->type() == QEvent::Resize)
        setTitle(title());
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
