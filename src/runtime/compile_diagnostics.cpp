#include <bloom/runtime/snapshot_compiler.hpp>

namespace bloom::runtime {

std::string_view compileDiagnosticCodeId(const CompileDiagnosticCode code) noexcept {
    switch (code) {
    case CompileDiagnosticCode::RegistryNotFrozen:
        return "bloom.runtime.compile.registry-not-frozen";
    case CompileDiagnosticCode::CompositionNotFound:
        return "bloom.runtime.compile.composition-not-found";
    case CompileDiagnosticCode::UnknownNodeType:
        return "bloom.runtime.compile.unknown-node-type";
    case CompileDiagnosticCode::UnsupportedNodeVersion:
        return "bloom.runtime.compile.unsupported-node-version";
    case CompileDiagnosticCode::UnsupportedNode:
        return "bloom.runtime.compile.unsupported-node";
    case CompileDiagnosticCode::InvalidCompositionOutput:
        return "bloom.runtime.compile.invalid-composition-output";
    case CompileDiagnosticCode::UnknownPort:
        return "bloom.runtime.compile.unknown-port";
    case CompileDiagnosticCode::MissingInput:
        return "bloom.runtime.compile.missing-input";
    case CompileDiagnosticCode::PortTypeMismatch:
        return "bloom.runtime.compile.port-type-mismatch";
    case CompileDiagnosticCode::MissingParameter:
        return "bloom.runtime.compile.missing-parameter";
    case CompileDiagnosticCode::UnexpectedParameter:
        return "bloom.runtime.compile.unexpected-parameter";
    case CompileDiagnosticCode::ParameterSchemaMismatch:
        return "bloom.runtime.compile.parameter-schema-mismatch";
    case CompileDiagnosticCode::ParameterValueKindMismatch:
        return "bloom.runtime.compile.parameter-value-kind-mismatch";
    case CompileDiagnosticCode::UnsupportedParameterSource:
        return "bloom.runtime.compile.unsupported-parameter-source";
    case CompileDiagnosticCode::InvalidParameterOverride:
        return "bloom.runtime.compile.invalid-parameter-override";
    case CompileDiagnosticCode::UnsupportedParameterOverride:
        return "bloom.runtime.compile.unsupported-parameter-override";
    case CompileDiagnosticCode::TopologyInvariant:
        return "bloom.runtime.compile.topology-invariant";
    }
    return "bloom.runtime.compile.unknown-diagnostic";
}

} // namespace bloom::runtime
