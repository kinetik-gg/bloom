#pragma once

#include <compare>
#include <cstddef>
#include <string_view>

namespace bloom::core {

// Validates strict Unicode scalar UTF-8. This intentionally performs no normalization,
// locale-sensitive comparison, or text shaping.
[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;

// Orders the encoded bytes as unsigned octets. Callers that require valid UTF-8 must validate
// separately; this function is also deterministic for hostile byte strings.
[[nodiscard]] std::strong_ordering compareUtf8Bytes(std::string_view left,
                                                    std::string_view right) noexcept;

// One decoded Unicode scalar and the number of bytes it occupied. `length == 0` means the bytes at
// the requested offset are not a well-formed scalar, in which case `value` is zero; callers must
// not advance past a failure.
struct Utf8Scalar final {
    char32_t value = 0;
    std::size_t length = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept { return length != 0; }

    friend constexpr bool operator==(const Utf8Scalar&, const Utf8Scalar&) noexcept = default;
};

// Decodes the scalar beginning at `offset`, applying exactly the acceptance rules isValidUtf8()
// applies -- no overlong forms, no surrogates, nothing above U+10FFFF -- so a string that validates
// decodes completely and a string that does not fails at the same byte. Performs no normalization,
// case folding, or shaping. An offset at or past the end is a failure, not an exception.
[[nodiscard]] Utf8Scalar decodeUtf8Scalar(std::string_view value, std::size_t offset) noexcept;

} // namespace bloom::core
