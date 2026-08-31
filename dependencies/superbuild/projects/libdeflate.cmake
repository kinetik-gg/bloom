# libdeflate 1.26 — OpenEXR 3.4's DEFLATE backend for its ZIP-family compression modes. OpenEXR
# dropped zlib for this role in the 3.2 line (see dependencies/licenses/libdeflate/review.md for
# the zlib-vs-libdeflate determination); the frame-output contract's flat OpenEXR preset requires
# exactly ZIP_COMPRESSION, so this backend is load-bearing for the one compression mode Bloom
# writes. MIT license (see dependencies/licenses/libdeflate/). Static, library-only build: the
# libdeflate-gzip CLI and its test programs are not built. Only the zlib-wrapper compression and
# decompression paths stay enabled — that is the exact surface OpenEXRCore's compression.c calls
# (libdeflate_zlib_compress / libdeflate_zlib_decompress_ex); raw gzip-format support is unused by
# OpenEXR and stays off. Installs its CMake package config into the shared prefix so
# bloom_dependency_openexr's find_package(libdeflate CONFIG) resolves it without touching the
# host.
set(BLOOM_LIBDEFLATE_VERSION 1.26)
set(BLOOM_LIBDEFLATE_URL
    "https://github.com/ebiggers/libdeflate/archive/refs/tags/v${BLOOM_LIBDEFLATE_VERSION}.tar.gz")
set(BLOOM_LIBDEFLATE_SHA256 bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85)

ExternalProject_Add(bloom_dependency_libdeflate
    URL "${BLOOM_LIBDEFLATE_URL}"
    URL_HASH SHA256=${BLOOM_LIBDEFLATE_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME libdeflate-${BLOOM_LIBDEFLATE_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DLIBDEFLATE_BUILD_STATIC_LIB:BOOL=ON
        -DLIBDEFLATE_BUILD_SHARED_LIB:BOOL=OFF
        -DLIBDEFLATE_COMPRESSION_SUPPORT:BOOL=ON
        -DLIBDEFLATE_DECOMPRESSION_SUPPORT:BOOL=ON
        -DLIBDEFLATE_ZLIB_SUPPORT:BOOL=ON
        -DLIBDEFLATE_GZIP_SUPPORT:BOOL=OFF
        -DLIBDEFLATE_BUILD_GZIP:BOOL=OFF
        -DLIBDEFLATE_BUILD_TESTS:BOOL=OFF
        -DLIBDEFLATE_INSTALL:BOOL=ON
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
