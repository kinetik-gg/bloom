#pragma once

#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace bloom::runtime::detail {

enum class CompileCheckpointPhase {
    General,
    IncomingEdgeIndex,
    DefinitionResolution,
};

class CompileCheckpointObserver {
  public:
    CompileCheckpointObserver() = default;
    CompileCheckpointObserver(const CompileCheckpointObserver&) = delete;
    CompileCheckpointObserver& operator=(const CompileCheckpointObserver&) = delete;
    virtual ~CompileCheckpointObserver() = default;

    virtual void checkpoint(CompileCheckpointPhase phase) = 0;
};

[[nodiscard]] SnapshotCompileResult compileSnapshot(const NodeDefinitionRegistry& registry,
                                                    const SnapshotCompileRequest& request,
                                                    const CancellationToken& cancellation,
                                                    CompileCheckpointObserver* checkpointObserver);

[[nodiscard]] inline document::NodeId
destinationNode(const document::InputPortRef& destination) noexcept {
    return std::visit(
        [](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, document::NodeInputRef>) {
                return input.nodeId;
            } else {
                return input.stackNodeId;
            }
        },
        destination);
}

[[nodiscard]] inline bool hasValueKind(const document::ParameterValue& value,
                                       const ParameterValueKind kind) noexcept {
    switch (kind) {
    case ParameterValueKind::Color4d:
        return std::holds_alternative<core::Color4d>(value);
    case ParameterValueKind::Vec2d:
        return std::holds_alternative<document::Vec2d>(value);
    case ParameterValueKind::Float64:
        return std::holds_alternative<double>(value);
    case ParameterValueKind::String:
        return std::holds_alternative<std::string>(value);
    }
    return false;
}

[[nodiscard]] inline const document::ParameterBinding*
findParameterBinding(const document::NodeRecord& node, const std::string_view role) noexcept {
    const auto match = std::ranges::find_if(
        node.parameters, [&](const auto& binding) { return binding.role == role; });
    return match == node.parameters.end() ? nullptr : &*match;
}

} // namespace bloom::runtime::detail
