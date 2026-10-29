#include <bloom/ui/kit/inline_edit.hpp>

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStyle>
#include <QToolTip>
#include <QWidget>

namespace bloom::ui::kit {
namespace {

void repolish(QWidget& widget) {
    widget.style()->unpolish(&widget);
    widget.style()->polish(&widget);
    widget.update();
}

} // namespace

KInlineEdit::KInlineEdit(QWidget& editor, Validator validator)
    : QObject(&editor), editor_(&editor), validator_(std::move(validator)) {
    editor.installEventFilter(this);
}

KInlineEdit::KInlineEdit(QLineEdit& editor, Validator validator)
    : KInlineEdit(static_cast<QWidget&>(editor), std::move(validator)) {
    line_ = &editor;
    // Typing again is the answer to a refusal, so the reason steps aside until the next ending.
    connect(&editor, &QLineEdit::textEdited, this, [this] { clearRefusal(); });
}

KInlineEdit::KInlineEdit(QPlainTextEdit& editor, Validator validator)
    : KInlineEdit(static_cast<QWidget&>(editor), std::move(validator)) {
    plain_ = &editor;
    connect(&editor, &QPlainTextEdit::textChanged, this, [this] {
        if (!ending_)
            clearRefusal();
    });
}

void KInlineEdit::setValidator(Validator validator) { validator_ = std::move(validator); }

void KInlineEdit::setTabEnds(const bool ends) { tabEnds_ = ends; }

QString KInlineEdit::text() const {
    if (line_ != nullptr)
        return line_->text();
    return plain_ != nullptr ? plain_->toPlainText() : QString{};
}

bool KInlineEdit::validate() {
    const QString reason = validator_ ? validator_(text()) : QString{};
    if (reason.isEmpty())
        clearRefusal();
    else
        showRefusal(reason);
    return reason.isEmpty();
}

void KInlineEdit::finish() { end(Ending::FocusOut, false); }

void KInlineEdit::showRefusal(const QString& reason) {
    if (editor_ == nullptr)
        return;
    if (refusal_.isEmpty())
        restingToolTip_ = editor_->toolTip();
    const bool changed = refusal_ != reason;
    refusal_ = reason;
    editor_->setProperty("kitError", true);
    editor_->setToolTip(reason);
    editor_->setAccessibleDescription(reason);
    repolish(*editor_);
    if (editor_->isVisible())
        QToolTip::showText(editor_->mapToGlobal(QPoint(0, editor_->height())), reason, editor_);
    if (changed)
        Q_EMIT refusalChanged(refusal_);
}

void KInlineEdit::clearRefusal() {
    if (refusal_.isEmpty() || editor_ == nullptr)
        return;
    refusal_.clear();
    editor_->setProperty("kitError", false);
    editor_->setToolTip(restingToolTip_);
    editor_->setAccessibleDescription({});
    repolish(*editor_);
    Q_EMIT refusalChanged(refusal_);
}

KInlineEdit::Validator KInlineEdit::nameValidator() {
    return [](const QString& text) {
        return text.trimmed().isEmpty() ? tr("A name cannot be empty") : QString{};
    };
}

void KInlineEdit::showRefusalNote(QWidget& near, const QString& text, const QString& reason) {
    QToolTip::showText(near.mapToGlobal(QPoint(0, near.height())),
                       tr("Not taken: “%1”. %2").arg(text, reason), &near);
}

void KInlineEdit::end(const Ending ending, const bool mayStayOpen) {
    // An owner closing the editor from inside accepted()/refused() hands the editor's focus away,
    // and that focus loss is the same edit ending again, not a second one.
    if (ending_ || editor_ == nullptr)
        return;
    const QString current = text();
    const QString reason = validator_ ? validator_(current) : QString{};
    if (!reason.isEmpty() && mayStayOpen) {
        showRefusal(reason);
        return;
    }
    ending_ = true;
    const QPointer<KInlineEdit> alive(this);
    if (reason.isEmpty()) {
        clearRefusal();
        Q_EMIT accepted(current, ending);
    } else {
        clearRefusal();
        QWidget* host = editor_->parentWidget() != nullptr ? editor_->parentWidget() : editor_.data();
        showRefusalNote(*host, current, reason);
        Q_EMIT refused(current, reason, ending);
    }
    if (alive)
        ending_ = false;
}

bool KInlineEdit::eventFilter(QObject* watched, QEvent* event) {
    if (watched != editor_)
        return QObject::eventFilter(watched, event);
    const auto multiline = plain_ != nullptr;
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Escape:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            event->accept();
            return true;
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            if (tabEnds_) {
                event->accept();
                return true;
            }
            break;
        default:
            break;
        }
        break;
    }
    case QEvent::KeyPress: {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Escape:
            end(Ending::Escape, false);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (multiline && !key->modifiers().testAnyFlags(Qt::ControlModifier |
                                                             Qt::MetaModifier))
                return false;
            end(Ending::Return, true);
            return true;
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            if (!tabEnds_)
                break;
            end(key->key() == Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier)
                    ? Ending::Backtab
                    : Ending::Tab,
                true);
            return true;
        default:
            break;
        }
        break;
    }
    case QEvent::FocusOut: {
        // The editor's own context menu borrows focus and gives it back, and a window switch
        // (or the window closing) takes it with the window: neither is the artist leaving the
        // field, and the edit carries on when the window is active again.
        const auto* focus = static_cast<QFocusEvent*>(event);
        if (focus->reason() != Qt::PopupFocusReason &&
            focus->reason() != Qt::ActiveWindowFocusReason)
            end(Ending::FocusOut, false);
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace bloom::ui::kit
