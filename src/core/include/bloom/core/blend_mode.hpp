#ifndef BLOOM_CORE_BLEND_MODE_HPP
#define BLOOM_CORE_BLEND_MODE_HPP

#include <array>
#include <cstdint>
#include <optional>

namespace bloom::core {

// How one layer's pixels combine with what is already beneath them.
//
// This lives in bloom_core, not in bloom_render or bloom_document, because three modules need the
// SAME closed vocabulary and the same stable integer mapping: the document schema validates and
// persists the stored integer, the CPU compositing kernel switches on the mode, and the runtime
// carries it from one to the other. A second enumeration in either of those modules would be two
// spellings of one contract, and a renumbering in one of them would silently re-interpret every
// saved document.
//
// The enumerator VALUES are the durable stored mapping (docs/architecture/color-management.md,
// "Blend modes"). Normal is 0, so a document that stores no blend mode at all -- every Layer Output
// written before the blend-mode slice -- decodes as Normal and renders exactly the picture it
// always did. Appending a mode is additive; renumbering or reusing a value is not, and would need a
// new schema key rather than a new number.
//
// The set is deliberately the separable modes only: each one combines the corresponding channels of
// two pixels and nothing else, so each has one closed formula over scene-linear values and none of
// them needs a luminance, saturation, or hue model that would have to commit to a colorimetry the
// process space does not fix.
enum class BlendMode : std::uint8_t {
    Normal = 0,
    Add = 1,
    Multiply = 2,
    Screen = 3,
    Overlay = 4,
    Darken = 5,
    Lighten = 6,
    Difference = 7,
};

// Every mode, in the authoring order every surface offers them in: Normal first because it is the
// default, then the modes grouped by what they do to the picture -- brighten, darken, contrast,
// extremes, difference. One list, so the timeline dropdown, the Properties row, and the node card
// cannot present three different orders of the same vocabulary.
inline constexpr std::array<BlendMode, 8> kBlendModes{
    BlendMode::Normal,  BlendMode::Add,    BlendMode::Multiply, BlendMode::Screen,
    BlendMode::Overlay, BlendMode::Darken, BlendMode::Lighten,  BlendMode::Difference,
};

inline constexpr BlendMode kDefaultBlendMode = BlendMode::Normal;

[[nodiscard]] constexpr std::int64_t blendModeStoredValue(const BlendMode mode) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(mode));
}

// The only way a stored integer becomes a BlendMode. An unknown value is rejected rather than
// clamped or folded to Normal: a document carrying a mode this build does not implement is a
// document this build cannot render faithfully, and the document layer refuses it at validation
// instead of quietly drawing something else.
[[nodiscard]] constexpr std::optional<BlendMode>
blendModeFromStoredValue(const std::int64_t value) noexcept {
    for (const auto mode : kBlendModes) {
        if (blendModeStoredValue(mode) == value) {
            return mode;
        }
    }
    return std::nullopt;
}

} // namespace bloom::core

#endif // BLOOM_CORE_BLEND_MODE_HPP
