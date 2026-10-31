# Kvazaar 2.3.2 — Bloom's bundled HEVC software encoder (BSD-3-Clause), built from the pinned
# upstream release on Linux, macOS and Windows. Static, position-independent and library-only
# (no CLI, no tests, no crypto); FFmpeg's LGPL libkvazaar wrapper links it into the worker's shared
# libavcodec. The CMake configure vector is the adjacent kvazaar.configure-arguments file, copied
# into the lock and checked byte for byte. Kvazaar's x86 kernels are compiler intrinsics with
# run-time dispatch; it needs no assembler.
set(BLOOM_KVAZAAR_VERSION 2.3.2)
set(BLOOM_KVAZAAR_URL
    "https://github.com/ultravideo/kvazaar/releases/download/v${BLOOM_KVAZAAR_VERSION}/kvazaar-${BLOOM_KVAZAAR_VERSION}.tar.xz")
set(BLOOM_KVAZAAR_SHA256 35e506a6d2be9ae343f17787eb80fa214492b276e9c88b6e93749050ae380070)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/kvazaar.configure-arguments")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/kvazaar.configure-arguments"
    BLOOM_KVAZAAR_CONFIGURE_ARGUMENTS)
if(WIN32)
    set(BLOOM_KVAZAAR_PRIVATE_LIBS)
else()
    set(BLOOM_KVAZAAR_PRIVATE_LIBS "-lpthread -lm")
endif()

ExternalProject_Add(bloom_dependency_kvazaar
    URL "${BLOOM_KVAZAAR_URL}"
    URL_HASH SHA256=${BLOOM_KVAZAAR_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME kvazaar-${BLOOM_KVAZAAR_VERSION}.tar.xz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
        -DCMAKE_INSTALL_LIBDIR:PATH=lib
        ${BLOOM_KVAZAAR_CONFIGURE_ARGUMENTS}
        ${BLOOM_CODEC_CMAKE_PLATFORM_ARGUMENTS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel 3
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
        COMMAND ${CMAKE_COMMAND}
        -DCODEC_PREFIX=<INSTALL_DIR>
        -DCODEC_LIBRARY=kvazaar
        -DCODEC_PKGCONFIG=kvazaar
        "-DCODEC_PRIVATE_LIBS=${BLOOM_KVAZAAR_PRIVATE_LIBS}"
        -DCODEC_WINDOWS=${WIN32}
        -P "${BLOOM_CODEC_STATIC_INSTALL_SCRIPT}"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
