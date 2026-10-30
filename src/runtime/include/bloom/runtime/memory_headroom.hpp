#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

// The only safety margins on Bloom's memory admission (docs/user-guide/memory.md). Usable RAM is
// the measured available system memory (within real OS/cgroup limits) minus ramBytes; usable VRAM
// is the driver's live device budget minus vramBytes. Work and caches use everything else.
//
// The values are global preferences stored as decimal byte strings under the keys below. This
// header is Qt-free so the settings model, the processing root and tests share one definition.
namespace bloom::runtime {

// Without a saved preference each headroom is this share of the capacity it protects (owner
// decision, 2026-10-30): RAM, the smaller of physical memory and an observed process/cgroup limit;
// VRAM, the device-local memory of the largest GPU Bloom has measured. No floor or ceiling.
inline constexpr std::uint64_t kDefaultHeadroomPercent = 10;
// Used only while the protected capacity is still unmeasured (no physical-memory reading, or no
// GPU measured yet for VRAM).
inline constexpr std::uint64_t kUnmeasuredHeadroomBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaximumMemoryHeadroomBytes = 256ULL * 1024 * 1024 * 1024;
inline constexpr std::string_view kRamHeadroomKey = "memory/ramHeadroomBytes";
inline constexpr std::string_view kVramHeadroomKey = "memory/vramHeadroomBytes";

// Exactly kDefaultHeadroomPercent of `capacity`.
[[nodiscard]] constexpr std::uint64_t
defaultHeadroomForCapacity(const std::uint64_t capacity) noexcept {
    return capacity / 100 * kDefaultHeadroomPercent +
           capacity % 100 * kDefaultHeadroomPercent / 100;
}
// The computed defaults, evaluated when called (preference load, Restore Defaults). They read only
// cached measurements: physical memory, the ledger's observed process limit and the recorded GPU
// device-local size; they never probe files on the calling thread.
[[nodiscard]] std::uint64_t defaultRamHeadroomBytes() noexcept;
[[nodiscard]] std::uint64_t defaultVramHeadroomBytes() noexcept;
// The GPU admission policy records each measured device's local memory; the largest one sets the
// VRAM default. Thread-safe.
void recordDeviceLocalMemoryBytes(std::uint64_t bytes) noexcept;

struct MemoryHeadroom final {
    std::uint64_t ramBytes = defaultRamHeadroomBytes();
    std::uint64_t vramBytes = defaultVramHeadroomBytes();

    friend bool operator==(const MemoryHeadroom&, const MemoryHeadroom&) = default;
};

[[nodiscard]] constexpr bool isValidHeadroomBytes(const std::uint64_t bytes) noexcept {
    return bytes <= kMaximumMemoryHeadroomBytes;
}

// A stored value: decimal digits only, within [0, 256 GiB]. Missing, empty, malformed or out of
// range text is no value; the caller then selects the computed default.
[[nodiscard]] inline std::optional<std::uint64_t>
parseHeadroomBytes(const std::string_view text) noexcept {
    std::uint64_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || stop != end || !isValidHeadroomBytes(value))
        return std::nullopt;
    return value;
}

} // namespace bloom::runtime
