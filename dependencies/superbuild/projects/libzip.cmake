# libzip 1.11.4 — the qualified ZIP reader/writer for the .bloom container, per
# docs/architecture/project-format.md (BSD-3-Clause; see dependencies/licenses/libzip/). Feature
# minimization per the constrained ZIP profile: stored+deflate only — every crypto backend,
# bzip2/lzma/zstd compression, the tools, the regression suite, and the docs stay out of the
# prefix. Links the prefix's own zlib, never a host zlib.
set(BLOOM_LIBZIP_VERSION 1.11.4)
set(BLOOM_LIBZIP_URL
    "https://github.com/nih-at/libzip/archive/refs/tags/v${BLOOM_LIBZIP_VERSION}.tar.gz")
set(BLOOM_LIBZIP_SHA256 4844595615e2436e3cf1ed46a1b260fbdaf8b8fa8f2b594e8b5d8162c696b8b2)

ExternalProject_Add(bloom_dependency_libzip
    URL "${BLOOM_LIBZIP_URL}"
    URL_HASH SHA256=${BLOOM_LIBZIP_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME libzip-${BLOOM_LIBZIP_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS bloom_dependency_zlib
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        # zlib 1.3.2's installed package config unconditionally includes its shared-library
        # export, which a static-only build does not install; module-mode discovery against
        # ZLIB_ROOT finds the static archive correctly, so config preference is disabled for
        # exactly this sub-build.
        -DCMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=OFF
        -DZLIB_ROOT:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DBUILD_TOOLS:BOOL=OFF
        -DBUILD_REGRESS:BOOL=OFF
        -DBUILD_OSSFUZZ:BOOL=OFF
        -DBUILD_EXAMPLES:BOOL=OFF
        -DBUILD_DOC:BOOL=OFF
        -DENABLE_COMMONCRYPTO:BOOL=OFF
        -DENABLE_GNUTLS:BOOL=OFF
        -DENABLE_MBEDTLS:BOOL=OFF
        -DENABLE_OPENSSL:BOOL=OFF
        -DENABLE_WINDOWS_CRYPTO:BOOL=OFF
        -DENABLE_BZIP2:BOOL=OFF
        -DENABLE_LZMA:BOOL=OFF
        -DENABLE_ZSTD:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
