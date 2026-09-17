#include "viewer_editor_text_layout.hpp"
#include <QApplication>
#include <QClipboard>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_editor.hpp>

namespace bloom::ui {
namespace {
int textBoundary(const QString& text, const int position, const bool forward, const bool word) {
    QTextBoundaryFinder finder(word ? QTextBoundaryFinder::Word : QTextBoundaryFinder::Grapheme,
                               text);
    finder.setPosition(position);
    int next = position;
    while (true) {
        next = static_cast<int>(forward ? finder.toNextBoundary() : finder.toPreviousBoundary());
        if (next < 0)
            return forward ? static_cast<int>(text.size()) : 0;
        if (!word || finder.boundaryReasons().testFlag(QTextBoundaryFinder::StartOfItem))
            return next;
    }
}
} // namespace

void ViewerEditor::publishTextEdit() {
    if (!textEdit_)
        return;
    auto content = textEdit_->text;
    if (!textEdit_->preedit.isEmpty()) {
        const auto first = std::min(textEdit_->cursor, textEdit_->anchor);
        content.replace(first, std::abs(textEdit_->cursor - textEdit_->anchor), textEdit_->preedit);
    }
    if (!session_.updateValueEdit(content.toStdString())) {
        if (textEdit_) {
            textEdit_->text =
                session_.effectiveStringValue(textEdit_->parameter).value_or(QString{});
            textEdit_->preedit.clear();
            textEdit_->cursor =
                std::min(textEdit_->cursor, static_cast<int>(textEdit_->text.size()));
            textEdit_->anchor = textEdit_->cursor;
            refreshTextLayout();
        }
        return;
    }
    refreshTextLayout();
    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
}
void ViewerEditor::replaceTextSelection(const QString& replacement) {
    if (!textEdit_)
        return;
    auto& edit = *textEdit_;
    const auto first = std::min(edit.cursor, edit.anchor);
    edit.text.replace(first, std::abs(edit.cursor - edit.anchor), replacement);
    edit.cursor = first + static_cast<int>(replacement.size());
    edit.anchor = edit.cursor;
    edit.preedit.clear();
    edit.preeditCursorVisible = true;
    edit.pendingClick.reset();
    edit.multiline = edit.multiline || replacement.contains('\n');
    publishTextEdit();
}
bool ViewerEditor::textEditKey(QKeyEvent* event) {
    if (!textEdit_)
        return false;
    const int key = event->key();
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool control = event->modifiers().testFlag(Qt::ControlModifier) ||
                         event->modifiers().testFlag(Qt::MetaModifier);
    if (key == Qt::Key_Escape) {
        finishTextEditing(false);
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (control || (!textEdit_->multiline && !shift)) {
            QGuiApplication::inputMethod()->commit();
            finishTextEditing(true);
        } else {
            replaceTextSelection("\n");
        }
    } else if (event->matches(QKeySequence::SelectAll)) {
        textEdit_->anchor = 0;
        textEdit_->cursor = static_cast<int>(textEdit_->text.size());
    } else if (event->matches(QKeySequence::Paste)) {
        replaceTextSelection(QApplication::clipboard()->text());
    } else if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
        const int first = std::min(textEdit_->cursor, textEdit_->anchor);
        QApplication::clipboard()->setText(
            textEdit_->text.mid(first, std::abs(textEdit_->cursor - textEdit_->anchor)));
        if (event->matches(QKeySequence::Cut))
            replaceTextSelection({});
    } else if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
        if (textEdit_->cursor == textEdit_->anchor)
            textEdit_->anchor =
                textBoundary(textEdit_->text, textEdit_->cursor, key == Qt::Key_Delete, control);
        replaceTextSelection({});
    } else if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Home ||
               key == Qt::Key_End || key == Qt::Key_Up || key == Qt::Key_Down) {
        auto& edit = *textEdit_;
        const bool forward = key == Qt::Key_Right || key == Qt::Key_End || key == Qt::Key_Down;
        if (key == Qt::Key_Left || key == Qt::Key_Right) {
            if (!shift && edit.cursor != edit.anchor && !control)
                edit.cursor = forward ? std::max(edit.cursor, edit.anchor)
                                      : std::min(edit.cursor, edit.anchor);
            else
                edit.cursor = textBoundary(edit.text, edit.cursor, forward, control);
        } else if (control) {
            edit.cursor = forward ? static_cast<int>(edit.text.size()) : 0;
        } else if (edit.layoutReady) {
            const auto cursorByte = textCaretByte();
            const auto caret = textLayoutCaret(edit.layout, cursorByte);
            const auto& lines = edit.layout.lines;
            auto index = std::size_t{0};
            for (const auto& line : lines)
                if (line.byteRange.begin <= cursorByte)
                    index = line.index;
            std::size_t byte = forward ? lines[index].byteRange.end : lines[index].byteRange.begin;
            if (key == Qt::Key_Up || key == Qt::Key_Down) {
                index = forward ? std::min(index + 1, lines.size() - 1) : (index ? index - 1 : 0);
                const QPointF point(caret.x1(), lines[index].box.y + edit.layout.caretHeight / 2);
                byte = nearestTextLayoutBoundary(edit.layout, point);
            }
            edit.cursor = static_cast<int>(
                QString::fromUtf8(edit.text.toUtf8().left(static_cast<qsizetype>(byte))).size());
        } else if (key == Qt::Key_Home || key == Qt::Key_End) {
            // Navigation must remain usable while a newer layout is being prepared.
            if (forward) {
                const auto newline = edit.text.indexOf('\n', edit.cursor);
                edit.cursor = static_cast<int>(newline < 0 ? edit.text.size() : newline);
            } else {
                edit.cursor =
                    edit.cursor == 0
                        ? 0
                        : static_cast<int>(edit.text.lastIndexOf('\n', edit.cursor - 1) + 1);
            }
        }
        if (!shift)
            edit.anchor = edit.cursor;
        edit.pendingClick.reset();
    } else if (!event->text().isEmpty() &&
               (!control || event->modifiers().testFlag(Qt::AltModifier)) &&
               (event->text().front().isPrint() ||
                (event->text().size() > 1 && event->text().front().isHighSurrogate() &&
                 event->text().at(1).isLowSurrogate()))) {
        replaceTextSelection(event->text());
    }
    // All editing keys, including application Delete/nudge/tool bindings, terminate here.
    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    update();
    event->accept();
    return true;
}

