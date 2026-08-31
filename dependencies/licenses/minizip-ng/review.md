# minizip-ng 4.2.2 License And Feature Review

Reviewed: 2026-08-31

## License

- SPDX expression: `Zlib`. Permissive; compatible with Bloom's Apache-2.0 distribution and static
  linking under ADR 0014.
- Attribution: satisfied by shipping the bound `LICENSE` bytes with distributed builds.
  `copyrightFiles`/`noticeFiles` are empty by review: the copyright/license terms live inside the
  license text itself. `sourceObligation: none`; `modified: false` with an empty patch set.

## Why minizip-ng Is In The Closure

minizip-ng is one of OpenColorIO 2.5.2's six unconditionally required dependencies (see
`dependencies/licenses/opencolorio/review.md`'s "Mandatory Transitive Closure") — OCIO's core
library links it for `.ocioz` archive-config read/write support
(`src/OpenColorIO/OCIOZArchive.cpp`, compiled unconditionally, not gated behind any `OCIO_BUILD_*`
switch). `docs/architecture/color-management.md`'s "Durable OCIO Configuration Identity" table
names project-relative `.ocioz` and external `.ocioz` as accepted locator kinds, so this recipe is
load-bearing for Bloom's own OCIO configuration model, not merely an incidental OCIO build
requirement.

## Feature Minimization

OpenColorIO's own `share/cmake/modules/Findminizip-ng.cmake` states the requirement directly: "OCIO
only needs ZLIB" and warns that "Setting MZ_BZIP2=ON will cause linking issue since OCIO will not
be linked against BZIP2" — i.e. enabling an extra compression backend is not just unnecessary
surface, it actively breaks OCIO's own link step, since OCIO's `find_package(minizip-ng)` result
only pulls in `ZLIB::ZLIB`. The recipe
(`dependencies/superbuild/projects/minizip-ng.cmake`) sets the **exact** option set OpenColorIO's
own `share/cmake/modules/install/Installminizip-ng.cmake` passes when it builds this dependency
itself (confirmed by reading that file in the downloaded OpenColorIO 2.5.2 archive), extended with
`MZ_PPMD:BOOL=OFF` (a backend option that did not exist when OCIO's install script was last
written for an earlier minizip-ng release, but follows the identical "OCIO only needs ZLIB" logic):

