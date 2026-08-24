#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/task_types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::runtime {

enum class SnapshotCompileStatus {
    Compiled,
    Unsupported,
    Cancelled,
    Failed,
};

enum class CompileDiagnosticCode {
    RegistryNotFrozen,
    CompositionNotFound,
    UnknownNodeType,
    UnsupportedNodeVersion,
    UnsupportedNode,
    InvalidCompositionOutput,
    UnknownPort,
    MissingInput,
    PortTypeMismatch,
    MissingParameter,
    UnexpectedParameter,
    ParameterSchemaMismatch,
    ParameterValueKindMismatch,
    UnsupportedParameterSource,
    InvalidParameterOverride,
    UnsupportedParameterOverride,
    TopologyInvariant,
};

[[nodiscard]] std::string_view compileDiagnosticCodeId(CompileDiagnosticCode code) noexcept;

struct CompileSubject {
    document::CompositionId compositionId;
    std::optional<document::NodeId> nodeId;
    std::optional<document::EdgeId> edgeId;
    std::optional<document::ParameterId> parameterId;
    std::optional<document::LayerId> layerId;
    std::optional<document::LayerSlotId> layerSlotId;
    std::string field;

    friend bool operator==(const CompileSubject&, const CompileSubject&) = default;
};

struct CompileDiagnostic {
    CompileDiagnosticCode code = CompileDiagnosticCode::TopologyInvariant;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    CompileSubject subject;
    std::string summary;
    std::string detail;

    friend bool operator==(const CompileDiagnostic&, const CompileDiagnostic&) = default;
};

struct SnapshotParameterOverride final {
    document::Revision sourceRevision;
    document::ParameterId parameterId;
    std::variant<double, document::Vec2d> value;

    friend bool operator==(const SnapshotParameterOverride&,
                           const SnapshotParameterOverride&) = default;
};

struct SnapshotCompileRequest {
    document::Snapshot snapshot;
    document::CompositionId compositionId;
    std::optional<SnapshotParameterOverride> parameterOverride = std::nullopt;
};

struct SnapshotCompileResult {
    SnapshotCompileStatus status = SnapshotCompileStatus::Failed;
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::vector<CompileDiagnostic> diagnostics;
};

class SnapshotCompiler final {
  public:
    explicit SnapshotCompiler(const NodeDefinitionRegistry& registry) noexcept
        : registry_(registry) {}

    [[nodiscard]] SnapshotCompileResult compile(const SnapshotCompileRequest& request,
                                                const CancellationToken& cancellation) const;

  private:
    const NodeDefinitionRegistry& registry_;
};

} // namespace bloom::runtime
