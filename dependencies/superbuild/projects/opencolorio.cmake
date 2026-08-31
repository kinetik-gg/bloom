# OpenColorIO 2.5.2 — the color configuration/transform library docs/architecture/color-management.md
# builds Bloom's CPU display path on (BSD-3-Clause; see dependencies/licenses/opencolorio/). Current
# 2.5 security/bug-fix release and CY2026 family per docs/architecture/dependency-intake.md's Initial
# Qualification Candidates; 2.5.2 is also the exact release that fixes GHSA-66xr-9rgw-v6m8,
# GHSA-rxp3-rrgx-f547, GHSA-28jr-x9w2-5pc4, and GHSA-fgx7-35rr-5mx2 (stack buffer overflows in the
# Spi1D/Spi3D/IridasCube/Discreet-1DL LUT parsers) — see dependencies/licenses/opencolorio/security.md.
#
# Static, library-only build: OCIO_BUILD_APPS, OCIO_BUILD_TESTS, OCIO_BUILD_GPU_TESTS,
# OCIO_BUILD_PYTHON, and OCIO_BUILD_DOCS are the five off-switches
# docs/architecture/dependency-intake.md's Feature Minimization section names explicitly; every
# other optional integration OCIO 2.5.2's top-level CMakeLists.txt exposes is also off
# (OCIO_BUILD_OPENFX, OCIO_BUILD_NUKE, OCIO_BUILD_JAVA, OCIO_USE_OIIO_FOR_APPS — the last is a
# no-op once OCIO_BUILD_APPS is OFF, set explicitly for clarity). See
# dependencies/licenses/opencolorio/review.md for the full per-option contract justification.
#
# Auto-fetch (OCIO_INSTALL_EXT_PACKAGES): OCIO 2.5.2's CMakeLists.txt defines exactly three values
# for this option — NONE, MISSING (the upstream default: install only what find_package can't
# locate), and ALL (force-install everything, skipping find_package entirely). Every OCIO Find
# module's ocio_handle_dependency(... ALLOW_INSTALL ...) call routes through
# share/cmake/macros/ocio_install_dependency.cmake, whose install guard is exactly
# `NOT OCIO_INSTALL_EXT_PACKAGES STREQUAL NONE` — so OCIO_INSTALL_EXT_PACKAGES:STRING=NONE is a
# hard "never install, never fetch" switch: a REQUIRED dependency that find_package cannot resolve
# from CMAKE_PREFIX_PATH fails configuration with `message(SEND_ERROR ...)` instead of downloading
# anything. Confirmed in the build log: every mandatory dependency line reads "Found <dep>
# (version ...)" resolved from ${BLOOM_DEPENDENCY_PREFIX}, never "Installed <dep>".
#
# Mandatory transitive closure (determined from the 2.5.2 source itself, not memory or docs):
# share/cmake/modules/FindExtPackages.cmake's "Required dependencies" section unconditionally
# calls ocio_handle_dependency(... REQUIRED ALLOW_INSTALL ...) for exactly six packages — expat,
# yaml-cpp, pystring, Imath, ZLIB, minizip-ng — regardless of which OCIO_BUILD_* switch is set;
# every other dependency in that file (lcms2, openfx, Python, pybind11, OpenImageIO/OpenEXR, OSL)
# is gated behind OCIO_BUILD_APPS/OCIO_BUILD_OPENFX/OCIO_BUILD_PYTHON/OCIO_BUILD_DOCS/
# OCIO_BUILD_TESTS and therefore never probed with every one of those off. Imath and ZLIB are
# already qualified prefix components (bloom_dependency_imath, bloom_dependency_zlib) and are
# reused, not re-intaken; expat, yaml-cpp, pystring, and minizip-ng become their own recipes with
# full intake rigor (bloom_dependency_expat, bloom_dependency_yaml-cpp, bloom_dependency_pystring,
# bloom_dependency_minizip-ng).
#
# DEPENDS on all six: find_package(Imath 3.1 CONFIG)/find_package(ZLIB) reuse the prefix components
# already qualified for OpenEXR; find_package(expat CONFIG), find_package(yaml-cpp CONFIG), the
# find_path/find_library pair in Findpystring.cmake, and find_package(minizip-ng CONFIG) resolve
# the four new recipes below, all through the same CMAKE_PREFIX_PATH mechanism as every other
# recipe in this superbuild.
#
# ZLIB resolution workaround: OCIO's own unqualified `find_package(ZLIB ...)` call in
# FindExtPackages.cmake hits the exact same broken bloom_dependency_zlib ZLIBConfig.cmake
# (unconditionally includes a ZLIB-shared.cmake this static-only prefix never installs) documented
# in dependencies/superbuild/projects/minizip-ng.cmake and
# dependencies/licenses/minizip-ng/review.md's "ZLIB Resolution" section.
# CMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=OFF routes it through Module mode (CMake's bundled
# FindZLIB.cmake, hinted at the prefix via ZLIB_ROOT) instead, sidestepping the broken config; the
# other five mandatory dependencies are unaffected (Imath/expat/yaml-cpp/minizip-ng resolve
# through an explicit `CONFIG` keyword inside their own OCIO-provided Find<name>.cmake wrapper,
# and pystring never calls find_package at all). This build has no ambient host `zlib-ng` probe of
# its own (that risk is specific to minizip-ng's own CMakeLists), so no
# CMAKE_DISABLE_FIND_PACKAGE_ZLIB-NG override is needed here.
set(BLOOM_OPENCOLORIO_VERSION 2.5.2)
set(BLOOM_OPENCOLORIO_URL
    "https://github.com/AcademySoftwareFoundation/OpenColorIO/archive/refs/tags/v${BLOOM_OPENCOLORIO_VERSION}.tar.gz")
set(BLOOM_OPENCOLORIO_SHA256 722601e01b78b7a12da4829cb450674935f404b0e508f3f20046fa77570e3272)

ExternalProject_Add(bloom_dependency_opencolorio
    URL "${BLOOM_OPENCOLORIO_URL}"
    URL_HASH SHA256=${BLOOM_OPENCOLORIO_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME opencolorio-${BLOOM_OPENCOLORIO_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS
        bloom_dependency_imath
        bloom_dependency_zlib
        bloom_dependency_expat
        bloom_dependency_yaml-cpp
        bloom_dependency_pystring
        bloom_dependency_minizip-ng
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_PREFIX_PATH:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DCMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=OFF
        -DZLIB_ROOT:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DOCIO_INSTALL_EXT_PACKAGES:STRING=NONE
        -DOCIO_BUILD_APPS:BOOL=OFF
        -DOCIO_BUILD_OPENFX:BOOL=OFF
        -DOCIO_BUILD_NUKE:BOOL=OFF
        -DOCIO_BUILD_TESTS:BOOL=OFF
        -DOCIO_BUILD_GPU_TESTS:BOOL=OFF
        -DOCIO_BUILD_DOCS:BOOL=OFF
        -DOCIO_BUILD_PYTHON:BOOL=OFF
        -DOCIO_BUILD_JAVA:BOOL=OFF
        -DOCIO_USE_OIIO_FOR_APPS:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
