#include <bloom/commands/result.hpp>

#include <utility>

namespace bloom::commands {

OperationResult OperationResult::applied(std::vector<OperationOutput> outputs) {
    return {
        .status = OperationStatus::Applied,
        .issues = {},
        .outputs = std::move(outputs),
    };
}

OperationResult OperationResult::noChange() {
    return {
        .status = OperationStatus::NoChange,
        .issues = {},
        .outputs = {},
    };
}

OperationResult OperationResult::rejected(OperationIssueCode code, std::string message) {
    return {
        .status = OperationStatus::Rejected,
        .issues = {{code, std::move(message)}},
        .outputs = {},
    };
}

} // namespace bloom::commands
