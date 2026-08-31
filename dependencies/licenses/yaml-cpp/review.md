# yaml-cpp 0.9.0 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `MIT`. Permissive; compatible with Bloom's Apache-2.0 distribution and static
  linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright line lives inside the license
  text. `sourceObligation: none`; `modified: false` with an empty patch set.

## Why yaml-cpp Is In The Closure

yaml-cpp is one of OpenColorIO 2.5.2's six unconditionally required dependencies (see
`dependencies/licenses/opencolorio/review.md`'s "Mandatory Transitive Closure") — OCIO's core
library reads and writes `.ocio` config files as YAML via `src/OpenColorIO/OCIOYaml.cpp`
(confirmed: `grep -rl "yaml-cpp" src` returns `OCIOYaml.cpp`, `apphelpers/mergeconfigs/OCIOMYaml.*`,
and `CMakeLists.txt` — all part of the core library, not gated behind any `OCIO_BUILD_*` switch).

## Feature Minimization

The recipe (`dependencies/superbuild/projects/yaml-cpp.cmake`) builds only the static library:

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `YAML_BUILD_SHARED_LIBS` | `OFF` | Matches the static, feature-minimized prefix convention already used by every other recipe. |
| `YAML_CPP_BUILD_CONTRIB` | `OFF` | The legacy `GraphBuilderInterface` contrib sources (`src/contrib/`) are unused: `grep -rn "GraphBuilder" .` against the downloaded OpenColorIO 2.5.2 archive finds zero references. OCIO's `OCIOYaml.cpp`/`OCIOMYaml.cpp` use only `YAML::Node`/`YAML::Emitter`, never the contrib graph-building API. |
| `YAML_CPP_BUILD_TOOLS` | `OFF` | The `parse`/`sandbox` example CLI programs are not part of Bloom's linked surface. |
| `BUILD_TESTING` / `YAML_CPP_BUILD_TESTS` | `OFF` | `YAML_CPP_BUILD_TESTS` is `cmake_dependent_option`'d on `BUILD_TESTING AND YAML_CPP_MAIN_PROJECT`; both are set explicitly OFF so the test suite (and its bundled/system-gtest probe, `YAML_USE_SYSTEM_GTEST`) never builds regardless of how the superbuild's global `BUILD_TESTING` cache state resolves for this `ExternalProject_Add`-driven configure. |
| `YAML_CPP_INSTALL` | `ON` (upstream default here, since this `ExternalProject_Add` build is its own top-level `YAML_CPP_MAIN_PROJECT`) | Required so the CMake package config (`yaml-cpp-config.cmake`, target `yaml-cpp::yaml-cpp`) lands in the shared prefix for OpenColorIO's `find_package(yaml-cpp CONFIG)`. Set explicitly rather than relying on the conditional default. |
| `YAML_CPP_FORMAT_SOURCE` | `OFF` | Build-time `clang-format` invocation is a source-hygiene step for yaml-cpp's own development, not a Bloom capability; also avoids a `find_program(clang-format)` probe with no effect on the shipped artifact. |
| `YAML_ENABLE_PIC` | `ON` (upstream default) | Matches the superbuild-global `CMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON` convention; set explicitly for clarity. |

`YAML_CPP_DISABLE_UNINSTALL` and `YAML_MSVC_SHARED_RT` are left at their upstream defaults: the
former only affects whether an `uninstall` convenience target is generated (irrelevant to a
one-shot superbuild staging prefix) and the latter is Windows-only and inert on this Linux build.
