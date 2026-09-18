# OpenH264 2.6.0 — source-built link-time stub for FFmpeg's LGPL-compatible libopenh264 wrapper.
# The source library is private to the superbuild. The qualified prefix receives headers, a
# pkg-config record, and a GNU ld import script only; Cisco's separately fetched runtime binary
# is never installed here or copied beside the worker.
set(BLOOM_OPENH264_VERSION 2.6.0)
set(BLOOM_OPENH264_URL
    "https://github.com/cisco/openh264/archive/refs/tags/v${BLOOM_OPENH264_VERSION}.tar.gz")
set(BLOOM_OPENH264_SHA256 558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69)
set(BLOOM_OPENH264_PRIVATE_PREFIX "${CMAKE_BINARY_DIR}/openh264-private")

ExternalProject_Add(bloom_dependency_openh264
    URL "${BLOOM_OPENH264_URL}"
    URL_HASH SHA256=${BLOOM_OPENH264_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME openh264-${BLOOM_OPENH264_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        make -j3 V=No BUILDTYPE=Debug USE_ASM=No clean
        COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        make -j3 V=No BUILDTYPE=Debug USE_ASM=No libraries
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        make -C <SOURCE_DIR> -j3 V=No BUILDTYPE=Debug USE_ASM=No install-shared PREFIX=${BLOOM_OPENH264_PRIVATE_PREFIX}
        COMMAND ${CMAKE_COMMAND}
        -DOPENH264_PRIVATE_PREFIX=${BLOOM_OPENH264_PRIVATE_PREFIX}
        -DOPENH264_INSTALL_PREFIX=<INSTALL_DIR>
        -P "${CMAKE_CURRENT_LIST_DIR}/openh264-install.cmake"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
