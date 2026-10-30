#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

// The only safety margins on Bloom's memory admission (docs/user-guide/memory.md). Usable RAM is
// the measured available system memory (within real OS/cgroup limits) minus ramBytes; usable VRAM
// is the driver's live device budget minus vramBytes. Work and caches use everything else.
//
// The values are global preferences stored as decimal byte strings under the keys below. This
// header is Qt-free so the settings model, the processing root and tests share one definition.
namespace bloom::runtime {

inline constexpr std::uint64_t kDefaultMemoryHeadroomBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaximumMemoryHeadroomBytes = 256ULL * 1024 * 1024 * 1024;
inline constexpr std::string_view kRamHeadroomKey = "memory/ramHeadroomBytes";
inline constexpr std::string_view kVramHeadroomKey = "memory/vramHeadroomBytes";

struct MemoryHeadroom final {
    std::uint64_t ramBytes = kDefaultMemoryHeadroomBytes;
    std::uint64_t vramBytes = kDefaultMemoryHeadroomBytes;

    friend bool operator==(const MemoryHeadroom&, const MemoryHeadroom&) = default;
};

[[nodiscard]] constexpr bool isValidHeadroomBytes(const std::uint64_t bytes) noexcept {
    return bytes <= kMaximumMemoryHeadroomBytes;
}

// A stored value: decimal digits only, within [0, 256 GiB]. Missing, empty, malformed or out of
// range text selects the 2 GiB default.
[[nodiscard]] inline std::uint64_t parseHeadroomBytes(const std::string_view text) noexcept {
    std::uint64_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || stop != end || !isValidHeadroomBytes(value))
        return kDefaultMemoryHeadroomBytes;
    return value;
}

} // namespace bloom::runtime