void ViewerEditor::inputMethodEvent(QInputMethodEvent* event) {
    if (!textEdit_) {
        QWidget::inputMethodEvent(event);
        return;
    }
    auto& edit = *textEdit_;
    if (!event->commitString().isEmpty() || event->replacementLength() != 0) {
        int first = std::min(edit.cursor, edit.anchor);
        int count = std::abs(edit.cursor - edit.anchor);
        if (event->replacementStart() != 0 || event->replacementLength() != 0) {
            first = std::clamp(edit.cursor + event->replacementStart(), 0,
                               static_cast<int>(edit.text.size()));
            count = std::clamp(event->replacementLength(), 0,
                               static_cast<int>(edit.text.size()) - first);
        }
        edit.text.replace(first, count, event->commitString());
        edit.cursor = first + static_cast<int>(event->commitString().size());
        edit.anchor = edit.cursor;
    }
    edit.preedit = event->preeditString();
    edit.preeditCursor = static_cast<int>(edit.preedit.size());
    edit.preeditCursorVisible = true;
    edit.pendingClick.reset();
    for (const auto& attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            edit.preeditCursor =
                std::clamp(attribute.start, 0, static_cast<int>(edit.preedit.size()));
            edit.preeditCursorVisible = attribute.length != 0;
        } else if (attribute.type == QInputMethodEvent::Selection) {
            edit.anchor = std::clamp(attribute.start, 0, static_cast<int>(edit.text.size()));
            edit.cursor = std::clamp(attribute.start + attribute.length, 0,
                                     static_cast<int>(edit.text.size()));
        }
    }
    publishTextEdit();
    event->accept();
}
QVariant ViewerEditor::inputMethodQuery(const Qt::InputMethodQuery query) const {
    if (!textEdit_)
        return QWidget::inputMethodQuery(query);
    switch (query) {
    case Qt::ImHints:
        return static_cast<int>(textEdit_->multiline ? Qt::ImhMultiLine : Qt::ImhNone);
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle: {
        const auto caret = textCaretForTest();
        return QRectF(caret.p1(), caret.p2())
            .normalized()
            .adjusted(0, 0, kit::px(kit::Size::Hairline), 0);
    }
    case Qt::ImCursorPosition:
        return textEdit_->cursor;
    case Qt::ImAnchorPosition:
        return textEdit_->anchor;
    case Qt::ImSurroundingText:
        return textEdit_->text;
    case Qt::ImCurrentSelection:
        return textEdit_->text.mid(std::min(textEdit_->cursor, textEdit_->anchor),
                                   std::abs(textEdit_->cursor - textEdit_->anchor));
    default:
        return QWidget::inputMethodQuery(query);
    }
}
} // namespace bloom::ui
