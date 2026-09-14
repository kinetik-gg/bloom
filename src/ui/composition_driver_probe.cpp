#include "composition_driver_probe.hpp"
#include <algorithm>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/value_nodes.hpp>

namespace bloom::ui {
namespace {
class DriverProbe final {
  public:
    DriverProbe(const document::Snapshot& snapshot, document::CompositionId composition)
        : document_(snapshot.project()), stack_(document_), composition_(composition) {}
    template <class Operation, class... Args> commands::CommandResult edit(Args&&... args) {
        commands::Transaction transaction("Inspect driver", document_.snapshot().revision());
        transaction.emplace<Operation>(composition_, std::forward<Args>(args)...);
        return stack_.execute(std::move(transaction));
    }
    document::NodeId add(std::string_view type) {
        return edit<commands::AddNode>(std::string(type), document::Vec2d{})
            .outputId<document::NodeId>(commands::kAddNodeOutput)
            .value_or(document::NodeId{});
    }
    bool connect(document::OutputPortRef source, document::InputPortRef input) {
        return edit<commands::ConnectPorts>(std::move(source), std::move(input)).succeeded();
    }
    std::string operand(document::NodeId id, document::ParameterValueKind kind) const {
        const auto snapshot = document_.snapshot();
        const auto* node = snapshot.project().findComposition(composition_)->graph().findNode(id);
        const auto* definition =
            node ? document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
                 : nullptr;
        if (definition)
            for (const auto& parameter : definition->parameters)
                if (parameter.valueKind == kind)
                    return parameter.role;
        return {};
    }
    document::Snapshot snapshot() const { return document_.snapshot(); }

  private:
    document::Document document_;
    commands::CommandStack stack_;
    document::CompositionId composition_;
};
} // namespace
runtime::SnapshotCompileResult compileDriverProbe(const document::Snapshot& snapshot,
                                                  document::CompositionId compositionId,
                                                  std::span<const document::ParameterId> parameters,
                                                  const runtime::CancellationToken& cancellation) {
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (!composition || !composition->graph().compositionOutput())
        return {};
    DriverProbe probe(snapshot, compositionId);
    const auto merge = probe.add(document::kLayerStackNodeType);
    for (const auto id : parameters) {
        if (cancellation.isCancellationRequested())
            return {};
        const auto* parameter = composition->parameters().find(id);
        const auto* driver =
            parameter ? std::get_if<document::DriverBindingSource>(&parameter->source) : nullptr;
        if (!driver)
            continue;
        document::OutputPortRef output{driver->sourceNodeId, driver->outputPort};
        const auto kind = composition->graph().outputKind(output);
        if (!kind || *kind == document::SocketValueKind::Image)
            return {};
        auto valueKind = document::ParameterValueKind::Float64;
        auto type = document::kSolidSourceNodeType;
        if (*kind == document::SocketValueKind::Color)
            valueKind = document::ParameterValueKind::Color4d;
        if (*kind == document::SocketValueKind::String) {
            type = document::kTextSourceNodeType;
            valueKind = document::ParameterValueKind::String;
        }
        if (*kind == document::SocketValueKind::Vector2) {
            type = document::kLayerOutputNodeType;
            valueKind = document::ParameterValueKind::Vec2d;
        }
        if (*kind == document::SocketValueKind::Vector3) {
            const auto reduce = probe.add(document::kVector3ReduceNodeType);
            if (!probe.connect(
                    output,
                    document::NodeInputRef{
                        reduce, probe.operand(reduce, document::ParameterValueKind::Vec3d)}))
                return {};
            output = {reduce, std::string(document::kResultPortName)};
        }
        const auto image = probe.add(type);
        if (!probe.connect(output, document::NodeInputRef{image, probe.operand(image, valueKind)}))
            return {};
        if (type == document::kLayerOutputNodeType) {
            const auto source = probe.add(document::kSolidSourceNodeType);
            if (!probe.connect({source, "image"}, document::NodeInputRef{image, "image"}))
                return {};
        }
        if (!probe.connect({image, "image"},
                           document::LayerStackInputRef{
                               merge, {}, std::string(document::kLayerStackContentInputRole)}))
            return {};
    }
    const auto outputId = composition->graph().compositionOutput()->nodeId;
    (void)probe.edit<commands::DisconnectInput>(document::NodeInputRef{outputId, "image"});
    if (!probe.connect({merge, "image"}, document::NodeInputRef{outputId, "image"}))
        return {};
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    return compiler.compile({probe.snapshot(), compositionId}, cancellation);
}
} // namespace bloom::ui
