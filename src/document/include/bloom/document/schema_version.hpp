#pragma once

#include <compare>
#include <cstdint>

namespace bloom::document {

struct SchemaVersion final {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept { return major != 0; }

    friend constexpr auto operator<=>(const SchemaVersion&,
                                      const SchemaVersion&) noexcept = default;
};

} // namespace bloom::document
