#pragma once

#include <bloom/commands/result.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace bloom::scripting {

// One diagnostic of a command result, in the shape every scripting client publishes (MCP's
// structuredContent.diagnostics, Python's result["diagnostics"]). A refused transaction always
// carries at least one, and each states the cause and what to do about it -- never only that the
// host refused.
struct CommandDiagnostic final {
    // bloom.scripting.command-rejected (an operation refused), bloom.scripting.validation-failed
    // (the edit would leave the document invalid), bloom.scripting.stale-revision,
    // bloom.scripting.history-limit, bloom.scripting.operation-warning, ...
    std::string code;
    // "error" or "warning".
    std::string severity = "error";
    // The operation in the transaction's list that caused it (0-based), when one did. Stale
    // revisions and history limits belong to the whole transaction and name none.
    std::optional<std::size_t> operationIndex;
    std::string operation;
    // The document path a validation issue names; empty otherwise.
    std::string path;
    std::string message;
};

// At most this many validation issues are listed; a further one is summarised by count.
inline constexpr std::size_t kMaxCommandValidationDiagnostics = 16;

[[nodiscard]] std::vector<CommandDiagnostic>
commandDiagnostics(const commands::CommandResult& result);

} // namespace bloom::scripting
