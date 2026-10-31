# libvpx 1.17.0 — Bloom's bundled VP9 software encoder (BSD-3-Clause with Google's VP8/VP9 patent
# grant), built from the pinned upstream tag on Linux, macOS and Windows. Static,
# position-independent and library-only: VP9 encode with the 10-bit (Profile 2) path, no VP8, no
# VP9 decoder (FFmpeg's native decoder reads VP9), no tools, examples, tests, WebM I/O or libyuv.
# FFmpeg's LGPL libvpx wrapper links it into the worker's shared libavcodec. The configure vector
# is the adjacent libvpx.configure-arguments file, copied into the lock and checked byte for byte.
# x86 assembly uses the superbuild-private NASM through `AS`; ARM uses libvpx's NEON kernels.
set(BLOOM_LIBVPX_VERSION 1.17.0)
set(BLOOM_LIBVPX_URL
    "https://github.com/webmproject/libvpx/archive/refs/tags/v${BLOOM_LIBVPX_VERSION}.tar.gz")
set(BLOOM_LIBVPX_SHA256 1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/libvpx.configure-arguments")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/libvpx.configure-arguments"
    BLOOM_LIBVPX_CONFIGURE_ARGUMENTS)

set(BLOOM_LIBVPX_ENV CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER})
if(BLOOM_NASM_REQUIRED)
    list(APPEND BLOOM_LIBVPX_ENV AS=${BLOOM_NASM_EXECUTABLE})
endif()
set(BLOOM_LIBVPX_SHELL)
set(BLOOM_LIBVPX_MAKE make)
set(BLOOM_LIBVPX_PLATFORM_ARGUMENTS)
set(BLOOM_LIBVPX_POST_INSTALL)
if(WIN32)
    # libvpx's MSVC route: configure (a POSIX shell script, run from the same MSYS2 installation as
    # FFmpeg's) generates a Visual Studio solution, and its make drives MSBuild with the static
    # runtime FFmpeg's MSVC link uses. The import name and pkg-config record FFmpeg expects are
    # written afterwards.
    find_program(BLOOM_LIBVPX_SHELL NAMES sh bash
        HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
    find_program(BLOOM_LIBVPX_MAKE_PROGRAM NAMES make
        HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
    set(BLOOM_LIBVPX_MAKE "${BLOOM_LIBVPX_MAKE_PROGRAM}")
    set(BLOOM_LIBVPX_ENV)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(BLOOM_LIBVPX_ARCH arm64)
    else()
        set(BLOOM_LIBVPX_ARCH x64)
        list(APPEND BLOOM_LIBVPX_ENV AS=${BLOOM_NASM_EXECUTABLE})
    endif()
    string(REPLACE "x64" "x86_64" bloom_libvpx_target "${BLOOM_LIBVPX_ARCH}")
    list(APPEND BLOOM_LIBVPX_PLATFORM_ARGUMENTS
        --target=${bloom_libvpx_target}-win64-vs17 --enable-static-msvcrt)
    set(BLOOM_LIBVPX_PC "${CMAKE_CURRENT_BINARY_DIR}/bloom-libvpx-vpx.pc")
    file(WRITE "${BLOOM_LIBVPX_PC}"
        "prefix=${BLOOM_DEPENDENCY_PREFIX}\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\n\n"
        "Name: vpx\nDescription: WebM Project VPx codec implementation\n"
        "Version: ${BLOOM_LIBVPX_VERSION}\nLibs: -L\${libdir} -lvpx\nCflags: -I\${includedir}\n")
    set(BLOOM_LIBVPX_POST_INSTALL
        COMMAND ${CMAKE_COMMAND} -E copy
            <INSTALL_DIR>/lib/${BLOOM_LIBVPX_ARCH}/vpxmt.lib <INSTALL_DIR>/lib/vpx.lib
        COMMAND ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/lib/pkgconfig
        COMMAND ${CMAKE_COMMAND} -E copy "${BLOOM_LIBVPX_PC}" <INSTALL_DIR>/lib/pkgconfig/vpx.pc)
endif()

ExternalProject_Add(bloom_dependency_libvpx
    URL "${BLOOM_LIBVPX_URL}"
    URL_HASH SHA256=${BLOOM_LIBVPX_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME libvpx-${BLOOM_LIBVPX_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS ${BLOOM_CODEC_DEPENDS}
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        ${BLOOM_LIBVPX_ENV}
        ${BLOOM_DEPENDENCY_COMMON_ENV}
        ${BLOOM_LIBVPX_SHELL}
        <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --libdir=<INSTALL_DIR>/lib
        ${BLOOM_LIBVPX_CONFIGURE_ARGUMENTS}
        ${BLOOM_LIBVPX_PLATFORM_ARGUMENTS}
    BUILD_COMMAND ${BLOOM_LIBVPX_MAKE} -j3
    INSTALL_COMMAND ${BLOOM_LIBVPX_MAKE} install
        ${BLOOM_LIBVPX_POST_INSTALL}
        COMMAND ${CMAKE_COMMAND}
        -DCODEC_PREFIX=<INSTALL_DIR>
        -DCODEC_LIBRARY=vpx
        -DCODEC_PKGCONFIG=vpx
        -DCODEC_WINDOWS=OFF
        -P "${BLOOM_CODEC_STATIC_INSTALL_SCRIPT}"
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
