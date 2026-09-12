# stb_truetype 1.26 Security Review

Reviewed: 2026-09-12

## Upstream's Own Statement

The header opens with its own unconditional warning, reproduced verbatim from the vendored bytes:

```text
   NO SECURITY GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES

This library does no range checking of the offsets found in the file,
meaning an attacker can use it to read arbitrary memory.
```

That statement, not any individual advisory, is the controlling fact for this intake. stb_truetype
is qualified here **only** for parsing font bytes Bloom itself pins, and that boundary is the entire
basis of the disposition below.

## Reachability In Bloom

- The only font bytes that reach this code are the build-time constant produced by
  `bloom_render_fonts`: `src/ui/kit/third_party/dejavu-sans/DejaVuSans.ttf`, whose bytes are
  digest-recorded in `src/ui/kit/third_party/dejavu-sans/provenance.md`
  (`7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954`) and embedded as a byte array
  at CMake configure time. No runtime code path opens a font file.
- Bloom has no font import, no font-family selection, no project-referenced font asset, and no
  media import pipeline that could carry one. `bloom.text-source` fixes the font; nothing in the
  document schema names a font at all. A project file cannot redirect this code at other bytes.
- The only attacker-influenced inputs to the text path are the content string, the pixel size, and
  the color, all validated by the document/compile/evaluate layers before rasterization: the size is
  finite and positive and bounded, the string is validated UTF-8, and a codepoint with no glyph in
  the embedded font resolves to glyph index 0 through stb's own `cmap` lookup rather than to an
  out-of-range index.
- `STBTT_assert` is defined to nothing, so no malformed-data assertion can abort an evaluation; the
  documented consequence is that the library's out-of-range reads are not caught by an assert
  either, which is exactly why the trusted-input boundary above is mandatory rather than advisory.

## Known Vulnerabilities

GitHub Security Advisories for `nothings/stb` at review time: zero (repository advisories endpoint
and the global advisory database queried for `stb`, both empty). Upstream does not publish
advisories, so NVD is the authoritative list. An NVD keyword search for `stb_truetype` returns 13
CVEs; every one of them is a malformed-font-file parsing defect, and every one is unreachable under
the trusted-input boundary above.

| CVE | Affected | Defect | Disposition |
| --- | --- | --- | --- |
| CVE-2020-6617, -6619, -6623 | through 1.22 | assertion failures in `stbtt__cff_int`, `stbtt__buf_seek`, `stbtt__cff_get_index` | not-affected: fixed before 1.26, and Bloom compiles asserts out |
| CVE-2020-6618, -6620, -6621, -6622 | through 1.22 | heap over-reads in `stbtt__find_table`, `stbtt__buf_get8`, `ttUSHORT`, `stbtt__buf_peek8` | not-affected: version floor — the locked header is 1.26 |
| CVE-2022-25514, -25515, -25516 | 1.26 | heap buffer over-reads in `ttUSHORT`, `ttULONG`, `stbtt__find_table` on crafted fonts; disputed upstream on the grounds of the header's own trusted-input disclaimer | accepted-risk: reachable only from an untrusted font file, which Bloom never supplies |
| CVE-2026-5314, -5315 | up to 1.26 | out-of-bounds reads in `stbtt_InitFont_internal` and `stbtt__buf_get8`; public exploits, vendor unresponsive | accepted-risk: same boundary — both require a crafted font, and `InitFont` only ever sees the embedded DejaVu bytes |
| CVE-2026-18497 | up to 1.26 | heap buffer overflow in `stbtt__GetGlyphShapeTT()` on a malformed TTF with an inflated `endPtsOfContours` and truncated glyph data | accepted-risk: same boundary; the embedded font is well-formed and digest-pinned |

No fixed upstream version exists for the 2022 and 2026 entries: 1.26 is still the current release,
and upstream's position is that these are use-outside-the-documented-contract reports rather than
defects. Upgrading is therefore not an available mitigation, and no patch is applied (patching a
vendored header would also void the byte-identical provenance claim).

## Disposition

Accepted for intake at the pinned commit, **scoped to Bloom-pinned font bytes only**. The accepted
risks above are all conditioned on the same precondition, so the re-review trigger is not a version
bump but a capability change:

- Any feature that lets a font file, a font family name, or font bytes from a project, a user, or a
  network reach this code — font import, project-referenced font assets, a font picker, a scripting
  hook, media import of a font — must not ship against this component as qualified here. Such a
  feature requires a new security review and either a hardened/fuzzed parser, an out-of-process
  sandbox, or a different font engine.
- Re-review also on any new `nothings/stb` advisory, on any change to the pinned commit, and if
  upstream ever ships a release that fixes the 2022/2026 reports.
