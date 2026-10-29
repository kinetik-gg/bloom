#include <bloom/ui/kit/text_field.hpp>

#include <bloom/ui/kit/icons.hpp>

#include <QAction>
#include <QEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOptionFrame>

namespace bloom::ui::kit {
namespace {

void repolish(QWidget& widget) {
    widget.style()->unpolish(&widget);
    widget.style()->polish(&widget);
    widget.update();
}

// Placeholder text is a hint, not a value: Faint ink, in both the line and multi-line forms.
void applyPlaceholderInk(QWidget& widget) {
    QPalette palette = widget.palette();
    palette.setColor(QPalette::PlaceholderText, color(Color::Faint));
    widget.setPalette(palette);
}

void applyError(QWidget& widget, QString& restingToolTip, const QString& reason) {
    const bool had = widget.property("kitError").toBool();
    if (!had && !reason.isEmpty())
        restingToolTip = widget.toolTip();
    widget.setProperty("kitError", !reason.isEmpty());
    widget.setToolTip(reason.isEmpty() ? restingToolTip : reason);
    widget.setAccessibleDescription(reason);
    if (had != !reason.isEmpty())
        repolish(widget);
}

} // namespace

KTextField::KTextField(QWidget* parent) : KTextField(QString{}, parent) {}

KTextField::KTextField(const QString& text, QWidget* parent) : QLineEdit(text, parent) {
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setProperty("kitControl", true);
    applyPlaceholderInk(*this);
}

void KTextField::setTypeRole(const TypeRole role) {
    role_ = role;
    setFont(kit::font(role));
}

void KTextField::setError(const QString& reason) { applyError(*this, restingToolTip_, reason); }

QString KTextField::error() const {
    return property("kitError").toBool() ? toolTip() : QString{};
}

void KTextField::setEmbedded(const bool embedded) {
    embedded_ = embedded;
    setProperty("kitControl", !embedded);
    setProperty("kitEmbedded", embedded);
    setFrame(!embedded);
    if (embedded) {
        // The host sizes an embedded editor to its own cell.
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        setFixedHeight(px(Size::Control));
    }
    repolish(*this);
}

KInlineEdit& KTextField::useInlineEdit(KInlineEdit::Validator validator) {
    if (inline_ == nullptr)
        inline_ = new KInlineEdit(*this, std::move(validator));
    else
        inline_->setValidator(std::move(validator));
    return *inline_;
}

KLineEdit::KLineEdit(QWidget* parent) : KTextField(parent) {}

KLineEdit::KLineEdit(const QString& text, QWidget* parent) : KTextField(text, parent) {}

KPathField::KPathField(QWidget* parent) : KLineEdit(parent) {
    // The whole path is always one hover away, unless a refusal reason is showing there.
    connect(this, &QLineEdit::textChanged, this, [this](const QString& path) {
        if (error().isEmpty())
            setToolTip(path);
    });
}

void KPathField::paintEvent(QPaintEvent* event) {
    if (hasFocus() || text().isEmpty()) {
        KLineEdit::paintEvent(event);
        return;
    }
    // The field's own box, then the path elided in the middle inside its contents.
    QPainter painter(this);
    QStyleOptionFrame option;
    initStyleOption(&option);
    style()->drawPrimitive(QStyle::PE_PanelLineEdit, &option, &painter, this);
    const QRect area = style()->subElementRect(QStyle::SE_LineEditContents, &option, this);
    const QMargins margins = textMargins();
    const QRect text = area.marginsRemoved(margins);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                     fontMetrics().elidedText(this->text(), Qt::ElideMiddle, text.width()));
}

KSearchField::KSearchField(QWidget* parent) : KLineEdit(parent) {
    setPlaceholderText(tr("Search"));
    search_ = addAction(icon(IconId::Zoom, IconRole::Chrome, Color::Muted),
                        QLineEdit::LeadingPosition);
    search_->setObjectName(QStringLiteral("kSearchGlyph"));
    search_->setEnabled(false);
    clear_ = addAction(icon(IconId::Close, IconRole::Chrome, Color::Muted),
                       QLineEdit::TrailingPosition);
    clear_->setObjectName(QStringLiteral("kSearchClear"));
    clear_->setToolTip(tr("Clear"));
    clear_->setVisible(false);
    connect(clear_, &QAction::triggered, this, [this] {
        clear();
        setFocus(Qt::OtherFocusReason);
    });
    connect(this, &QLineEdit::textChanged, this,
            [this](const QString& text) { clear_->setVisible(!text.isEmpty()); });
}

bool KSearchField::event(QEvent* event) {
    // The first Escape is this field's (it clears), so a window shortcut on Escape must not take it.
    if (event->type() == QEvent::ShortcutOverride &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && !text().isEmpty()) {
        event->accept();
        return true;
    }
    return KLineEdit::event(event);
}

void KSearchField::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (!text().isEmpty()) {
            clear();
            event->accept();
            return;
        }
        Q_EMIT escaped();
        event->ignore();
        return;
    }
    KLineEdit::keyPressEvent(event);
}

KTextArea::KTextArea(QWidget* parent) : QPlainTextEdit(parent) {
    setProperty("kitControl", true);
    setFont(kit::font(TypeRole::Value));
    setFrameShape(QFrame::NoFrame);
    applyPlaceholderInk(*this);
}

void KTextArea::setTypeRole(const TypeRole role) { setFont(kit::font(role)); }

void KTextArea::setError(const QString& reason) { applyError(*this, restingToolTip_, reason); }

KInlineEdit& KTextArea::useInlineEdit(KInlineEdit::Validator validator) {
    if (inline_ == nullptr)
        inline_ = new KInlineEdit(*this, std::move(validator));
    else
        inline_->setValidator(std::move(validator));
    return *inline_;
}

} // namespace bloom::ui::kit
