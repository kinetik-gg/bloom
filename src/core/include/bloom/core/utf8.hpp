#pragma once

#include <compare>
#include <string_view>

namespace bloom::core {

// Validates strict Unicode scalar UTF-8. This intentionally performs no normalization,
// locale-sensitive comparison, or text shaping.
[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;

// Orders the encoded bytes as unsigned octets. Callers that require valid UTF-8 must validate
// separately; this function is also deterministic for hostile byte strings.
[[nodiscard]] std::strong_ordering compareUtf8Bytes(std::string_view left,
                                                    std::string_view right) noexcept;

} // namespace bloom::core
