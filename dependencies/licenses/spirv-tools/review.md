# SPIRV-Tools License And Feature Review

Reviewed: 2026-09-19

## License

- SPDX expression: `Apache-2.0`. The root `LICENSE` is the Apache License 2.0; the bundled
  `utils/vscode/src/lsp/LICENSE` is the same Apache-2.0 text and is bound beside this file.
  `sourceObligation: none`; `modified: false` with an empty patch set.

## Reached Scope And Minimization

- Distribution review: the offline validator `spirv-val` ships as the runtime GPU shader-validation
  tool beside the desktop, CLI, and MCP executables under the private `bloom-gpu-tools/` directory,
  so the lock records `linkage: executable` with `shippingRoles: ["executable", "license"]`
  (schema 1.3's shipping form). The staged binary travels with the Apache-2.0 text and this review,
  plus a generated inventory binding the executable digest to `bin/spirv-val` in the qualified
  prefix. The build-only libraries and CMake package the upstream build also installs into the
  prefix remain build support, not shipped runtime files.
- Recipe options (`dependencies/superbuild/projects/spirv-tools.cmake`):

| CMake option | Value | Justification |
| --- | --- | --- |
| `SPIRV_SKIP_TESTS` | `ON` | No test tree; googletest, effcee, re2, and abseil are never reached or downloaded. |
| `SPIRV_SKIP_EXECUTABLES` | `OFF` | Builds the tools, including `spirv-val`. |
| `SPIRV_BUILD_FUZZER` | `OFF` | The fuzzers and protobuf are not built. |
| `SPIRV_WERROR` | `OFF` | Upstream warnings cannot fail the dependency build. |
| `SPIRV-Headers_SOURCE_DIR` | explicit sibling source dir | Satisfies `external/CMakeLists.txt` without system resolution or fetching. |

- SPIRV_Tools builds the optimizer (`spirv-opt`) as part of the tools, but it is **not linked into
  glslang**: glslang is configured independently with `ENABLE_OPT=OFF`.

## Deterministic Build Version

- The upstream `utils/update_build_version.py` uses `FORCED_BUILD_VERSION_DESCRIPTION` when that
  environment variable is set, and otherwise derives the description from `git describe` /
  `git rev-parse` of the source tree. This archive is extracted inside the Bloom checkout, so the
  fallback previously baked Bloom's parent git HEAD into `spirv-val --version`.
- Decision: the recipe
  (`dependencies/superbuild/projects/spirv-tools.cmake`) pins
  `FORCED_BUILD_VERSION_DESCRIPTION` to this component's exact source commit
  `b707790a898e44038547df54580022fc1cf89c3d` for the build and install generation steps, using a
  generator-agnostic `cmake -E env` wrapper around `cmake --build` / `cmake --install`. This is a
  reviewed build-environment decision only: no upstream source is patched and no runtime dependency
  pin changes. The lock records it as an enabled feature decision.
