#include <bloom/document/persisted_text.hpp>

#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace {

[[nodiscard]] bool isIdentifierCharacter(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-';
}

void validateBoundedText(const std::string_view value, const std::size_t maximumBytes,
                         std::string path, const std::string_view label,
                         bloom::document::ValidationResult& result) {
    using bloom::document::ValidationCode;
    if (value.empty()) {
        result.add(ValidationCode::EmptyKey, std::move(path),
                   std::string(label) + " must not be empty");
    } else if (value.size() > maximumBytes || !bloom::core::isValidUtf8(value)) {
        result.add(ValidationCode::InvalidValue, std::move(path),
                   std::string(label) + " must be valid UTF-8 of at most " +
                       std::to_string(maximumBytes) + " bytes");
    }
}

} // namespace

namespace bloom::document {

bool isValidHumanFacingName(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaxHumanFacingNameBytes && core::isValidUtf8(value);
}

bool isValidStructuralText(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaxStructuralTextBytes && core::isValidUtf8(value);
}

bool isValidNamespacedIdentifier(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaxNamespacedIdentifierBytes &&
           ((value.front() >= 'a' && value.front() <= 'z') ||
            (value.front() >= '0' && value.front() <= '9')) &&
           std::ranges::all_of(value, isIdentifierCharacter);
}

void validateHumanFacingName(const std::string_view value, std::string path,
                             const std::string_view label, ValidationResult& result) {
    validateBoundedText(value, kMaxHumanFacingNameBytes, std::move(path), label, result);
}

void validateStructuralText(const std::string_view value, std::string path,
                            const std::string_view label, ValidationResult& result) {
    validateBoundedText(value, kMaxStructuralTextBytes, std::move(path), label, result);
}

void validateNamespacedIdentifier(const std::string_view value, std::string path,
                                  const std::string_view label, ValidationResult& result) {
    if (value.empty()) {
        result.add(ValidationCode::EmptyKey, std::move(path),
                   std::string(label) + " must not be empty");
    } else if (!isValidNamespacedIdentifier(value)) {
        result.add(ValidationCode::InvalidValue, std::move(path),
                   std::string(label) + " must match [a-z0-9][a-z0-9._-]{0,127}");
    }
}

} // namespace bloom::document
