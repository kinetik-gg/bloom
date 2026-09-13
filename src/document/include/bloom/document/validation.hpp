#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::document {

enum class ValidationCode {
    InvalidId,
    DuplicateId,
    EmptyKey,
    InvalidValue,
    MissingReference,
    TypeMismatch,
    SocketKindMismatch,
    InvalidInterpolation,
    DuplicateTime,
    InvalidOrder,
    OrphanObject,
    SharedReference,
    DuplicateInput,
    GraphCycle,
    InvalidLayerBoundary,
    InvalidLayerStack,
    MissingCompositionOutput,
    ForeignDocument,
    RevisionMismatch,
};

enum class ValidationSeverity { Error, Warning };

struct ValidationIssue {
    ValidationCode code;
    std::string path;
    std::string message;
    ValidationSeverity severity = ValidationSeverity::Error;

    friend bool operator==(const ValidationIssue&, const ValidationIssue&) = default;
};

class ValidationResult final {
  public:
    [[nodiscard]] bool ok() const noexcept {
        return std::ranges::none_of(
            issues_, [](const auto& issue) { return issue.severity == ValidationSeverity::Error; });
    }
    [[nodiscard]] std::span<const ValidationIssue> issues() const noexcept { return issues_; }

    void add(ValidationCode code, std::string path, std::string message,
             ValidationSeverity severity = ValidationSeverity::Error) {
        issues_.push_back({code, std::move(path), std::move(message), severity});
    }

    void append(std::string_view prefix, const ValidationResult& other) {
        for (const auto& issue : other.issues()) {
            std::string path(prefix);
            if (!path.empty() && !issue.path.empty()) {
                path.push_back('.');
            }
            path.append(issue.path);
            add(issue.code, std::move(path), issue.message, issue.severity);
        }
    }

  private:
    std::vector<ValidationIssue> issues_;
};

} // namespace bloom::document
