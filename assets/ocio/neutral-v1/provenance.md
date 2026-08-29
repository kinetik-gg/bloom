# Bloom Neutral v1 OCIO Configuration Provenance

Reviewed: 2026-08-29

## Origin

`config.ocio` in this directory is authored directly against
`docs/architecture/color-management.md`'s Bloom Neutral v1 paragraph and Version 1 Decisions --
it is not derived from, or a copy of, any third-party OCIO configuration. It is a minimal
OpenColorIO v2 configuration written and validated against OpenColorIO 2.5.2 (the version this
repository's documentation cites in its Primary References), using OCIO's native `interop_id`
color-space field (introduced in the OpenColorIO 2.x line this repository targets) so that the
two required Color Interop Forum identifiers are exact, unambiguous, machine-checkable mappings
rather than a name, role, alias, or approximate-chromaticities substitute.

## Exposed Color Interop IDs

| Color-space name | `interop_id` | Role |
| --- | --- | --- |
| `lin_rec709_scene` | `lin_rec709_scene` | Scene-referred process reference space (`scene_linear` role); linear-light Rec.709 primaries, D65 white point |
| `srgb_rec709_display` | `srgb_rec709_display` | Display/output space; sRGB (IEC 61966-2-1 EOTF) over Rec.709 primaries |

Both interop IDs were confirmed round-trip-readable via `OCIO::Config::getColorSpace(name)
.getInteropID()` against the exact checked-in bytes. Because the process reference space and the
display space share identical Rec.709/D65 primaries, the display space's `from_scene_reference`
transform is exactly the sRGB piecewise transfer function (`ExponentWithLinearTransform`, gamma
2.4, offset 0.055, `direction: inverse`) with no primaries matrix -- an exact transform, not an
approximate-chromaticities shortcut. Numerically verified against the closed-form sRGB OETF
(`v = 12.92*l` at or below the 0.0031308 breakpoint, `v = 1.055*l^(1/2.4) - 0.055` above it) and
its EOTF inverse: maximum observed float32 discrepancy 6.7e-6 across a 0..1 sample sweep on both
`lin_rec709_scene -> srgb_rec709_display` and the reverse direction, consistent with float32
rounding, not formula error.

The config also validates cleanly under `OCIO::Config::validate()` on OpenColorIO 2.5.2: it
declares exactly one display (`srgb_rec709_display`) with exactly one view
(`srgb_rec709_display`), and the `scene_linear`/`default` roles both resolve to
`lin_rec709_scene`.

## Immutability

Per `docs/architecture/color-management.md`: "Any content change requires a new built-in URI."
The bytes of `config.ocio` in this directory are the durable identity bound to
`bloom://ocio/neutral-v1/config.ocio` forever. A future revision to Bloom Neutral's transform,
roles, or structure -- for any reason, including a defect found later -- MUST land at a new URI
(e.g. `bloom://ocio/neutral-v2/config.ocio`) with its own directory and its own digest; this file
is never edited in place once merged.

## Digest

Two SHA-256 values are recorded for this payload. Only the second is a durable value; the first
is a reference identity of the raw bytes, useful for `sha256sum`-style spot checks, but it is NOT
what `kBloomNeutralV1ConfigDigest` carries.

**Plain payload SHA-256** (reference identity only, not a project-persisted value):

```
SHA-256(config.ocio bytes) = 6cb2d1460218958ffede3fb747f3a7d46d355ac0d3d5e6452eb9a5a47d63373f
```

Computed directly with `sha256sum assets/ocio/neutral-v1/config.ocio` over the exact checked-in
632-byte payload (26 lines, ASCII-only, LF line endings, single trailing LF, no timestamps or
environment references).

**Doc-normative OCIO Content Revision Version 1 envelope digest** -- this is the value
`kBloomNeutralV1ConfigDigest` (`src/host/include/bloom/host/bloom_neutral_profile.hpp`) actually
carries, and the value `makeBloomNeutralColorSettingsV1()` installs into
`OcioConfigReference.expectedRevision.digest`:

```
SHA-256("BloomOcioRevision\0" || u16(1) || u8(1) || u64(632) || config.ocio bytes)
  = a6fc07c95cea14897b725992a179d35055cccce2ffaf719ca73b4a289c89eacf
```

Per `docs/architecture/color-management.md`'s "OCIO Content Revision Version 1": for a built-in
payload, `expectedRevision.digest = SHA-256("BloomOcioRevision\0" || u16(1) || u8(locatorKind) ||
u64(payloadByteCount) || exactPayloadBytes)`, with all integers unsigned big-endian. Here
`u16(1)` is the revision format version, `u8(1)` is `locatorKind` 1 (immutable Bloom built-in, per
the doc: "`locatorKind` is `1` for an immutable built-in"), and `u64(632)` is the exact byte count
of `assets/ocio/neutral-v1/config.ocio`. This constant conforms to that formula exactly --
`bloom.host.bloom-neutral-profile` (`src/host/tests/bloom_neutral_profile_tests.cpp`) rebuilds the
envelope byte-for-byte from the checked-in asset and asserts equality with
`kBloomNeutralV1ConfigDigest`, so a regression in the asset bytes, the envelope formula, or the
constant is caught, not just a regression in the asset bytes alone.

The repository `.gitattributes` marks `assets/ocio/**` `-text` so these digest-bound bytes are
never line-ending normalized.

## Status

Authored and reviewed bytes for the version 1 durable Bloom Neutral profile, with a digest
constant conforming to `docs/architecture/color-management.md`'s OCIO Content Revision Version 1
formula. OCIO parsing, processor construction, transform execution, the built-in registry, and
cross-platform qualification remain pending Batch 7 implementation per the color-management doc;
this asset supplies only the frozen, checked-in payload and its doc-normative digest.
