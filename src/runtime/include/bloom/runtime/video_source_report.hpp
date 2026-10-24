#pragma once
#include <cstdint>
#include <string>

namespace bloom::runtime {
// What the most recent decode of one video source used, for status surfaces. It is published
// beside evaluation, never as an evaluation diagnostic, so it does not change frame cacheability.
struct VideoSourceReport final {
    // Lowercase hex SHA-256 of the asset's verified content.
    std::string assetDigest;
    // "software", "vaapi" or "software (fallback: <reason>)".
    std::string decode;
    // The visible note for an assumed input colour interpretation ("Colour tags missing —
    // assumed Rec.709. Change in Assets → Input Colour"); empty when nothing was assumed.
    std::string colourNote;
    // Increases with every report; larger is more recent.
    std::uint64_t sequence = 0;
    friend bool operator==(const VideoSourceReport&, const VideoSourceReport&) = default;
};
} // namespace bloom::runtime
