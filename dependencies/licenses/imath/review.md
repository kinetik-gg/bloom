# Imath 3.2.3 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `BSD-3-Clause`. Permissive; compatible with Bloom's Apache-2.0 distribution
  and static linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright line lives inside the license
  text and Imath imposes no separate notice file. `sourceObligation: none`; `modified: false`
  with an empty patch set.

## Feature Minimization

Imath is a pure math library (vectors, matrices, quaternions, half-float, interval/box types)
consumed by OpenEXR and, transitively, by Bloom's flat OpenEXR write path
(`docs/architecture/frame-output.md`). Bloom uses none of Imath directly today; it is an
indirect, private implementation dependency of `bloom_dependency_openexr`. The recipe
(`dependencies/superbuild/projects/imath.cmake`) enables only the core static library:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `PYTHON` | `OFF` | Boost.Python `PyImath` bindings are unrelated to Bloom's C++-only consumption; dependency-intake.md's feature-minimization section requires enabling only capabilities Bloom owns. |
| `PYBIND11` | `OFF` | Same as `PYTHON`, for the pybind11-based bindings variant. |
| `BUILD_TESTING` | `OFF` | Imath's `include(CTest)` defaults `BUILD_TESTING` ON; explicit `OFF` keeps `src/ImathTest` out of the build, matching frame-output.md's "tests off" instruction. |
| `BUILD_WEBSITE` | `OFF` | Keeps the readthedocs documentation source out of the build ("docs off"). |
| `BUILD_SHARED_LIBS` | `OFF` (superbuild-global) | Matches the static, feature-minimized prefix convention already used by zlib/libzip. |

No other Imath option changes behavior relevant to Bloom's usage; `IMATH_INSTALL_PKG_CONFIG` and
`IMATH_HALF_USE_LOOKUP_TABLE` are left at their upstream defaults (both ON) since neither expands
the built surface or introduces an external dependency.
