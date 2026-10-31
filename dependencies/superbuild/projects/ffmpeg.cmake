# FFmpeg 8.1.2 — the bundled media worker provider on Linux, macOS and Windows (ADR 0035). The 8.1
# maintenance branch is the current stable point-release line selected for this intake; the
# application never links these libraries, only bloom-media-worker does. LGPL-only, shared-library,
# library-only build: the exact allow-list lives in the adjacent argument file and is also copied
# into the generated lock by the reviewed intake input. The platform adjustments below change the
# toolchain, loader naming and Linux-only features, never the LGPL boundary.
set(BLOOM_FFMPEG_VERSION 8.1.2)
set(BLOOM_FFMPEG_URL
    "https://ffmpeg.org/releases/ffmpeg-${BLOOM_FFMPEG_VERSION}.tar.xz")
set(BLOOM_FFMPEG_SHA256 464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c)

# VA-API is a Linux interface; macOS and Windows builds never enable it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BLOOM_FFMPEG_VAAPI_DEFAULT ON)
else()
    set(BLOOM_FFMPEG_VAAPI_DEFAULT OFF)
endif()
option(BLOOM_FFMPEG_ENABLE_VAAPI
    "Enable FFmpeg VA-API hardware acceleration; turn off when libva development files are absent"
    ${BLOOM_FFMPEG_VAAPI_DEFAULT})
if(BLOOM_FFMPEG_ENABLE_VAAPI AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "BLOOM_FFMPEG_ENABLE_VAAPI is Linux-only")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/ffmpeg.configure-arguments")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/ffmpeg.configure-arguments"
    BLOOM_FFMPEG_CONFIGURE_ARGUMENTS)
if(NOT BLOOM_FFMPEG_ENABLE_VAAPI)
    list(REMOVE_ITEM BLOOM_FFMPEG_CONFIGURE_ARGUMENTS
        --enable-vaapi
        --enable-encoder=h264_vaapi
        --enable-encoder=hevc_vaapi
        --enable-hwaccel=h264_vaapi
        --enable-hwaccel=hevc_vaapi)
    list(APPEND BLOOM_FFMPEG_CONFIGURE_ARGUMENTS --disable-vaapi)
else()
    find_program(BLOOM_FFMPEG_PKG_CONFIG pkg-config)
    if(NOT BLOOM_FFMPEG_PKG_CONFIG)
        message(FATAL_ERROR
            "BLOOM_FFMPEG_ENABLE_VAAPI=ON requires pkg-config and libva; configure with "
            "-DBLOOM_FFMPEG_ENABLE_VAAPI=OFF when VA-API development files are unavailable.")
    endif()
    execute_process(
        COMMAND "${BLOOM_FFMPEG_PKG_CONFIG}" --exists libva
        RESULT_VARIABLE BLOOM_FFMPEG_LIBVA_RESULT)
    if(NOT BLOOM_FFMPEG_LIBVA_RESULT EQUAL 0)
        message(FATAL_ERROR
            "BLOOM_FFMPEG_ENABLE_VAAPI=ON requires libva development files; configure with "
            "-DBLOOM_FFMPEG_ENABLE_VAAPI=OFF when VA-API development files are unavailable.")
    endif()
endif()

# Bundled permissive codecs (codec-common.cmake): OpenH264 (H.264 encode), Kvazaar (HEVC encode),
# dav1d (AV1 decode), SVT-AV1 (AV1 encode), libvpx (VP9 encode) and Opus (Opus encode) are static
# archives linked into libavcodec on every platform, so the worker ships no extra runtime files. --pkg-config-flags=--static makes FFmpeg read their
# Libs.private runtime libraries.
set(BLOOM_FFMPEG_DEPENDS
    bloom_dependency_openh264
    bloom_dependency_kvazaar
    bloom_dependency_dav1d
    bloom_dependency_svt_av1
    bloom_dependency_libvpx
    bloom_dependency_opus)

# NVENC (nv-codec-headers.cmake): FFmpeg's h264_nvenc/hevc_nvenc compile against the MIT headers and
# load the NVIDIA driver at run time. macOS has no NVENC, so its build omits them.
if(BLOOM_NV_CODEC_HEADERS_REQUIRED)
    list(APPEND BLOOM_FFMPEG_DEPENDS bloom_dependency_nv_codec_headers)
else()
    list(REMOVE_ITEM BLOOM_FFMPEG_CONFIGURE_ARGUMENTS
        --enable-ffnvcodec
        --enable-nvenc
        --enable-encoder=h264_nvenc
        --enable-encoder=hevc_nvenc)
endif()

# Reviewed source patches (dependencies/patches/ffmpeg/, recorded in the lock with their digests and
# apply order). The Windows build takes `patch` from the same MSYS2 installation as its shell.
set(BLOOM_FFMPEG_PATCHES
    "${CMAKE_CURRENT_LIST_DIR}/../../patches/ffmpeg/0001-libkvazaar-pad-frames-to-whole-blocks.patch")
