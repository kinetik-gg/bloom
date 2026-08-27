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
