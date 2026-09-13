# stb_truetype 1.26 License And Feature Review

Reviewed: 2026-09-12

## License

- SPDX expression: `MIT OR Unlicense`. Bloom's distribution relies on the MIT alternative, because
  MIT is an unambiguous grant in every jurisdiction while a public-domain dedication is not, and
  shipping the notice MIT requires also satisfies the Unlicense alternative. Permissive, compatible
  with Bloom's Apache-2.0 distribution and with static linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes. They travel three ways — beside the
  vendored header at `src/render/third_party/stb_truetype/LICENSE`, in this component directory, and
  inside the application's "Open Source Licenses…" catalog, which
  `src/ui/generate_third_party_license_catalog.cmake` builds from every
  `dependencies/licenses/*/LICENSE`. `THIRD_PARTY_NOTICES.md` carries the repository-level entry.
- `copyrightFiles`/`noticeFiles`: empty by review. The single copyright line ("Copyright (c) 2017
  Sean Barrett") lives inside the license text itself. The header additionally names its
  contributors in comments, which the MIT notice condition does not require Bloom to reproduce
  separately — and those comments ship anyway, since the whole header is checked in.
- `sourceObligation`: none. `modified`: false, with an empty patch set.

## Why stb_truetype Is In The Closure

Bloom needs TrueType glyph rasterization on the Qt-free CPU reference path. Before this slice the
repository had none: `docs/architecture/layer-graph-model.md` recorded that `AddTextLayer` refuses
"until portable CPU text rendering exists" precisely because "the repository has no Qt-free glyph
rasterization facility; UI font assets and Qt painting are not a portable document evaluator".
`src/render` and `src/runtime` may not use Qt (`AGENTS.md`), and the evaluator must produce
identical pixels on Linux, macOS, and Windows, so a platform text API (DirectWrite, CoreText,
Fontconfig/Pango) is disqualified on both determinism and portability.

stb_truetype is the smallest dependency that closes that gap: one header, no build system, no
transitive dependencies, no allocator or I/O of its own beyond macros the caller supplies, and a
deterministic scanline rasterizer that produces an 8-bit coverage bitmap from a glyph outline. The
alternative candidates are all substantially heavier for this slice — FreeType is a full
multi-format font engine with its own build, configuration surface, and transitive closure;
HarfBuzz solves shaping, which this slice does not do (fixed font, left-to-right advance-only
layout, no complex-script support).

## Feature Minimization

There is no build system to minimize, so minimization is expressed in how Bloom compiles the
header:

- Exactly one translation unit in the repository defines `STB_TRUETYPE_IMPLEMENTATION`
  (`src/render/stb_truetype_implementation.cpp`). Every other Bloom file that needs glyph data goes
  through Bloom's own `bloom/render/text_raster.hpp`, which does not include this header at all, so
  the vendored declarations are not visible outside that one TU.
- `STBTT_STATIC` is defined, so every symbol the header emits has internal linkage and nothing from
  stb enters `bloom_render`'s exported symbol table or any Bloom public header.
- `STBTT_assert` is defined to nothing. The upstream asserts fire on malformed font data; Bloom's
  font bytes are a digest-pinned build-time constant (see `security.md`), a release build would
  compile them out anyway, and an abort inside an evaluator is not an acceptable failure mode for a
  render task that must return a diagnostic.
- The header's packing/atlas helpers (`stbtt_PackBegin` and friends), SDF helpers, and the
  rasterizer-v1 path are not referenced by Bloom. They are compiled (a single-header library has no
  switch to drop them) but reach no Bloom call site.
- Warning suppression is scoped to that one translation unit through an `OBJECT` library,
  `bloom_render_stb_truetype`, which never calls `bloom_enable_warnings()` and therefore also never
  attaches clang-tidy. Bloom's own adapter code stays under the full strict-warning and clang-tidy
  profile. This mirrors the intake contract's "all third-party targets are private implementation
  dependencies and treated as system headers for Bloom warning policy without suppressing Bloom
  adapter warnings".
- No superbuild recipe, no `ExternalProject_Add`, no `FetchContent`: the normal root build gains no
  dependency acquisition logic, which the repository-shape rule forbids.

## Not A Lock Component

See `provenance.md`'s "Status" section for the determination and its citations: an in-tree header
that installs nothing into a qualified prefix has no recipe, no `profileBuild`, and no prefix
artifacts, so the lock v1 shape cannot represent it without fabrication. The lock is
generator-owned; this component record exists without one, following the
`src/ui/kit/third_party/*` precedent for checked-in vendored content.
