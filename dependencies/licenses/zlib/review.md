# zlib 1.3.2 License Review

Reviewed: 2026-08-27

- SPDX expression: `Zlib`. Permissive; compatible with Bloom's Apache-2.0 distribution and static
  linking under ADR 0014.
- Attribution: the license requires that altered sources be marked and the notice retained in
  source distributions; Bloom ships the exact license text and does not alter the source
  (`modified: false`, empty patch set). `copyrightFiles`/`noticeFiles` are empty by review: the
  copyright line lives inside the license text and the zlib license imposes no separate notice
  file. `sourceObligation: none`.
- Contrib licenses inside the archive (dotzlib, minizip) do not apply: the recipe builds only the
  core library; contrib code is neither compiled nor installed.

## Issue #93 Resolution: Static-Only Config-Mode Resolution

`dependencies/licenses/minizip-ng/review.md`'s "ZLIB Resolution" section first documented this
defect: zlib 1.3.2's generated `lib/cmake/zlib/ZLIBConfig.cmake` unconditionally `include()`s
both `ZLIB-shared.cmake` and `ZLIB-static.cmake` whenever an unqualified `find_package(ZLIB)`
(no explicit `COMPONENTS`) reaches Config mode. This recipe's `ZLIB_BUILD_SHARED:BOOL=OFF` means
`ZLIB-shared.cmake` is never installed, so Config-mode resolution failed hard for any consumer
that did not separately force Module mode -- a real defect in this already-qualified recipe's
installed package config, not a downstream consumer bug.

Fixed by `dependencies/superbuild/projects/zlib.cmake`'s `bloom_fix_static_only_config`
`ExternalProject_Add_Step`, which runs immediately after zlib's own install step and replaces the
generated `ZLIBConfig.cmake` with the byte-for-byte equivalent recorded at
`dependencies/superbuild/projects/zlib-static-only-ZLIBConfig.cmake`. That replacement differs
from upstream only in guarding each `include()` with an `EXISTS` check, so it degrades gracefully
for this static-only profile while remaining identical to upstream behavior if a future profile
ever ships both component export sets. This is a documented post-install config adjustment
performed by the recipe itself and checked into the repository -- never an unrecorded hand-edit of
build output.

Verified against a genuinely clean full superbuild rebuild (`rm -rf build/superbuild
build/dependency-prefix` then a fresh configure and `cmake --build --parallel 6`): an out-of-recipe
smoke project ran `find_package(ZLIB REQUIRED CONFIG PATHS <prefix> NO_DEFAULT_PATH)` with no
`COMPONENTS`, confirmed `ZLIB_FOUND` and the `ZLIB::ZLIBSTATIC` imported target, then compiled,
linked, and ran a trivial `zlibVersion()` consumer against the fixed prefix -- all succeeded.
`minizip-ng.cmake` and `opencolorio.cmake`'s existing Module-mode workarounds (see
`dependencies/licenses/minizip-ng/review.md`) remain in place unchanged; they are now
belt-and-suspenders rather than load-bearing for those two recipes.
