# pystring 1.2.0 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `BSD-3-Clause`. Permissive; compatible with Bloom's Apache-2.0 distribution and
  static linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright line lives inside the license
  text. `sourceObligation: none`; `modified: false` with an empty patch set.

## Why pystring Is In The Closure

pystring is one of OpenColorIO 2.5.2's six unconditionally required dependencies (see
`dependencies/licenses/opencolorio/review.md`'s "Mandatory Transitive Closure") — OCIO's core
library uses pystring's Python-string-semantics helpers (`split`, `join`, `strip`, path
manipulation, etc.) throughout config parsing and file-path handling; there is no build switch
that removes this dependency.

## Feature Minimization

pystring's `CMakeLists.txt` (version 1.2.0) is a small, single-purpose build with almost no
options: it defines only the standard `option(BUILD_SHARED_LIBS ...)`, which the superbuild's
global `-DBUILD_SHARED_LIBS:BOOL=OFF` already resolves to `OFF`, matching the static,
feature-minimized prefix convention. The recipe
(`dependencies/superbuild/projects/pystring.cmake`) does not repeat that as a component-local
override; it relies on the inherited global default like `BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS`
already provides for every recipe.

pystring's `CMakeLists.txt` exposes no `BUILD_TESTING`/`PYSTRING_BUILD_TESTS`-style switch: the
`pystring_test`, `pystring_test_header_only`, and `pystring_test_header_only_define` executables
(`add_executable(...)` calls with no `if()` guard) always build. None of them is installed
(`install(TARGETS ...)` in this `CMakeLists.txt` names only the `pystring` and
`pystring_header_only` library targets), so they enter no shipped artifact or CMake package
export — they are unused build-time byproducts, not a shipped capability. Reducing this further
would require patching upstream to add a test-disable switch, which is out of this task's scope
(no patches are applied to any of this batch's components; see the lock handoff). Both the
compiled `pystring` static-library target (what OpenColorIO's `Findpystring.cmake` locates) and
the `pystring_header_only` interface target install; OpenColorIO consumes only the former (see
`opencolorio.cmake`'s comment on `Findpystring.cmake`'s `find_path`/`find_library` discovery,
which is not CMake-package-config-based).
