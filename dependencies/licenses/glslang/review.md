# glslang License And Feature Review

Reviewed: 2026-09-19

## License

- SPDX expression: `BSD-3-Clause AND BSD-2-Clause AND MIT AND Apache-2.0 AND
  GPL-3.0-or-later WITH Bison-exception-2.2`. Additional upstream license texts the archive bundles
  (`AML-glslang`, `MIT-Khronos-old`) are preserved under `LICENSES/`; the exact per-file mapping is
  the upstream `license-checker.cfg` and `REUSE.toml`, both bound beside this file.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## GPL-3.0-or-later And The Bison Exception

- Upstream `license-checker.cfg` scopes `GPL-3.0-or-later` / `GPL-Header` to **only**
  `glslang/MachineIndependent/glslang_tab.cpp` and `glslang_tab.cpp.h`, which are Bison-generated;
  those two paths are excluded from the general permissive set. The Bison Exception 2.2 text is
  preserved exactly at `LICENSES/Bison-exception-2.2.txt` and inside `LICENSE.txt` (which also
  carries the NVIDIA and Khronos permissive notices).
- Distribution review: the `glslangValidator` binary and its build-time library set are staged as
  the runtime GPU colour-compilation tool beside the desktop, CLI, and MCP executables under the
  private `bloom-gpu-tools/` directory. The lock therefore records `linkage: executable` with
  `shippingRoles: ["executable", "license"]` (schema 1.3's shipping form). Every staged binary
  travels with the complete `LICENSES/` set and this review, plus a generated inventory binding the
  executable digest to `bin/glslangValidator` in the qualified prefix.
- The Bison Exception 2.2 is the basis on which the compiled `glslang_tab.cpp`/`.h` content is
  distributed alongside the permissive set; the exact per-file mapping remains the upstream
  `license-checker.cfg` and `REUSE.toml` bound beside this file. No legal conclusion beyond that
  recorded mapping is asserted here; an independent counsel review of the exception remains a
  qualify gate for a signed release, not a blocker for staging the reviewed binary.

## Reached Scope And Minimization

- Recipe options (`dependencies/superbuild/projects/glslang.cmake`):

| CMake option | Value | Justification |
| --- | --- | --- |
| `BUILD_EXTERNAL` | `OFF` | No `External/` submodule fetch; nothing is downloaded during configure. |
| `ALLOW_EXTERNAL_SPIRV_TOOLS` | `OFF` | No SPIRV-Tools linkage from glslang. |
| `ENABLE_OPT` | `OFF` | The optimizer is not required; glslang uses its vendored `SPIRV/spirv.hpp11`. |
| `GLSLANG_TESTS` | `OFF` | No test tree; googletest is never reached. |
| `ENABLE_GLSLANG_BINARIES` | `ON` | Builds the offline `glslang`/`glslangValidator` tool. |
| `ENABLE_HLSL` | `OFF` | The unused HLSL front end is left out. |

- The tool is the runtime backend for GPU colour compilation (for example generated OCIO display
  shaders) and is also used at build/qualify time to regenerate the fixed built-in shaders. This
  packaging slice stages the binary, its licenses, and its inventory; it adds no process or compiler
  implementation, and the process invocation and its resource limits remain a separate runtime
  contract. All test dependencies remain unreached (`GLSLANG_TESTS=OFF` with no
  `External/googletest` in the archive).
