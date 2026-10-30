#include <bloom/commands/parameter_reset_operations.hpp>

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <type_traits>

namespace bloom::commands {

OperationResult ResetParameter::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The composition does not exist");
    const auto* parameter = composition->parameters().find(parameter_);
    if (parameter == nullptr)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "The parameter is not in the composition");
    // The node that owns the parameter, and its definition's declaration of it.
    std::optional<document::NodeId> owner;
    const document::ParameterDefinition* declared = nullptr;
    for (const auto& node : composition->graph().nodes()) {
        const auto binding = std::ranges::find(node.parameters, parameter_,
                                               &document::ParameterBinding::parameterId);
        if (binding == node.parameters.end())
            continue;
        const auto* definition =
            document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
        if (definition == nullptr)
            break;
        const auto found = std::ranges::find(definition->parameters, binding->role,
                                             &document::ParameterDefinition::role);
        if (found != definition->parameters.end()) {
            owner = node.id;
            declared = &*found;
        }
        break;
    }
    if (!owner || declared == nullptr)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "The parameter has no node default to reset to");
    if (std::holds_alternative<document::DriverBindingSource>(parameter->source))
        // Disconnecting restores the registry default in one step.
        return DisconnectInput(composition_, document::NodeInputRef{*owner, declared->role})
            .apply(draft);
    if (std::holds_alternative<document::ConstantValueSource>(parameter->source))
        return SetParameterSource(composition_, parameter_,
                                  document::ConstantValueSource{declared->defaultValue})
            .apply(draft);
    const auto* animated = std::get_if<document::AnimationCurveSource>(&parameter->source);
    if (animated == nullptr)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "The parameter's source cannot be reset");
    return std::visit(
        [&](const auto& held) -> OperationResult {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, double> || std::is_same_v<Held, document::Vec2d> ||
                          std::is_same_v<Held, document::Vec3d> ||
                          std::is_same_v<Held, core::Color4d>)
                return SetKeyframeAtTime(composition_, animated->curveId, keyTime_, held)
                    .apply(draft);
            else
                return OperationResult::rejected(OperationIssueCode::Unsupported,
                                                 "This parameter's kind cannot carry a keyframe");
        },
        declared->defaultValue);
}

} // namespace bloom::commands
