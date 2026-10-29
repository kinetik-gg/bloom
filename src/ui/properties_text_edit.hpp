#pragma once

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/inline_edit.hpp>
#include <bloom/ui/kit/text_field.hpp>

#include <QLineEdit>
#include <QObject>
#include <QPlainTextEdit>

#include <functional>
#include <memory>
#include <type_traits>

namespace bloom::ui::properties {

// The one text-parameter editor binding (lane B). The Properties Text row (single line and its
// multi-line expansion), a registry String row, and a node card's text rows all bind their kit
// text field through here, so typing into any of them is the same session edit:
//
// - the first edited character begins a live session value edit (an `Edit Text` edit for a text
//   layer's content, a plain value edit for any other string), and each edit after it updates the
//   live value, so the Viewer and every other surface preview the text as it is typed;
// - the kit inline-edit policy ends it: Return (Ctrl+Return in the multi-line form), Escape, Tab
//   and focus loss all COMMIT exactly one transaction -- Escape confirms everywhere, and Undo is
//   the discard. Text that was never changed commits nothing;
// - a session that drops the edit underneath (a time, revision or selection change) releases the
//   binding, and the surface's own refresh puts the document's text back.
//
// `parameter` resolves the edited parameter at the moment an edit begins, so a row that is reused
// across selections always writes the parameter it currently shows.
template <typename Editor, typename Parameter>
void bindTextEdit(CompositionSession& session, Editor& editor, Parameter parameter) {
    struct Binding {
        document::ParameterId parameter;
        bool active = false;
    };
    const auto binding = std::make_shared<Binding>();
    const auto text = [&editor] {
        if constexpr (std::is_base_of_v<QPlainTextEdit, Editor>)
            return editor.toPlainText();
        else
            return editor.text();
    };
    const auto begin = [&session, binding, parameter] {
        const auto id = parameter();
        if (id == document::ParameterId{})
            return false;
        binding->parameter = id;
        binding->active = session.beginTextEdit(id) || session.beginValueEdit(id);
        return binding->active;
    };
    // textEdited is the artist's typing by definition; a multi-line editor's textChanged also fires
    // for a programmatic setPlainText, which only a focused editor's typing is not.
    const auto focused = [target = &editor] {
        if constexpr (std::is_base_of_v<QPlainTextEdit, Editor>) {
            return target->hasFocus();
        } else {
            (void)target;
            return true;
        }
    };
    const auto typed = [&session, binding, begin, text, focused] {
        if (!focused())
            return;
        if (!binding->active || !session.isValueEditing(binding->parameter))
            if (!begin())
                return;
        (void)session.updateValueEdit(text().toStdString());
    };
    if constexpr (std::is_base_of_v<QPlainTextEdit, Editor>)
        QObject::connect(&editor, &QPlainTextEdit::textChanged, &editor, typed);
    else
        QObject::connect(&editor, &QLineEdit::textEdited, &editor, typed);

    auto& policy = editor.useInlineEdit();
    QObject::connect(
        &policy, &kit::KInlineEdit::accepted, &editor,
        [&session, binding, parameter, begin](const QString& committed) {
            const bool owned = binding->active && session.isValueEditing(binding->parameter);
            binding->active = false;
            if (owned) {
                (void)session.updateValueEdit(committed.toStdString());
                (void)session.commitValueEdit();
                return;
            }
            // Text set without typing (a paste the editor reported as one edit, a programmatic
            // change) commits only if it actually differs from what the document holds.
            const auto id = parameter();
            if (id == document::ParameterId{} ||
                session.effectiveStringValue(id).value_or(QString{}) == committed)
                return;
            if (begin()) {
                binding->active = false;
                (void)session.updateValueEdit(committed.toStdString());
                (void)session.commitValueEdit();
            }
        });
    QObject::connect(&session, &CompositionSession::liveValueChanged, &editor, [&session, binding] {
        if (binding->active && !session.isValueEditing(binding->parameter))
            binding->active = false;
    });
    QObject::connect(&editor, &QObject::destroyed, &session, [&session, binding] {
        if (binding->active && session.isValueEditing(binding->parameter))
            (void)session.commitValueEdit();
    });
}

} // namespace bloom::ui::properties
