#pragma once

// Private to src/host: small factories for the export queue's value types.
#include <bloom/host/export_queue.hpp>
#include <bloom/host/export_verification.hpp>

#include <optional>
#include <string>

namespace bloom::host::detail {

[[nodiscard]] inline ExportFailure
exportFailure(const ExportFailureCode code, std::string diagnostic, std::string reason,
              const std::optional<ExportOutputId> output = std::nullopt,
              const std::optional<std::uint64_t> frame = std::nullopt) {
    ExportFailure failure;
    failure.code = code;
    failure.output = output;
    failure.frame = frame;
    failure.diagnosticCode = std::move(diagnostic);
    failure.reason = std::move(reason);
    return failure;
}

[[nodiscard]] inline ExportLogEntry
exportLog(const runtime::DiagnosticSeverity severity, std::string code, std::string message,
          const std::optional<ExportOutputId> output = std::nullopt,
          const std::optional<std::uint64_t> frame = std::nullopt) {
    ExportLogEntry entry;
    entry.severity = severity;
    entry.code = std::move(code);
    entry.output = output;
    entry.frame = frame;
    entry.message = std::move(message);
    return entry;
}

[[nodiscard]] inline ExportAssetImportResult exportImport(const ExportOutputId output,
                                                          const ExportAssetImportStatus status,
                                                          std::string code = {},
                                                          std::string reason = {}) {
    ExportAssetImportResult result;
    result.output = output;
    result.status = status;
    result.code = std::move(code);
    result.reason = std::move(reason);
    return result;
}

[[nodiscard]] inline ExportOutputVerification notRunVerification(const ExportOutputId output) {
    ExportOutputVerification verification;
    verification.output = output;
    verification.overall = ExportCheckOutcome::NotRun;
    return verification;
}

} // namespace bloom::host::detail
