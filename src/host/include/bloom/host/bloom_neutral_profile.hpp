#pragma once

#include <bloom/core/sha256.hpp>

namespace bloom::host {

// The reviewed, checked-in content revision digest for the immutable Bloom Neutral v1 built-in
// OCIO payload at assets/ocio/neutral-v1/config.ocio (durable locator
// bloom::document::kBloomNeutralConfigUriV1, exactly "bloom://ocio/neutral-v1/config.ocio" --
// see color_settings.hpp, which already owns that URI constant; it is not duplicated here).
//
// This is the doc-normative digest from docs/architecture/color-management.md's "OCIO Content
// Revision Version 1" for a built-in payload -- an envelope hash, not a plain payload hash:
//
//   SHA-256("BloomOcioRevision\0" || u16(1) || u8(1) || u64(632) || exactPayloadBytes)
//
// where u16/u8/u64 are unsigned big-endian, `u8(1)` is locatorKind 1 (immutable built-in), and
// `u64(632)` is the exact byte count of assets/ocio/neutral-v1/config.ocio. See
// assets/ocio/neutral-v1/provenance.md for the full origin record, including the reference plain
// payload SHA-256 and the derivation of this envelope value.
//
// The asset's bytes are immutable forever once merged: any future content change requires a new
// built-in URI (a new assets/ocio/<name>/config.ocio directory) with its own digest constant,
// never an edit of this value or the payload it names.
//
// bloom.host.bloom-neutral-profile (see tests/bloom_neutral_profile_tests.cpp) re-derives this
// exact envelope from the checked-in payload through core::Sha256Hasher at test time -- verifying
// the FORMULA, not just the payload bytes -- so the asset and this constant can never silently
// drift apart.
inline constexpr core::Sha256Digest kBloomNeutralV1ConfigDigest = core::Sha256Digest::fromBytes({{
    0xa6, 0xfc, 0x07, 0xc9, 0x5c, 0xea, 0x14, 0x89, //
    0x7b, 0x72, 0x59, 0x92, 0xa1, 0x79, 0xd3, 0x50, //
    0x55, 0xcc, 0xcc, 0xe2, 0xff, 0xaf, 0x71, 0x9c, //
    0xa7, 0x3b, 0x4a, 0x28, 0x9c, 0x89, 0xea, 0xcf, //
}});

} // namespace bloom::host
