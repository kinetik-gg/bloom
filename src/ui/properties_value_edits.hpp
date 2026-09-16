#pragma once

#include <bloom/ui/composition_session.hpp>

#include <memory>
#include <optional>

namespace bloom::ui::properties {

// The same gesture lifetime for numeric cells and colour pickers. Value callbacks continue
// through their existing typed setters, which stage matching edits in the session.
template <typename Control, typename Parameter, typename Component>
void bindValueEdit(CompositionSession& session, Control& control, Parameter parameter,
                   Component component) {
    struct Binding {
        document::ParameterId parameter;
        bool active = false;
    };
    const auto binding = std::make_shared<Binding>();
    QObject::connect(&control, &Control::editStarted, &control,
                     [&session, parameter, component, binding] {
                         const auto id = parameter();
                         const bool accepted = session.beginValueEdit(id, component());
                         binding->parameter = id;
                         binding->active = accepted;
                     });
    QObject::connect(&control, &Control::editFinished, &control, [&session, binding] {
        const bool owned = binding->active && session.isValueEditing(binding->parameter);
        binding->active = false;
        if (owned)
            (void)session.commitValueEdit();
    });
    QObject::connect(&control, &Control::editCancelled, &control, [&session, binding] {
        const bool owned = binding->active && session.isValueEditing(binding->parameter);
        binding->active = false;
        if (owned)
            session.cancelValueEdit();
    });
    QObject::connect(&session, &CompositionSession::liveValueChanged, &control,
                     [&session, &control, binding] {
                         if (binding->active && !session.isValueEditing(binding->parameter)) {
                             binding->active = false;
                             control.cancelEdit();
                         }
                     });
    QObject::connect(&control, &QObject::destroyed, &session, [&session, binding] {
        if (binding->active && session.isValueEditing(binding->parameter))
            session.cancelValueEdit();
    });
}

template <typename Control, typename Parameter>
void bindValueEdit(CompositionSession& session, Control& control, Parameter parameter) {
    bindValueEdit(session, control, parameter,
                  [] { return std::optional<document::AnimationComponent>{}; });
}
} // namespace bloom::ui::properties
