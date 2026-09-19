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
- No legal conclusion is asserted here. Whether the Bison Exception fully discharges GPL
  obligations for a distributed binary is a qualify-time legal-review question. This intake is
  **build-only**: the compiler binary is not part of the end-user application package, so the lock
  records `linkage: executable` with an **empty `shippingRoles`** array (schema 1.3's build-only
  form).

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

- The tool is invoked only at build/qualify time (for example to emit a fixed built-in OCIO display
  shader); no runtime shader compilation is added in this pass. All test dependencies remain
  unreached (`GLSLANG_TESTS=OFF` with no `External/googletest` in the archive).
