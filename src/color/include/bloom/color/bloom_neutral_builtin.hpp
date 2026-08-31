#pragma once

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <string_view>

namespace bloom::color {

// The single reviewed source of truth for the immutable Bloom Neutral v1 built-in OCIO payload's
// identity (issue #95 decision 2: "the digest lives in ONE place"). This header deliberately has
// no OCIO dependency and lives in the dependency-light `bloom_color` target -- not the heavier
// OpenColorIO-linked `bloom_color_ocio` target -- so that `src/host` can reference the digest by
// linking only `bloom_color` (see src/host/include/bloom/host/bloom_neutral_profile.hpp, which
// re-points to this constant, and src/color/include/bloom/color/ocio_builtin_registry.hpp, whose
// bloom_color_ocio built-in registry resolves the exact same constant against the config payload
// it embeds at build time).
//
// Exact durable locator URI. Repeated here as a plain string_view rather than including
// bloom/document/color_settings.hpp (bloom::document::kBloomNeutralConfigUriV1), which would
// introduce a color -> document module edge; the two constants are reviewed to stay byte-identical
// and bloom.host.bloom-neutral-profile (src/host/tests/bloom_neutral_profile_tests.cpp), which
// already links both bloom_document and (via this header) bloom_color, cross-checks them for
// exact equality.
inline constexpr std::string_view kBloomNeutralV1ConfigUri = "bloom://ocio/neutral-v1/config.ocio";

// Exact byte count of the checked-in assets/ocio/neutral-v1/config.ocio payload. The build-time
// embed in ocio_builtin_registry.cpp statically asserts its generated array matches this count.
inline constexpr std::size_t kBloomNeutralV1ConfigPayloadByteCount = 632;

// The doc-normative "OCIO Content Revision Version 1" built-in envelope digest for the exact
// checked-in assets/ocio/neutral-v1/config.ocio bytes -- see
// docs/architecture/color-management.md and assets/ocio/neutral-v1/provenance.md for the full
// derivation. Any content change requires a new built-in URI and a new digest constant; this
// value is never edited in place once merged.
inline constexpr core::Sha256Digest kBloomNeutralV1ConfigDigest = core::Sha256Digest::fromBytes({{
    0xa6, 0xfc, 0x07, 0xc9, 0x5c, 0xea, 0x14, 0x89, //
    0x7b, 0x72, 0x59, 0x92, 0xa1, 0x79, 0xd3, 0x50, //
    0x55, 0xcc, 0xcc, 0xe2, 0xff, 0xaf, 0x71, 0x9c, //
    0xa7, 0x3b, 0x4a, 0x28, 0x9c, 0x89, 0xea, 0xcf, //
}});

} // namespace bloom::color
