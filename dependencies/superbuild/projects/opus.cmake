# Opus 1.5.2 — Bloom's bundled Opus audio encoder (BSD-3-Clause), built from Xiph's pinned release
# on Linux, macOS and Windows. Static, position-independent and library-only (no demo programs,
# tests or documentation; the neural PLC, DRED and OSCE models are off, so no model data is
# compiled in); FFmpeg's LGPL libopus wrapper links it into the worker's shared libavcodec for the
# WebM formats. FFmpeg's native Opus decoder reads the result back. The Meson configure vector is
# the adjacent opus.configure-arguments file, copied into the lock and checked byte for byte. Opus
# uses compiler intrinsics with run-time dispatch; it needs no assembler.
set(BLOOM_OPUS_VERSION 1.5.2)
set(BLOOM_OPUS_URL
    "https://downloads.xiph.org/releases/opus/opus-${BLOOM_OPUS_VERSION}.tar.gz")
set(BLOOM_OPUS_SHA256 65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/opus.configure-arguments")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/opus.configure-arguments"
    BLOOM_OPUS_CONFIGURE_ARGUMENTS)

ExternalProject_Add(bloom_dependency_opus
    URL "${BLOOM_OPUS_URL}"
    URL_HASH SHA256=${BLOOM_OPUS_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME opus-${BLOOM_OPUS_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        ${BLOOM_MESON_EXECUTABLE} setup <BINARY_DIR> <SOURCE_DIR>
        --prefix=<INSTALL_DIR>
        ${BLOOM_OPUS_CONFIGURE_ARGUMENTS}
        ${BLOOM_CODEC_MESON_ARGUMENTS}
    BUILD_COMMAND ${BLOOM_NINJA_EXECUTABLE} -C <BINARY_DIR> -j3
    INSTALL_COMMAND ${BLOOM_NINJA_EXECUTABLE} -C <BINARY_DIR> install
        COMMAND ${CMAKE_COMMAND}
        -DCODEC_PREFIX=<INSTALL_DIR>
        -DCODEC_LIBRARY=opus
        -DCODEC_PKGCONFIG=opus
        -DCODEC_WINDOWS=${WIN32}
        -P "${BLOOM_CODEC_STATIC_INSTALL_SCRIPT}"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
