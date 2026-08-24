#pragma once

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

struct ValidationIssue {
    ValidationCode code;
    std::string path;
    std::string message;

    friend bool operator==(const ValidationIssue&, const ValidationIssue&) = default;
};

class ValidationResult final {
  public:
    [[nodiscard]] bool ok() const noexcept { return issues_.empty(); }
    [[nodiscard]] std::span<const ValidationIssue> issues() const noexcept { return issues_; }

    void add(ValidationCode code, std::string path, std::string message) {
        issues_.push_back({code, std::move(path), std::move(message)});
    }

    void append(std::string_view prefix, const ValidationResult& other) {
        for (const auto& issue : other.issues()) {
            std::string path(prefix);
            if (!path.empty() && !issue.path.empty()) {
                path.push_back('.');
            }
            path.append(issue.path);
            add(issue.code, std::move(path), issue.message);
        }
    }

  private:
    std::vector<ValidationIssue> issues_;
};

} // namespace bloom::document
