#pragma once

#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::commands {

enum class OperationStatus {
    Applied,
    NoChange,
    Rejected,
};

enum class OperationIssueCode {
    InvalidTarget,
    InvalidValue,
    DuplicateId,
    MissingReference,
    InvalidOrder,
    Unsupported,
};

struct OperationIssue {
    OperationIssueCode code;
    std::string message;

    friend bool operator==(const OperationIssue&, const OperationIssue&) = default;
};

using DurableObjectId =
    std::variant<document::CompositionId, document::NodeId, document::EdgeId, document::LayerId,
                 document::LayerSlotId, document::ParameterId, document::AnimationCurveId,
                 document::DriverBindingId>;

struct OperationOutput {
    std::string name;
    DurableObjectId id;

    friend bool operator==(const OperationOutput&, const OperationOutput&) = default;
};

struct OperationResult {
    OperationStatus status = OperationStatus::Applied;
    std::vector<OperationIssue> issues;
    std::vector<OperationOutput> outputs;

    [[nodiscard]] static OperationResult applied(std::vector<OperationOutput> outputs = {});
    [[nodiscard]] static OperationResult noChange();
    [[nodiscard]] static OperationResult rejected(OperationIssueCode code, std::string message);
};

enum class CommandAction {
    Execute,
    Undo,
    Redo,
};

enum class CommandStatus {
    Succeeded,
    NoChange,
    Rejected,
    ValidationFailed,
    StaleRevision,
    ForeignDocument,
    DraftBaseMismatch,
    RevisionOverflow,
    NothingToUndo,
    NothingToRedo,
};

struct OperationFailure {
    std::size_t operationIndex = 0;
    std::string operationType;
    OperationIssue issue;

    friend bool operator==(const OperationFailure&, const OperationFailure&) = default;
};

struct CommandOutput {
    std::size_t operationIndex = 0;
    std::string operationType;
    OperationOutput output;

    friend bool operator==(const CommandOutput&, const CommandOutput&) = default;
};

struct CommandResult {
    CommandAction action = CommandAction::Execute;
    CommandStatus status = CommandStatus::NoChange;
    document::Revision beforeRevision;
    document::Revision afterRevision;
    std::optional<document::Revision> expectedRevision;
    std::string label;
    std::vector<OperationFailure> operationFailures;
    std::vector<CommandOutput> outputs;
    document::ValidationResult validation;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == CommandStatus::Succeeded || status == CommandStatus::NoChange;
    }
    [[nodiscard]] bool changed() const noexcept { return status == CommandStatus::Succeeded; }

    template <core::TypedId IdType>
    [[nodiscard]] std::optional<IdType> outputId(std::string_view name,
                                                 std::size_t operationIndex = 0) const noexcept {
        for (const auto& item : outputs) {
            if (item.operationIndex == operationIndex && item.output.name == name) {
                if (const auto* id = std::get_if<IdType>(&item.output.id)) {
                    return *id;
                }
            }
        }
        return std::nullopt;
    }
};

} // namespace bloom::commands
