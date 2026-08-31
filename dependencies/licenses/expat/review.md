# expat 2.8.3 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `MIT`. Permissive; compatible with Bloom's Apache-2.0 distribution and static
  linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: both copyright lines live inside the license
  text itself. `sourceObligation: none`; `modified: false` with an empty patch set.

## Why expat Is In The Closure

expat is one of OpenColorIO 2.5.2's six unconditionally required dependencies (see
`dependencies/licenses/opencolorio/review.md`'s "Mandatory Transitive Closure") — OCIO's core
library parses XML-based color-transform files (e.g. ASC CDL `.cdl`/`.cc`/`.ccc`) using expat
directly; there is no build switch that removes this dependency from a static OCIO build.

## Feature Minimization

The recipe (`dependencies/superbuild/projects/expat.cmake`) builds only the static library:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `EXPAT_SHARED_LIBS` | `OFF` | Matches the static, feature-minimized prefix convention already used by every other recipe. |
| `EXPAT_BUILD_TOOLS` | `OFF` | The `xmlwf` well-formedness-checking CLI is not part of Bloom's linked surface. |
| `EXPAT_BUILD_EXAMPLES` | `OFF` | Example programs are not shipped. |
| `EXPAT_BUILD_TESTS` | `OFF` | Test suite is not part of the shipped surface. |
| `EXPAT_BUILD_DOCS` | `OFF` | Keeps generated man pages out of the prefix. |
| `EXPAT_BUILD_FUZZERS` | `OFF` | Matches upstream default; OSS-Fuzz harnesses are not shipped. |
| `EXPAT_BUILD_PKGCONFIG` | `OFF` | Bloom's dependency consumption is exclusively CMake-config-based (`cmake/BloomDependencyPrefix.cmake`'s restricted package search); a `.pc` file serves an ambient pkg-config discovery path Bloom's qualified/developer-system modes do not use. |
| `EXPAT_ENABLE_INSTALL` | `ON` (upstream default) | Required so the CMake package config (`expat-config.cmake`, target `expat::libexpat`) lands in the shared prefix for OpenColorIO's `find_package(expat CONFIG)`. |

expat's XML-parsing *feature* surface — `EXPAT_DTD`, `EXPAT_GE`, `EXPAT_NS` (all upstream default
`ON`) — is left untouched. These select what expat can correctly parse (parameter entities,
general entities, XML namespaces), not a build/tooling surface; disabling any of them narrows the
XML dialect OCIO's CDL/CLF/CTF file parsing can handle and is not a Bloom-owned capability
decision to make on OCIO's behalf. `EXPAT_CONTEXT_BYTES`, `EXPAT_ATTR_INFO`, `EXPAT_LARGE_SIZE`,
`EXPAT_MIN_SIZE`, `EXPAT_CHAR_TYPE`, and the `EXPAT_WITH_*` entropy-source detection options are
also left at their upstream defaults: none of them is a distinct capability OCIO's own build
selects or exposes to Bloom, so this recipe's minimization scope (matching the imath/libdeflate/
openexr precedent of leaving functional defaults alone and disabling only build-surface options)
does not reach them.
