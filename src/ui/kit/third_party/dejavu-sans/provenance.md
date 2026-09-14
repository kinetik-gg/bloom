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
never typed from memory. The original three TTFs were extracted from that verified archive; their digests are
the digests of the extracted members, and the retained Book face is those bytes unchanged.

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

## Renderer-only retention

GRAMMAR-1 replaces interface typography with Inter. Only the unmodified Book face remains,
at the exact path embedded by the render module's text source. Bold and Oblique are removed
from the UI resource pack and the repository. The render module and its embedded bytes are
unchanged; interface TypeRole never selects DejaVu Sans.

## Status

Pinned and reviewed. Adding a weight, changing a face, or moving to a new upstream release replaces
this record wholesale.
