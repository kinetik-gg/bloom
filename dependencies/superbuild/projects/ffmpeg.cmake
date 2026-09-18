# FFmpeg 8.1.2 — the Linux worker-only media provider intake. The 8.1 maintenance branch is the
# current stable point-release line selected for this intake; the application never links these
# libraries. LGPL-only, shared-library, library-only build: the exact allow-list lives in the
# adjacent argument file and is also copied into the generated lock by the reviewed intake input.
set(BLOOM_FFMPEG_VERSION 8.1.2)
set(BLOOM_FFMPEG_URL
    "https://ffmpeg.org/releases/ffmpeg-${BLOOM_FFMPEG_VERSION}.tar.xz")
set(BLOOM_FFMPEG_SHA256 464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c)

option(BLOOM_FFMPEG_ENABLE_VAAPI
    "Enable FFmpeg VA-API hardware acceleration; turn off when libva development files are absent"
    ON)

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

ExternalProject_Add(bloom_dependency_ffmpeg
    URL "${BLOOM_FFMPEG_URL}"
    URL_HASH SHA256=${BLOOM_FFMPEG_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME ffmpeg-${BLOOM_FFMPEG_VERSION}.tar.xz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS bloom_dependency_openh264
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        PKG_CONFIG_PATH=${BLOOM_DEPENDENCY_PREFIX}/lib/pkgconfig
        <SOURCE_DIR>/configure
        --cc=${CMAKE_C_COMPILER}
        --cxx=${CMAKE_CXX_COMPILER}
        --prefix=<INSTALL_DIR>
        ${BLOOM_FFMPEG_CONFIGURE_ARGUMENTS}
    BUILD_COMMAND make -j3
    INSTALL_COMMAND make install
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
