# DejaVu Sans Provenance

Reviewed: 2026-09-12

## Component

| Field | Value |
| --- | --- |
| Upstream project | DejaVu fonts (DejaVu Sans family) |
| Upstream repository | <https://github.com/dejavu-fonts/dejavu-fonts> |
| Pinned release | `version_2_37` (DejaVu 2.37) |
| Source archive | <https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip> |
| Source archive SHA-256 | `7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a` |
| License | Bitstream Vera Fonts Copyright (MIT-style permissive) + Arev Fonts Copyright, with all DejaVu changes in the public domain |
| License file | `LICENSE` (upstream `dejavu-fonts-ttf-2.37/LICENSE`, byte-identical) |
| Modified | No. Every file below is byte-identical to its file in the pinned archive. |

The archive SHA-256 above, and every per-file digest below, were computed with `sha256sum` over the
exact bytes downloaded from that URL on 2026-09-12 -- never copied from an upstream publication and
never typed from memory. The three TTFs were extracted from that verified archive; their digests are
the digests of the extracted members, and the vendored files are those bytes unchanged.

## License verdict

Redistribution inside Bloom, source and binary, is permitted, and the retained license text is what
the license itself requires.

- The Bitstream Vera Fonts Copyright grants, free of charge, the right "to reproduce and distribute
  the Font Software, including without limitation the rights to use, copy, merge, publish,
  distribute, and/or sell copies of the Font Software", on the condition that "the above copyright
  and trademark notices and this permission notice shall be included in all copies of one or more of
  the Font Software typefaces". `LICENSE` beside these files, and the entry in the repository's
  `THIRD_PARTY_NOTICES.md` and the application's embedded "Open Source Licenses..." catalog, are
  that inclusion.
- The Arev Fonts Copyright (Tavmjong Bah), covering the glyphs DejaVu imported from Arev, grants the
  same rights under the same notice condition.
- DejaVu's own changes on top of Bitstream Vera are placed in the public domain by the license
  file's first line, so they add no further condition.
- The two restrictions the licenses do impose are both satisfied by vendoring unchanged: a modified
  font must be renamed away from the "Bitstream"/"Vera" and "Tavmjong Bah"/"Arev" names (Bloom
  modifies nothing), and no copy of a typeface may be sold by itself (Bloom ships fonts only as part
  of the application, which the license explicitly allows: "The Font Software may be sold as part of
  a larger software package").

## Vendored files

| Path | SHA-256 | Registers as |
| --- | --- | --- |
| `LICENSE` | `7a083b136e64d064794c3419751e5c7dd10d2f64c108fe5ba161eae5e5958a93` | -- |
| `DejaVuSans.ttf` | `7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954` | `DejaVu Sans` (style `Book`, weight 400) |
| `DejaVuSans-Bold.ttf` | `e6476c1b80502924294eed40894c5b18e06c181444ca953e5334262df9c27724` | `DejaVu Sans` (style `Bold`, weight 700) |
| `DejaVuSans-Oblique.ttf` | `4af75fa16ee6d3ad43e1ecec41862c24954af26a55c6bb1ebb27bd486a50f5f4` | `DejaVu Sans` (style `Oblique`) |

## One family, three styles -- unlike the family this replaces

Plus Jakarta Sans, which DejaVu Sans replaces here, shipped its heavier static faces under their own
family names (`Plus Jakarta Sans Medium`), so `src/ui/kit/fonts.cpp` had to name each face
explicitly or silently get Regular. DejaVu's three faces all declare family name `DejaVu Sans` and
distinguish themselves by style (`Book` / `Bold` / `Oblique`) -- verified by reading the `name`
table of the three extracted files (name IDs 1/2/16/17) rather than assumed. The interface roles
therefore ask for the single family `DejaVu Sans` and let Qt's own weight matching pick the face:
the 500-weight UI roles resolve to Book and the 600-weight Title role resolves to Bold.
`src/ui/tests/kit_fonts_tests.cpp` asserts that resolution through `QFontInfo` rather than trusting
it.

The archive also carries DejaVu Serif, DejaVu Sans Mono, the Condensed and ExtraLight cuts, and
DejaVu Math TeX Gyre. None are vendored: Bloom's monospaced family is Geist Mono, and no implemented
component uses a serif, condensed, or math face.

The Oblique face is vendored on the product owner's explicit instruction for this task's shipped set
even though no implemented component asks for an italic role today; it registers with the other two
so the bundled family is complete rather than half-present.

## Status

Pinned and reviewed. Adding a weight, changing a face, or moving to a new upstream release replaces
this record wholesale.
