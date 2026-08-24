#pragma once

#include <bloom/document/validation.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace bloom::document {

inline constexpr std::size_t kMaxHumanFacingNameBytes = 4'096;
inline constexpr std::size_t kMaxStructuralTextBytes = 256;
inline constexpr std::size_t kMaxNamespacedIdentifierBytes = 128;

[[nodiscard]] bool isValidHumanFacingName(std::string_view value) noexcept;
[[nodiscard]] bool isValidStructuralText(std::string_view value) noexcept;
[[nodiscard]] bool isValidNamespacedIdentifier(std::string_view value) noexcept;

void validateHumanFacingName(std::string_view value, std::string path, std::string_view label,
                             ValidationResult& result);
void validateStructuralText(std::string_view value, std::string path, std::string_view label,
                            ValidationResult& result);
void validateNamespacedIdentifier(std::string_view value, std::string path, std::string_view label,
                                  ValidationResult& result);

} // namespace bloom::document
