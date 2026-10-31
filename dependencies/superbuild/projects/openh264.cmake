# OpenH264 2.6.0 — Bloom's bundled H.264 software encoder (BSD-2-Clause), built from Cisco's
# pinned source release on Linux, macOS and Windows. It is a static, position-independent archive
# linked into the worker's shared libavcodec through FFmpeg's LGPL libopenh264 wrapper; no Cisco
# binary is downloaded, fetched at run time, or installed by the user. The Meson configure vector
# is the adjacent openh264.configure-arguments file, copied into the lock and checked byte for
# byte.
set(BLOOM_OPENH264_VERSION 2.6.0)
set(BLOOM_OPENH264_URL
    "https://github.com/cisco/openh264/archive/refs/tags/v${BLOOM_OPENH264_VERSION}.tar.gz")
set(BLOOM_OPENH264_SHA256 558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/openh264.configure-arguments")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/openh264.configure-arguments"
    BLOOM_OPENH264_CONFIGURE_ARGUMENTS)

ExternalProject_Add(bloom_dependency_openh264
    URL "${BLOOM_OPENH264_URL}"
    URL_HASH SHA256=${BLOOM_OPENH264_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME openh264-${BLOOM_OPENH264_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS ${BLOOM_CODEC_DEPENDS}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        ${BLOOM_MESON_EXECUTABLE} setup <BINARY_DIR> <SOURCE_DIR>
        --prefix=<INSTALL_DIR>
        ${BLOOM_OPENH264_CONFIGURE_ARGUMENTS}
        ${BLOOM_CODEC_MESON_ARGUMENTS}
    BUILD_COMMAND ${BLOOM_NINJA_EXECUTABLE} -C <BINARY_DIR> -j3
    INSTALL_COMMAND ${BLOOM_NINJA_EXECUTABLE} -C <BINARY_DIR> install
        COMMAND ${CMAKE_COMMAND}
        -DCODEC_PREFIX=<INSTALL_DIR>
        -DCODEC_LIBRARY=openh264
        -DCODEC_PKGCONFIG=openh264
        "-DCODEC_PRIVATE_LIBS=${BLOOM_CODEC_CXX_RUNTIME}"
        -DCODEC_WINDOWS=${WIN32}
        -P "${BLOOM_CODEC_STATIC_INSTALL_SCRIPT}"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
