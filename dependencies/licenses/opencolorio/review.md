# OpenColorIO 2.5.2 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `BSD-3-Clause`. Permissive; compatible with Bloom's Apache-2.0 distribution
  and static linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright line lives inside the license
  text. `sourceObligation: none`; `modified: false` with an empty patch set.

## What Bloom Needs From OCIO (color-management.md)

`docs/architecture/color-management.md` fixes the CPU display path as the first production path
("Process And Display Separation"): a `ProcessFrame` in Bloom's fixed `lin_rec709_scene` process
interpretation is transformed by a "qualified OCIO display processor" into a `PreparedDisplayFrame`
for the Viewer and PNG output. The document is explicit about what that requires from the OCIO
build itself:

- "OCIO's processor and config cache IDs participate only in execution provenance..." and the
  "CPU Display Processor Boundary" section root the integration in OCIO's C++ **config/processor
  API only** — parsing `.ocio`/`.ocioz` configs (`ResolvedColorConfig`), building a CPU transform
  processor (`PreparedCpuDisplayProcessorHandle`), and applying it to `RGBA32F` pixel chunks
  (`PreparedDisplayFrame`). No GPU shader path, command-line tool, or scripting-language binding
  is named anywhere in this document.
- "Supervised OCIO Execution" describes a `bloom-color-worker` **helper process** that performs
  "all OCIO parsing, config construction, processor construction, and transform application" for
  untrusted (non-built-in) configs — this is Bloom-owned process supervision around OCIO's C++
  API, not an OCIO-provided app or GPU/OpenFX/Nuke integration.
- The accepted locator kinds ("Durable OCIO Configuration Identity") are a Bloom built-in URI, a
  project-relative `.ocioz`, an external `.ocioz`, and an external loose `config.ocio` — all
  resolved through OCIO's config/archive parsing, matching exactly the mandatory `minizip-ng`
  dependency this recipe consumes for `.ocioz` support (see "Mandatory Transitive Closure" below).
- Nothing in this document names OCIO's `ociobakelut`/`ociodisplay`/`ocioconvert`/`ociocheck` CLI
  tools, its Python bindings, its OpenFX or Nuke plugins, or its GPU shader/OpenGL test harness.
  These are the exact capabilities `docs/architecture/dependency-intake.md`'s Feature Minimization
  section names for OCIO: "OpenColorIO tools, Python bindings, tests, and optional integrations
  remain off unless a Bloom capability separately requires them" — none currently do.

## Build-Surface Feature Minimization

