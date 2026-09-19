# SPIRV-Headers License And Feature Review

Reviewed: 2026-09-19

## License

- Whole-component SPDX expression: `MIT AND CC-BY-4.0`. The component is code headers under MIT;
  `include/spirv/spir-v.xml` is MIT; `tools/buildHeaders/jsoncpp/*` is Public Domain / MIT; the
  `**.md`, `WORKSPACE`, and `.git` non-code metadata are CC-BY-4.0. Both texts are bound beside this
  file, plus the upstream `REUSE.toml` that maps them.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Reached Scope And Minimization

- This is a **build-only header source**, not a shipped or runtime artifact. The recipe
  (`dependencies/superbuild/projects/spirv-headers.cmake`) installs nothing
  (`SPIRV_HEADERS_ENABLE_INSTALL=OFF`) and disables tests (`SPIRV_HEADERS_ENABLE_TESTS=OFF`), so no
  test tree is reached. The only role recorded is `license`: the component contributes the MIT
  notice for the headers used by SPIRV-Tools.
- SPIRV-Tools consumes this tree through the explicit `SPIRV-Headers_SOURCE_DIR` path, ordered by a
  build dependency edge; no system header resolution and no in-recipe fetch occur.
