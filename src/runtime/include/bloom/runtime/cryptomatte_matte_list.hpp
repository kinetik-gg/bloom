#pragma once

// #364 Cryptomatte matte names and IDs (Cryptomatte specification v1.2, as Nuke reads it): an
// object name's ID is MurmurHash3_x86_32(name, seed 0) used as float32 bits, with bit 23 flipped
// when the exponent bits are 0 or 255 so every ID is a finite normal float. The matte list is the
// Nuke syntax: comma-separated entries, `\,` for a literal comma, surrounding spaces ignored, and
// `<float>` or `<0xHEX>` for a raw ID.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

[[nodiscard]] std::uint32_t cryptomatteNameId(std::string_view name) noexcept;
[[nodiscard]] std::vector<std::string> splitCryptomatteMatteList(std::string_view list);
[[nodiscard]] std::string joinCryptomatteMatteList(std::span<const std::string> mattes);
[[nodiscard]] std::optional<std::uint32_t> parseCryptomatteRawId(std::string_view entry) noexcept;
// A raw-ID entry written so that it parses back to exactly `id`.
[[nodiscard]] std::string formatCryptomatteRawId(std::uint32_t id);

} // namespace bloom::runtime
