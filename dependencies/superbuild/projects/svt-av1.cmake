# SVT-AV1 4.2.0 — Bloom's bundled AV1 software encoder (BSD-3-Clause-Clear with the Alliance for
# Open Media Patent License 1.0), built from the pinned upstream release on Linux, macOS and
# Windows. Static, position-independent and library-only (no encoder app, no tests); FFmpeg's LGPL
# libsvtav1 wrapper links it into the worker's shared libavcodec. Link-time optimization is off so
# the archive holds ordinary objects FFmpeg's link can resolve, and the git hash is excluded so the
# archive is reproducible. The CMake configure vector is the adjacent svt-av1.configure-arguments
# file, copied into the lock and checked byte for byte. x86 assembly uses the superbuild-private
# NASM; ARM uses SVT-AV1's own NEON/SVE kernels.
set(BLOOM_SVT_AV1_VERSION 4.2.0)
set(BLOOM_SVT_AV1_URL
    "https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v${BLOOM_SVT_AV1_VERSION}/SVT-AV1-v${BLOOM_SVT_AV1_VERSION}.tar.gz")
set(BLOOM_SVT_AV1_SHA256 c7b13c4a84bd3751aa35fcc72be13e6875467e7c2216879251a486e5b1e4e740)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/svt-av1.configure-arguments")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/svt-av1.configure-arguments"
    BLOOM_SVT_AV1_CONFIGURE_ARGUMENTS)
set(BLOOM_SVT_AV1_ASSEMBLER_ARGUMENTS)
if(BLOOM_NASM_REQUIRED)
    set(BLOOM_SVT_AV1_ASSEMBLER_ARGUMENTS
        -DCMAKE_ASM_NASM_COMPILER:FILEPATH=${BLOOM_NASM_EXECUTABLE})
endif()

ExternalProject_Add(bloom_dependency_svt_av1
    URL "${BLOOM_SVT_AV1_URL}"
    URL_HASH SHA256=${BLOOM_SVT_AV1_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME SVT-AV1-v${BLOOM_SVT_AV1_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS ${BLOOM_CODEC_DEPENDS}
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
        -DCMAKE_INSTALL_LIBDIR:PATH=lib
        ${BLOOM_SVT_AV1_CONFIGURE_ARGUMENTS}
        ${BLOOM_SVT_AV1_ASSEMBLER_ARGUMENTS}
        ${BLOOM_CODEC_CMAKE_PLATFORM_ARGUMENTS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel 3
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
        COMMAND ${CMAKE_COMMAND}
        -DCODEC_PREFIX=<INSTALL_DIR>
        -DCODEC_LIBRARY=SvtAv1Enc
        -DCODEC_PKGCONFIG=SvtAv1Enc
        -DCODEC_WINDOWS=${WIN32}
        -P "${BLOOM_CODEC_STATIC_INSTALL_SCRIPT}"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
