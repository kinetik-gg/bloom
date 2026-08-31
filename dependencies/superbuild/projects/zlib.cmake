# zlib 1.3.2 — the qualified deflate and CRC implementation for the .bloom container, per
# docs/architecture/project-format.md (Zlib license; see dependencies/licenses/zlib/). Core
# library only: contrib code (minizip, dotzlib, ...) is neither compiled nor installed, and the
# shared library is disabled so exactly the static archive enters the prefix.
set(BLOOM_ZLIB_VERSION 1.3.2)
set(BLOOM_ZLIB_URL
    "https://github.com/madler/zlib/archive/refs/tags/v${BLOOM_ZLIB_VERSION}.tar.gz")
set(BLOOM_ZLIB_SHA256 b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1)

ExternalProject_Add(bloom_dependency_zlib
    URL "${BLOOM_ZLIB_URL}"
    URL_HASH SHA256=${BLOOM_ZLIB_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME zlib-${BLOOM_ZLIB_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DZLIB_BUILD_SHARED:BOOL=OFF
        -DZLIB_BUILD_STATIC:BOOL=ON
        -DZLIB_BUILD_TESTING:BOOL=OFF
        -DZLIB_BUILD_MINIZIP:BOOL=OFF
        -DZLIB_INSTALL:BOOL=ON
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

# Issue #93 fix: zlib 1.3.2's generated lib/cmake/zlib/ZLIBConfig.cmake unconditionally
# include()s both ZLIB-shared.cmake and ZLIB-static.cmake when an unqualified find_package(ZLIB)
# requests no explicit COMPONENTS -- but ZLIB_BUILD_SHARED is OFF above, so this static-only
# prefix never installs ZLIB-shared.cmake and Config-mode resolution hard-fails for any consumer
# that does not force Module mode (see dependencies/licenses/minizip-ng/review.md "ZLIB
# Resolution", which documents two recipes working around exactly this by forcing Module mode
# locally). This step replaces the generated file with a byte-for-byte equivalent whose includes
# are existence-guarded, recorded at
# dependencies/superbuild/projects/zlib-static-only-ZLIBConfig.cmake -- a documented post-install
# config adjustment, never an unrecorded hand-edit of installed output. It runs after zlib's own
# install step so it is never skipped or raced by that step.
ExternalProject_Add_Step(bloom_dependency_zlib bloom_fix_static_only_config
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_LIST_DIR}/zlib-static-only-ZLIBConfig.cmake"
        "<INSTALL_DIR>/lib/cmake/zlib/ZLIBConfig.cmake"
    DEPENDEES install
    COMMENT "bloom: applying issue #93 static-only ZLIBConfig.cmake fix")