find_program(BLOOM_FFMPEG_PATCH_PROGRAM NAMES patch
    HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
set(BLOOM_FFMPEG_PATCH_COMMAND)
foreach(bloom_ffmpeg_patch IN LISTS BLOOM_FFMPEG_PATCHES)
    list(APPEND BLOOM_FFMPEG_PATCH_COMMAND
        COMMAND "${BLOOM_FFMPEG_PATCH_PROGRAM}" -p1 --forward --batch --input=${bloom_ffmpeg_patch})
endforeach()
list(POP_FRONT BLOOM_FFMPEG_PATCH_COMMAND)

# x86 SIMD: FFmpeg's x86 assembly is built with the superbuild-private NASM (nasm.cmake). Other
# targets use FFmpeg's inline/NEON assembly and need neither the tool nor the argument.
if(BLOOM_NASM_REQUIRED)
    list(APPEND BLOOM_FFMPEG_DEPENDS bloom_dependency_nasm)
    list(APPEND BLOOM_FFMPEG_CONFIGURE_ARGUMENTS --x86asmexe=${BLOOM_NASM_EXECUTABLE})
endif()

# Platform toolchain and loader naming. FFmpeg's configure is a POSIX shell script on every
# platform; Windows builds run it from an MSYS2 shell with the MSVC toolchain (FFmpeg's documented
# MSVC route), producing avcodec-NN.dll beside import libraries.
set(BLOOM_FFMPEG_SHELL)
set(BLOOM_FFMPEG_MAKE make)
set(BLOOM_FFMPEG_COMPILER_ARGUMENTS --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER})
if(APPLE)
    # Shared libraries carry @rpath install names, so the worker's @loader_path/lib rpath resolves
    # the copies bundled inside Bloom.app unmodified (no install_name_tool, signatures intact).
    list(APPEND BLOOM_FFMPEG_CONFIGURE_ARGUMENTS --install-name-dir=@rpath)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND BLOOM_FFMPEG_CONFIGURE_ARGUMENTS
            "--extra-cflags=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "--extra-ldflags=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
elseif(WIN32)
    find_program(BLOOM_FFMPEG_SHELL NAMES sh bash
        HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
    find_program(BLOOM_FFMPEG_MAKE_PROGRAM NAMES make
        HINTS "$ENV{MSYS2_ROOT}/usr/bin" "C:/msys64/usr/bin" REQUIRED)
    set(BLOOM_FFMPEG_MAKE "${BLOOM_FFMPEG_MAKE_PROGRAM}")
    # --toolchain=msvc selects cl/link from the Visual Studio developer environment.
    set(BLOOM_FFMPEG_COMPILER_ARGUMENTS)
    list(APPEND BLOOM_FFMPEG_CONFIGURE_ARGUMENTS --toolchain=msvc --target-os=win64)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        list(APPEND BLOOM_FFMPEG_CONFIGURE_ARGUMENTS --arch=aarch64)
    else()
        list(APPEND BLOOM_FFMPEG_CONFIGURE_ARGUMENTS --arch=x86_64)
    endif()
endif()

ExternalProject_Add(bloom_dependency_ffmpeg
    URL "${BLOOM_FFMPEG_URL}"
    URL_HASH SHA256=${BLOOM_FFMPEG_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME ffmpeg-${BLOOM_FFMPEG_VERSION}.tar.xz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS ${BLOOM_FFMPEG_DEPENDS}
    PATCH_COMMAND ${BLOOM_FFMPEG_PATCH_COMMAND}
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        PKG_CONFIG_PATH=${BLOOM_DEPENDENCY_PREFIX}/lib/pkgconfig
        ${BLOOM_FFMPEG_SHELL}
        <SOURCE_DIR>/configure
        ${BLOOM_FFMPEG_COMPILER_ARGUMENTS}
        --prefix=<INSTALL_DIR>
        ${BLOOM_FFMPEG_CONFIGURE_ARGUMENTS}
    BUILD_COMMAND ${BLOOM_FFMPEG_MAKE} -j3
    INSTALL_COMMAND ${BLOOM_FFMPEG_MAKE} install
    ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)

# FFmpeg has no upstream CMake package. Install a narrow config wrapper that exposes only the
# shared library targets the future bloom-media-worker may consume, while retaining the normal
# Bloom package-search restriction in cmake/BloomDependencyPrefix.cmake.
ExternalProject_Add_Step(bloom_dependency_ffmpeg bloom_install_cmake_config
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "<INSTALL_DIR>/lib/cmake/FFmpeg"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_LIST_DIR}/ffmpeg-config.cmake"
        "<INSTALL_DIR>/lib/cmake/FFmpeg/FFmpegConfig.cmake"
    DEPENDEES install
    COMMENT "bloom: installing the FFmpeg qualified-package wrapper")