The recipe (`dependencies/superbuild/projects/opencolorio.cmake`) disables every optional
integration OCIO 2.5.2's top-level `CMakeLists.txt` exposes (`grep -n "^option(" CMakeLists.txt`
against the downloaded archive lists exactly the options in this table plus the
`OCIO_USE_SIMD`/`OCIO_USE_SSE*`/`OCIO_USE_AVX*`/`OCIO_USE_F16C` CPU-dispatch group, which are left
at their upstream defaults — they select CPU instruction-set codepaths inside the CPU display
path color-management.md requires, not an external capability or dependency, and introduce no new
attack surface or license obligation):

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` (superbuild-global) | Matches the static, feature-minimized prefix convention already used by every other recipe. |
| `OCIO_INSTALL_EXT_PACKAGES` | `NONE` | Hard-disables OCIO's own dependency auto-fetch/auto-install mechanism; see "Auto-Fetch Disabled" below. |
| `OCIO_BUILD_APPS` | `OFF` | Named explicitly by dependency-intake.md's Feature Minimization section. Disables `ociobakelut`/`ociodisplay`/`ocioconvert`/`ociocheck`/`ociomakeclf`/`ocioarchive`/`ociolutimage`; none are part of Bloom's linked surface (see "What Bloom Needs From OCIO" above). Also gates off the optional lcms2/OpenImageIO/OpenEXR-for-apps probes in FindExtPackages.cmake. |
| `OCIO_BUILD_OPENFX` | `OFF` | Matches upstream default. OpenFX plugin hosting is an Autodesk/host-integration surface color-management.md never names; also gates off the optional `openfx` dependency probe. |
| `OCIO_BUILD_NUKE` | `OFF` | Matches upstream default. Nuke plugin support is unrelated to Bloom's C++ CPU display path. |
| `OCIO_BUILD_TESTS` | `OFF` | Named explicitly by dependency-intake.md's Feature Minimization section. Keeps OCIO's own unit-test suite (and its conditional OSL/OpenImageIO/Imath-for-tests probe) out of the build. |
| `OCIO_BUILD_GPU_TESTS` | `OFF` | Named explicitly by dependency-intake.md's Feature Minimization section. color-management.md's v1 path is the CPU processor ("the CPU path is the correctness oracle for the separately gated OCIO GPU processor" — Alpha And Pixel Flow); the GPU test harness is out of scope until that separately gated capability exists. |
| `OCIO_BUILD_DOCS` | `OFF` | Named explicitly by dependency-intake.md's Feature Minimization section. Keeps the Sphinx/readthedocs documentation source out of the build. |
| `OCIO_BUILD_PYTHON` | `OFF` | Named explicitly by dependency-intake.md's Feature Minimization section. Bloom's OCIO consumption is exclusively through `src/color`'s Qt-free C++ adapter (color-management.md, "CPU Display Processor Boundary"); also gates off the pybind11 and Python-development probes in FindExtPackages.cmake. |
| `OCIO_BUILD_JAVA` | `OFF` | Matches upstream default. Java bindings are unrelated to Bloom's C++ consumption. |
| `OCIO_USE_OIIO_FOR_APPS` | `OFF` | Matches upstream default; a no-op once `OCIO_BUILD_APPS` is `OFF` (the option only affects which image backend OCIO's own apps link against). Set explicitly so the option set documents intent even though it has no effect on this build. |

`src/CMakeLists.txt` unconditionally `add_subdirectory(apputils)` ("Various app. helpers used by
any upper layers.") regardless of `OCIO_BUILD_APPS` — it builds `libapputils.a` inside the build
tree but this recipe's build never installs it (confirmed absent from the prefix's `lib/`), so it
is an internal build-time artifact, not a shipped surface.

The resulting installed static library is `libOpenColorIO.a` (namespace `OpenColorIO_v2_5`, per
OCIO's default `OCIO_NAMESPACE`) plus its CMake package config (`OpenColorIOConfig.cmake`,
target `OpenColorIO::OpenColorIO`) and the private Find-module/macro files OCIO installs for
static consumers (`share/OpenColorIO/cmake/{modules,macros}/...`, needed because a static OCIO
consumer must itself link `expat`/`yaml-cpp`/`Imath`/`pystring`/`minizip-ng`/`ZLIB` — the six
mandatory dependencies below); no `bin/` tools, Python module, OpenFX/Nuke plugin, or
`share/doc`/`share/man` entries are installed by this component.

## Auto-Fetch Disabled

OCIO 2.5.2's top-level `CMakeLists.txt` (lines 353–375 of the downloaded archive) defines
`OCIO_INSTALL_EXT_PACKAGES` as a closed three-value option — `NONE`, `MISSING` (upstream default:
install only what `find_package` cannot locate), `ALL` (force-install every dependency, skipping
`find_package` entirely) — and fails configuration on any other value
(`message(FATAL_ERROR "OCIO_INSTALL_EXT_PACKAGES=... is unsupported.")`). Every dependency probe in
`share/cmake/modules/FindExtPackages.cmake` calls the `ocio_handle_dependency(<name> ... 
ALLOW_INSTALL ...)` macro (`share/cmake/macros/ocio_handle_dependency.cmake`), which only ever
calls `ocio_install_dependency()` (`share/cmake/macros/ocio_install_dependency.cmake`) — and that
macro's *entire* install path is gated by one condition:

```cmake
if(NOT ${dep_name}_FOUND AND OCIO_INSTALL_EXT_PACKAGES AND NOT OCIO_INSTALL_EXT_PACKAGES STREQUAL NONE)
    ...
    include(Install${dep_name})
