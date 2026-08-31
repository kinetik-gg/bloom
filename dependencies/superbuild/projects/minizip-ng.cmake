# minizip-ng 4.2.2 — the ZIP archive reader/writer OpenColorIO's core library requires
# unconditionally for .ocioz archive-config support (src/OpenColorIO/OCIOZArchive.cpp is compiled
# into libOpenColorIO itself, not gated behind OCIO_BUILD_APPS); see dependencies/licenses/minizip-ng/
# and OpenColorIO's FindExtPackages.cmake, whose "Required dependencies" section lists minizip-ng
# as REQUIRED ALLOW_INSTALL with no build-time opt-out.
#
# Feature minimization: OpenColorIO's own Findminizip-ng.cmake documents "OCIO only needs ZLIB"
# and warns that enabling another compression backend "will provoke linking issue since OCIO is
# not linking to those libraries" — so MZ_ZLIB is the only compression backend on, matching the
# exact option set OCIO's own Installminizip-ng.cmake passes when it builds this dependency
# itself (share/cmake/modules/install/Installminizip-ng.cmake in the OpenColorIO 2.5.2 archive).
# MZ_COMPAT stays OFF: OCIOZArchive.cpp includes mz.h/mz_zip.h/mz_zip_rw.h (the modern API), never
# mz_compat.h. Both compression and decompression stay enabled (no *_ONLY restriction): OCIO's
# core library links both the .ocioz reader and writer unconditionally, even though Bloom's own
# v1 color-management contract only ever reads an external .ocioz (see
# docs/architecture/color-management.md) — restricting either direction would break OCIO's own
# link, not just an unused Bloom capability. Zlib license (see dependencies/licenses/minizip-ng/).
#
# Auto-fetch: minizip-ng has its own FetchContent-based dependency fetcher independent of OCIO's
# OCIO_INSTALL_EXT_PACKAGES mechanism (MZ_FETCH_LIBS/MZ_FORCE_FETCH_LIBS, which can git-clone
# zlib-ng or other backends). Both are forced OFF here so a missing backend fails configuration
# instead of cloning one; MZ_ZLIB's own find_package(ZLIB) resolves bloom_dependency_zlib from
# CMAKE_PREFIX_PATH instead.
#
# DEPENDS on bloom_dependency_zlib (find_package(ZLIB), same CMAKE_PREFIX_PATH mechanism as every
# other recipe). Installs its CMake package config (minizip-ng-config.cmake, target
# MINIZIP::minizip-ng) into the shared prefix so bloom_dependency_opencolorio's
# find_package(minizip-ng CONFIG) resolves it without touching the host.
#
# ZLIB resolution workaround (two independent findings, neither touching the already-qualified
# bloom_dependency_zlib recipe, per the frozen "reuse, do not re-intake" decision):
#  1. minizip-ng's own CMakeLists.txt (line ~199) calls the unqualified `find_package(ZLIB QUIET)`
#     (no CONFIG keyword). With the superbuild-global CMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=ON,
#     CMake tries Config mode first against the prefix's own lib/cmake/zlib/ZLIBConfig.cmake — but
#     that installed config unconditionally `include()`s both ZLIB-shared.cmake and
#     ZLIB-static.cmake with no COMPONENTS given, and only the static variant exists in this
#     static-only prefix (bloom_dependency_zlib never builds ZLIB_BUILD_SHARED). The missing
#     ZLIB-shared.cmake include is a real (non-fatal but build-breaking) CMake Error. Setting
#     CMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=OFF here routes the unqualified ZLIB probe through
#     Module mode (CMake's own bundled FindZLIB.cmake) instead, sidestepping the broken config
#     entirely; every other dependency this recipe and opencolorio.cmake resolve (Imath, expat,
#     yaml-cpp, minizip-ng itself, pystring) is found through an explicit `CONFIG` keyword inside
#     OpenColorIO's own Find<name>.cmake wrapper modules and is unaffected by this override.
#  2. This build machine has a host-system `zlib-ng` package (`/usr/lib/cmake/zlib-ng/`,
#     independent of any Bloom-owned search path). minizip-ng's `MZ_ZLIB_FLAVOR=auto` default
#     probes `find_package(ZLIB-NG QUIET CONFIG)` before plain ZLIB; left alone, that would resolve
#     the ambient host package instead of the qualified prefix — exactly the "accidental
#     host-library reference" class of defect the Qualification Matrix forbids.
#     CMAKE_DISABLE_FIND_PACKAGE_ZLIB-NG:BOOL=ON forces that probe to report not-found
#     unconditionally, so only the ZLIB (not ZLIB-NG) branch — pointed at the prefix via
#     ZLIB_ROOT — can resolve.
# See dependencies/licenses/minizip-ng/review.md's "ZLIB Resolution" section for the full
# evidence (config-log lines) and a recommendation that bloom_dependency_zlib's own CMake option
# set be revisited so future consumers do not need this same recipe-local workaround.
set(BLOOM_MINIZIP_NG_VERSION 4.2.2)
set(BLOOM_MINIZIP_NG_URL
    "https://github.com/zlib-ng/minizip-ng/archive/refs/tags/${BLOOM_MINIZIP_NG_VERSION}.tar.gz")
set(BLOOM_MINIZIP_NG_SHA256 71af7b9799856d8b03619df3949e9c1be9703f8de0795af71399ba283cb27aac)

ExternalProject_Add(bloom_dependency_minizip-ng
    URL "${BLOOM_MINIZIP_NG_URL}"
    URL_HASH SHA256=${BLOOM_MINIZIP_NG_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME minizip-ng-${BLOOM_MINIZIP_NG_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS bloom_dependency_zlib
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_PREFIX_PATH:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DCMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB-NG:BOOL=ON
        -DZLIB_ROOT:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DMZ_COMPAT:BOOL=OFF
        -DMZ_ZLIB:BOOL=ON
        -DMZ_BZIP2:BOOL=OFF
        -DMZ_LZMA:BOOL=OFF
        -DMZ_PPMD:BOOL=OFF
        -DMZ_ZSTD:BOOL=OFF
        -DMZ_FETCH_LIBS:BOOL=OFF
        -DMZ_FORCE_FETCH_LIBS:BOOL=OFF
        -DMZ_PKCRYPT:BOOL=OFF
        -DMZ_WZAES:BOOL=OFF
        -DMZ_OPENSSL:BOOL=OFF
        -DMZ_ICONV:BOOL=OFF
        -DMZ_COMPRESS_ONLY:BOOL=OFF
        -DMZ_DECOMPRESS_ONLY:BOOL=OFF
        -DMZ_FILE32_API:BOOL=OFF
        -DMZ_BUILD_TESTS:BOOL=OFF
        -DMZ_BUILD_UNIT_TESTS:BOOL=OFF
        -DMZ_BUILD_FUZZ_TESTS:BOOL=OFF
        -DMZ_CODE_COVERAGE:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
