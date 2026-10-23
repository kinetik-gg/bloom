# NASM 2.16.03 — build-host-only assembler for FFmpeg's x86 SIMD. It is assembled from source into
# a superbuild-private tools directory, never into the dependency prefix: the consumed prefix
# gains no build tool, and nothing from this component is linked or shipped. FFmpeg's configure
# is pointed at it through --x86asmexe. Non-x86 targets use FFmpeg's inline/NEON assembly and do
# not need an assembler, so the component is defined only when the target processor is x86.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([xX]86_64|AMD64|amd64|[xX]86|i[3-6]86)$")
    set(BLOOM_NASM_REQUIRED ON)
else()
    set(BLOOM_NASM_REQUIRED OFF)
endif()

set(BLOOM_NASM_VERSION 2.16.03)
set(BLOOM_NASM_URL
    "https://www.nasm.us/pub/nasm/releasebuilds/${BLOOM_NASM_VERSION}/nasm-${BLOOM_NASM_VERSION}.tar.xz")
set(BLOOM_NASM_SHA256 1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148)
set(BLOOM_NASM_TOOLS_PREFIX "${CMAKE_BINARY_DIR}/tools")

if(BLOOM_NASM_REQUIRED)
    ExternalProject_Add(bloom_dependency_nasm
        URL "${BLOOM_NASM_URL}"
        URL_HASH SHA256=${BLOOM_NASM_SHA256}
        DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        DOWNLOAD_NAME nasm-${BLOOM_NASM_VERSION}.tar.xz
        DOWNLOAD_NO_PROGRESS ON
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        BUILD_IN_SOURCE ON
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
            CC=${CMAKE_C_COMPILER}
            <SOURCE_DIR>/configure
            --prefix=${BLOOM_NASM_TOOLS_PREFIX}
        BUILD_COMMAND make -j3 nasm
        INSTALL_COMMAND make install
        ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
        BUILD_ALWAYS OFF
        INSTALL_DIR "${BLOOM_NASM_TOOLS_PREFIX}"
        USES_TERMINAL_DOWNLOAD ON
        USES_TERMINAL_BUILD ON)
    set(BLOOM_NASM_EXECUTABLE "${BLOOM_NASM_TOOLS_PREFIX}/bin/nasm")
endif()
