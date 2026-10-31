# dav1d 1.5.4 — Bloom's bundled AV1 decoder (BSD-2-Clause), built from VideoLAN's pinned, signed
# release on Linux, macOS and Windows. Static, position-independent and library-only (no tools,
# tests, examples or docs); FFmpeg's LGPL libdav1d wrapper links it into the worker's shared
# libavcodec. The Meson configure vector is the adjacent dav1d.configure-arguments file, copied
# into the lock and checked byte for byte. x86 assembly uses the superbuild-private NASM.
set(BLOOM_DAV1D_VERSION 1.5.4)
set(BLOOM_DAV1D_URL
    "https://downloads.videolan.org/pub/videolan/dav1d/${BLOOM_DAV1D_VERSION}/dav1d-${BLOOM_DAV1D_VERSION}.tar.xz")
set(BLOOM_DAV1D_SHA256 686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/dav1d.configure-arguments")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/dav1d.configure-arguments"
    BLOOM_DAV1D_CONFIGURE_ARGUMENTS)

ExternalProject_Add(bloom_dependency_dav1d
    URL "${BLOOM_DAV1D_URL}"
    URL_HASH SHA256=${BLOOM_DAV1D_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME dav1d-${BLOOM_DAV1D_VERSION}.tar.xz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS ${BLOOM_CODEC_DEPENDS}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        ${BLOOM_MESON_EXECUTABLE} setup <BINARY_DIR> <SOURCE_DIR>
        --prefix=<INSTALL_DIR>
        ${BLOOM_DAV1D_CONFIGURE_ARGUMENTS}
        ${BLOOM_CODEC_MESON_ARGUMENTS}
    BUILD_COMMAND ${BLOOM_NINJA_EXECUTABLE} -C <BINARY_DIR> -j3
    INSTALL_COMMAND ${BLOOM_NINJA_EXECUTABLE} -C <BINARY_DIR> install
        COMMAND ${CMAKE_COMMAND}
        -DCODEC_PREFIX=<INSTALL_DIR>
        -DCODEC_LIBRARY=dav1d
        -DCODEC_PKGCONFIG=dav1d
        -DCODEC_WINDOWS=${WIN32}
        -P "${BLOOM_CODEC_STATIC_INSTALL_SCRIPT}"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