| CMake option | Value | Contract justification |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` (superbuild-global) | Matches the static, feature-minimized prefix convention already used by every other recipe. |
| `MZ_COMPAT` | `OFF` | The legacy `unzip.h`/`zip.h` compatibility API is unused: `src/OpenColorIO/OCIOZArchive.cpp` includes only the modern `mz.h`/`mz_zip.h`/`mz_zip_rw.h` headers, never `mz_compat.h`. Matches OCIO's own install script. |
| `MZ_ZLIB` | `ON` | The one compression backend OCIO links against (`ZLIB::ZLIB`, reused from `bloom_dependency_zlib` — not re-intaken). |
| `MZ_BZIP2` | `OFF` | Not linked by OCIO; enabling it breaks OCIO's own link step per `Findminizip-ng.cmake`'s explicit warning. |
| `MZ_LZMA` | `OFF` | Same reasoning as `MZ_BZIP2`. |
| `MZ_PPMD` | `OFF` | Same reasoning as `MZ_BZIP2`; not present in the version OCIO's install script targeted, added here for the same "OCIO only needs ZLIB" logic. |
| `MZ_ZSTD` | `OFF` | Same reasoning as `MZ_BZIP2`. |
| `MZ_FETCH_LIBS` | `OFF` | See "Auto-Fetch" below: minizip-ng has its own, OCIO-independent dependency fetcher. |
| `MZ_FORCE_FETCH_LIBS` | `OFF` | See "Auto-Fetch" below. |
| `MZ_PKCRYPT` | `OFF` | PKWARE traditional (weak) encryption; not used by OCIO's `.ocioz` format and not a Bloom capability. |
| `MZ_WZAES` | `OFF` | WinZip AES encryption; same reasoning. |
| `MZ_OPENSSL` | `OFF` | Only relevant to `MZ_PKCRYPT`/`MZ_WZAES`; also avoids probing for a host OpenSSL Bloom does not lock. |
| `MZ_ICONV` | `OFF` | String-encoding conversion for legacy DOS/OEM zip filename codepages; OCIO's `.ocioz` entries are Bloom/OCIO-authored UTF-8 paths, not third-party archives needing codepage recovery. Matches OCIO's own install script. |
| `MZ_COMPRESS_ONLY` / `MZ_DECOMPRESS_ONLY` | `OFF` / `OFF` | OCIO's core library links both the `.ocioz` reader and writer unconditionally (`OCIOZArchive.cpp` implements both `ExtractOCIOZArchive` and `CreateOCIOZArchive`); restricting either direction would break OCIO's own link even though Bloom's v1 color-management contract only reads an external `.ocioz` today. |
| `MZ_FILE32_API` | `OFF` (upstream default) | 32-bit-only POSIX file API; Bloom targets 64-bit platforms exclusively. |
| `MZ_BUILD_TESTS` / `MZ_BUILD_UNIT_TESTS` / `MZ_BUILD_FUZZ_TESTS` | `OFF` / `OFF` / `OFF` (upstream defaults) | Test/fuzz executables are not part of the shipped surface; set explicitly for clarity. |
| `MZ_CODE_COVERAGE` | `OFF` (upstream default) | Coverage instrumentation is a minizip-ng development concern, not a Bloom build. |

## ZLIB Resolution

Two independent findings surfaced only once a mandatory, non-CONFIG `find_package(ZLIB)` probe
entered this superbuild for the first time (neither OpenEXR nor Imath links ZLIB directly, so this
was latent and unexercised before OpenColorIO's intake). Both are worked around locally in
`dependencies/superbuild/projects/minizip-ng.cmake` and `opencolorio.cmake` without modifying the
already-qualified `bloom_dependency_zlib` recipe, per the frozen "reuse, do not re-intake"
decision:

1. **Broken static-only `ZLIBConfig.cmake`.** `bloom_dependency_zlib`'s installed
   `lib/cmake/zlib/ZLIBConfig.cmake` unconditionally `include()`s both `ZLIB-shared.cmake` and
   `ZLIB-static.cmake` when no `COMPONENTS` are requested — but this static-only prefix
   (`ZLIB_BUILD_SHARED:BOOL=OFF` in `zlib.cmake`) only ever installs `ZLIB-static.cmake`. Any
   unqualified `find_package(ZLIB)` (no explicit `CONFIG`/`MODULE` keyword) that reaches Config
   mode — which the superbuild-global `CMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=ON` makes the first
   attempt — hits a real (non-fatal, but build-breaking) CMake Error:
   ```
   CMake Error at .../lib/cmake/zlib/ZLIBConfig.cmake:40 (include):
     include could not find requested file:
       .../lib/cmake/zlib/ZLIB-shared.cmake
   ```
   minizip-ng's own `CMakeLists.txt:199` (`find_package(ZLIB QUIET)`) and OpenColorIO's own
   `FindExtPackages.cmake` (`ocio_handle_dependency(ZLIB REQUIRED ALLOW_INSTALL ...)`, also
   unqualified) both trigger this. Both recipes set
   `CMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=OFF`, which routes their unqualified ZLIB probe through
   Module mode (CMake's own bundled `FindZLIB.cmake`) instead, avoiding the broken config
   entirely. This does not affect Imath/expat/yaml-cpp/minizip-ng resolution, since OpenColorIO
   resolves each of those through an explicit `CONFIG` keyword inside its own
   `share/cmake/modules/Find<name>.cmake` wrapper, independent of this global preference.
2. **Ambient host `zlib-ng` discovery.** This development machine has a system-installed
   `zlib-ng` package (`/usr/lib/cmake/zlib-ng/zlib-ng-config.cmake`, entirely unrelated to Bloom's
   qualified prefix). minizip-ng's default `MZ_ZLIB_FLAVOR=auto` probes
   `find_package(ZLIB-NG QUIET CONFIG)` before falling back to plain `ZLIB`; left unguarded, this
   silently links the host's shared `zlib-ng` instead of the qualified prefix's static zlib —
   exactly the "accidental host-library reference" class of defect the Qualification Matrix
   forbids. `minizip-ng.cmake` sets `CMAKE_DISABLE_FIND_PACKAGE_ZLIB-NG:BOOL=ON` (a standard CMake
   mechanism to force one named package's `find_package()` result to unconditionally report
   not-found) so only the intended `ZLIB` branch, pointed at the prefix via `ZLIB_ROOT`, can ever
   resolve. This is scoped to the exact package name minizip-ng probes; it does not suppress any
   other package's discovery.

**Recommendation for the supervisor**: finding 1 is a real defect in the already-qualified
`bloom_dependency_zlib` recipe's option set (it does not pass whatever option/COMPONENTS
combination the upstream zlib CMake packaging expects for a static-only, Config-mode-clean
install) and affects every future consumer that resolves ZLIB via an unqualified
`find_package(ZLIB)`, not just this batch. It is out of this task's ownership to fix (zlib is
explicitly reused, not re-intaken, in this task package), but is flagged here so it can be
addressed at the source rather than re-worked-around by each future consumer.

## MZ_LIBBSD Feature-Summary Note

minizip-ng's build-end feature summary reports `MZ_LIBBSD, Builds with libbsd crypto random` as
"enabled" — this is not a recipe option (there is no `-DMZ_LIBBSD` in this recipe's `CMAKE_ARGS`;
it is `cmake_dependent_option(MZ_LIBBSD ... ON "UNIX" OFF)`'s own upstream-computed default on any
Unix build). It is cosmetically enabled but never actually reached at configure time on this
target: the only code path that acts on it —
`if(MZ_LIBBSD AND NOT HAVE_ARC4RANDOM_BUF)` guarding a `pkg_check_modules(... libbsd-overlay)`
probe — is gated on `NOT HAVE_ARC4RANDOM_BUF`, and glibc on the qualified Linux target provides
`arc4random_buf` natively (`check_symbol_exists("arc4random_buf" "stdlib.h" HAVE_ARC4RANDOM_BUF)`
succeeds), so the guard is false and the `pkg_check_modules` probe never executes. Confirmed by
absence of any `pkg_check_modules`/libbsd-related log line in the configure output and by the
installed `lib/pkgconfig/minizip-ng.pc`, whose `Requires.private` names only `zlib` and whose
`Libs.private` is empty — no `libbsd` link dependency exists in the built artifact. No undeclared
external package enters the closure.

## Auto-Fetch

minizip-ng's `CMakeLists.txt` implements its own dependency-fetching mechanism
(`MZ_FETCH_LIBS`/`MZ_FORCE_FETCH_LIBS`, which can `clone_repo()` zlib-ng or other compression
backends from git) that is entirely independent of OpenColorIO's `OCIO_INSTALL_EXT_PACKAGES`
mechanism — OCIO never sees or controls it once `find_package(minizip-ng CONFIG)` succeeds against
the prefix. `MZ_FETCH_LIBS` defaults to `${WIN32}` (off on Linux, the platform this recipe
qualifies) but is set explicitly to `OFF` here regardless of host platform, alongside
`MZ_FORCE_FETCH_LIBS:BOOL=OFF`, so a missing backend hard-fails configuration instead of ever
attempting a network clone. `MZ_ZLIB`'s own `find_package(ZLIB)` call resolves
`bloom_dependency_zlib` from `CMAKE_PREFIX_PATH` instead.