```

With `OCIO_INSTALL_EXT_PACKAGES:STRING=NONE`, this condition is always false, so `Install<dep>.cmake`
(the file that would run `ExternalProject_Add`/`FetchContent` against a live git remote) is never
included for any dependency, found or not. A `REQUIRED` dependency `find_package` cannot resolve
from `CMAKE_PREFIX_PATH` therefore reaches `ocio_handle_dependency`'s final check —
`message(SEND_ERROR "${dep_name} is required, will abort at the end.")` — and configuration fails
without ever attempting a network fetch. The configure log for this recipe shows every mandatory
dependency resolved, never installed (see "Mandatory Transitive Closure" below for the exact log
lines); no `Installed <dep>` message appears anywhere in the log, confirming `Install<dep>.cmake`
was never reached.

## Mandatory Transitive Closure

Determined from the downloaded 2.5.2 source tree itself
(`share/cmake/modules/FindExtPackages.cmake`), not from documentation or memory. The file's
"Required dependencies" section (lines 47–117) calls `ocio_handle_dependency(<name> REQUIRED
ALLOW_INSTALL ...)` unconditionally, before any `if(OCIO_BUILD_*)` guard, for exactly six
packages: `expat`, `yaml-cpp`, `pystring`, `Imath`, `ZLIB`, `minizip-ng`. Every other dependency in
that file — `lcms2` (`if(OCIO_BUILD_APPS)`), `openfx` (`if(OCIO_BUILD_OPENFX)`), `Python`/
`pybind11` (`if(OCIO_BUILD_PYTHON OR OCIO_BUILD_DOCS)`), `OpenImageIO`/`OpenEXR`
(`if((OCIO_BUILD_APPS AND OCIO_USE_OIIO_FOR_APPS) OR OCIO_BUILD_TESTS)`), and `OSL`
(`if(OCIO_BUILD_TESTS)` and only when both OpenImageIO and Imath targets already exist) — is
gated behind a build switch this recipe sets to `OFF`, so none of those optional probes ever runs.

The root `CMakeLists.txt` states the same conclusion in prose (lines 437–444, printed only for
static builds): "The following mandatory dependencies MUST be linked to the consumer application
or shared library that uses static OpenColorIO: expat, yaml-cpp, Imath, pystring, minizip-ng and
ZLIB" — matching the six-package closure above exactly.

Disposition per candidate:

- **Imath** — already a qualified prefix component (`bloom_dependency_imath`); reused via
  `find_package(Imath 3.1 CONFIG)`, not re-intaken.
- **ZLIB** (zlib) — already a qualified prefix component (`bloom_dependency_zlib`); reused via
  `find_package(ZLIB)`, not re-intaken.
- **expat** — required; new recipe `bloom_dependency_expat`
  (`dependencies/superbuild/projects/expat.cmake`, `dependencies/licenses/expat/`).
- **yaml-cpp** — required; new recipe `bloom_dependency_yaml-cpp`
  (`dependencies/superbuild/projects/yaml-cpp.cmake`, `dependencies/licenses/yaml-cpp/`).
- **pystring** — required; new recipe `bloom_dependency_pystring`
  (`dependencies/superbuild/projects/pystring.cmake`, `dependencies/licenses/pystring/`).
- **minizip-ng** — required; new recipe `bloom_dependency_minizip-ng`
  (`dependencies/superbuild/projects/minizip-ng.cmake`, `dependencies/licenses/minizip-ng/`).
- **lcms2, openfx, Python, pybind11, OpenImageIO, OpenEXR (as an OCIO app backend), OSL** — NOT
  required by this minimized build; every gating `OCIO_BUILD_*` switch above is `OFF`, so none of
  these probes executes. No lock entry, recipe, or vendored-component record is created for any of
  them. (OpenEXR is already a separately qualified Bloom component for
  `docs/architecture/frame-output.md`'s unrelated flat-EXR write path, but OCIO does not consume
  it in this build — see `OCIO_USE_OIIO_FOR_APPS`/`OCIO_BUILD_APPS` above.)

Evidence that the closure holds at actual configure time (config-log excerpt showing each
mandatory external resolved from the shared prefix, and confirming no optional probe fired) is
recorded in the superbuild verification output; see the task report for the exact log lines.

## Vendored/No-Switch Surface

Unlike OpenEXR's OpenJPH, OCIO 2.5.2's `ext/` and `vendor/` trees (`add_subdirectory(vendor)` /
`add_subdirectory(ext)` in the root `CMakeLists.txt`) contain no unconditionally-compiled
external library that lacks a corresponding `find_package`-based off-switch reachable by this
recipe's option set: `vendor/` holds OCIO's own utility code (not a third-party library), and
`ext/sse2neon` is guarded by `if(NOT (x86 or x86_64))` (non-x86 SIMD shim, irrelevant on Bloom's
x86-64 CI target and never installed regardless). No openjph-style "forced vendored, no lock
record" component applies to OpenColorIO itself.
