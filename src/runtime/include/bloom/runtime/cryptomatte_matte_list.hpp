#pragma once

// #364 Cryptomatte matte names and IDs (Cryptomatte specification v1.2, as Nuke reads it): an
// object name's ID is MurmurHash3_x86_32(name, seed 0) used as float32 bits, with bit 23 flipped
// when the exponent bits are 0 or 255 so every ID is a finite normal float. The matte list is the
// Nuke syntax: comma-separated entries, `\,` for a literal comma, surrounding spaces ignored, and
// `<float>` or `<0xHEX>` for a raw ID.

#include <array>
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

// The first rank pair of one Cryptomatte type found among channel names: `<layer>00.R` (ID) and
// `<layer>00.G` (coverage), with R/G, r/g or red/green suffixes.
struct CryptomatteRankChannels final {
    std::string layer;
    std::string id;
    std::string coverage;
    friend bool operator==(const CryptomatteRankChannels&,
                           const CryptomatteRankChannels&) = default;
};
// Every Cryptomatte type among `channels` (full channel names), in first-appearance order.
[[nodiscard]] std::vector<CryptomatteRankChannels>
cryptomatteTypes(std::span<const std::string> channels);

// The ID-colour preview colour of `id` (linear RGB in [0.15, 1]): three bytes of the ID's bits,
// so every object keeps one distinct, stable colour (the Cryptomatte preview convention).
[[nodiscard]] std::array<float, 3> cryptomatteIdColour(std::uint32_t id) noexcept;

} // namespace bloom::runtime
