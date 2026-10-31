# nv-codec-headers 12.1.14.1 — FFmpeg's MIT-licensed copy of NVIDIA's Video Codec SDK interface
# headers (dependencies/licenses/nv-codec-headers/). Header-only: FFmpeg's NVENC encoders load the
# NVIDIA driver's encode library at run time (dlopen/LoadLibrary), so nothing NVIDIA is linked or
# shipped, and a computer without the driver simply has no NVENC provider. The 12.1 line needs a
# Linux driver 530.41.03 or newer (Windows 531.61). Installs the headers and the ffnvcodec
# pkg-config record into the dependency prefix; macOS has no NVENC and does not build it.
set(BLOOM_NV_CODEC_HEADERS_VERSION 12.1.14.1)
set(BLOOM_NV_CODEC_HEADERS_URL
    "https://github.com/FFmpeg/nv-codec-headers/releases/download/n${BLOOM_NV_CODEC_HEADERS_VERSION}/nv-codec-headers-${BLOOM_NV_CODEC_HEADERS_VERSION}.tar.gz")
set(BLOOM_NV_CODEC_HEADERS_SHA256 043e46265517855f13bcb98b3a225b2761b2a83983b1645fa1d05876f1c3a781)

if(APPLE)
    set(BLOOM_NV_CODEC_HEADERS_REQUIRED OFF)
else()
    set(BLOOM_NV_CODEC_HEADERS_REQUIRED ON)
endif()

if(BLOOM_NV_CODEC_HEADERS_REQUIRED)
    ExternalProject_Add(bloom_dependency_nv_codec_headers
        URL "${BLOOM_NV_CODEC_HEADERS_URL}"
        URL_HASH SHA256=${BLOOM_NV_CODEC_HEADERS_SHA256}
        DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
        DOWNLOAD_NAME nv-codec-headers-${BLOOM_NV_CODEC_HEADERS_VERSION}.tar.gz
        DOWNLOAD_NO_PROGRESS ON
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=<SOURCE_DIR>
            -DINSTALL_PREFIX=<INSTALL_DIR>
            -P "${CMAKE_CURRENT_LIST_DIR}/nv-codec-headers-install.cmake"
        ${BLOOM_DEPENDENCY_OFFLINE_ARGS}
        BUILD_ALWAYS OFF
        INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
        USES_TERMINAL_DOWNLOAD ON)
endif()
