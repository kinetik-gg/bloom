#pragma once

// Private (non-FILE_SET, non-installed) header shared by the flat OpenEXR writer and reopen
// verifier, mirroring how bloom_color_ocio keeps its real third-party types out of any public
// header (ocio_internal.hpp). Nothing here names an OpenEXR/Imath type; it only names the closed
// version 1 header allowlist so the writer and verifier stay driven by one literal table copied
// from docs/architecture/frame-output.md ("Flat OpenEXR Preset Version 1"), not from each other.

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::output::detail {

// CONTRACT DISCREPANCY (reported, not silently worked around; see the F1 task report): the
// qualified OpenEXR 3.4.15 library rejects a data/display window whose min or max coordinate
// magnitude reaches INT32_MAX/2 -- narrower than the full signed-32 domain
// docs/architecture/frame-output.md's "Flat OpenEXR Preset Version 1" describes ("checked
// inclusive signed-32 bounds"). Confirmed both empirically (a window at literal INT32_MAX throws
// "Invalid display window" from Imf::OutputFile) and against the vendored source
// (OpenEXRCore/validation.c's validate_image_dimensions: `kLargeVal = INT32_MAX / 2`; a window is
// rejected when `min <= -kLargeVal || max >= kLargeVal`). This is a deliberate internal-overflow
// safety margin in the library, not a bug reachable through public API choice -- the OpenEXRCore
// C API enforces the identical check the classic C++ API delegates to. The writer enforces this
// real ceiling itself so an in-range-per-doc-but-out-of-range-per-library window is a clean typed
// rejection before staging rather than a caught library exception.
inline constexpr std::int64_t kFlatExrLibraryCoordinateCeilingV1 = 1'073'741'823LL; // INT32_MAX/2

[[nodiscard]] constexpr bool
flatExrWindowWithinLibraryCeilingV1(const std::int64_t origin,
                                    const std::uint32_t extent) noexcept {
    if (extent == 0) {
        return false;
    }
    const auto maxCoordinate = origin + static_cast<std::int64_t>(extent) - 1;
    return origin > -kFlatExrLibraryCoordinateCeilingV1 &&
           maxCoordinate < kFlatExrLibraryCoordinateCeilingV1;
}

struct FlatExrHeaderAttributeContractV1 final {
    std::string_view name;
    // The exact string OpenEXR's Imf::Attribute::typeName() returns for this attribute's type.
    std::string_view openExrTypeName;
};

inline constexpr std::string_view kFlatExrColorInteropIdV1 = "lin_rec709_scene";

// The closed version 1 header allowlist: exactly these ten attributes, nothing else. The
// verifier enumerates every attribute the reopened file actually carries and fails closed on
// anything outside this table (an unexpected name, a wrong type, or a missing entry) before it
// checks any value; the writer inserts exactly these ten. Order here is documentation order, not
// a serialization requirement -- the doc places no ordering constraint on the header's attribute
// list (only on the channel list, checked separately).
inline constexpr std::array<FlatExrHeaderAttributeContractV1, 10> kFlatExrHeaderAttributesV1{{
    {"channels", "chlist"},
    {"compression", "compression"},
    {"dataWindow", "box2i"},
    {"displayWindow", "box2i"},
    {"lineOrder", "lineOrder"},
    {"pixelAspectRatio", "float"},
    {"screenWindowCenter", "v2f"},
    {"screenWindowWidth", "float"},
    {"chromaticities", "chromaticities"},
    {"colorInteropID", "string"},
}};

// Channel names in the exact required physical (lexical) header order A, B, G, R.
inline constexpr std::array<std::string_view, 4> kFlatExrChannelNamesLexicalV1{"A", "B", "G", "R"};

} // namespace bloom::output::detail
