# SPIRV-Headers @ 29981f65241605e08b0ede4cfeb999fe3b723c6a - the Khronos SPIR-V headers, pinned to
# the exact revision glslang's known_good.json names for the vulkan-sdk-1.4.357.0 source ref. It is
# used only as a build-time source tree: SPIRV-Tools consumes its include directory directly.
#
# Linkage is not a runtime library: this component is a build-only header source. It installs
# nothing into the shared prefix (SPIRV_HEADERS_ENABLE_INSTALL=OFF) and ships no end-user artifact;
# its lock record exists because it is a reached build dependency and carries an MIT license notice.
# Tests are OFF so no test tree or extra dependency is reached.
set(BLOOM_SPIRV_HEADERS_COMMIT 29981f65241605e08b0ede4cfeb999fe3b723c6a)
set(BLOOM_SPIRV_HEADERS_URL
    "https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/${BLOOM_SPIRV_HEADERS_COMMIT}")
set(BLOOM_SPIRV_HEADERS_SHA256
    232899f1ad4104fb5bc377b94596c7621575eee62ad9a9e8f929b63a7dd8a7ad)

ExternalProject_Add(bloom_dependency_spirv-headers
    URL "${BLOOM_SPIRV_HEADERS_URL}"
    URL_HASH SHA256=${BLOOM_SPIRV_HEADERS_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME spirv-headers-${BLOOM_SPIRV_HEADERS_COMMIT}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DSPIRV_HEADERS_ENABLE_TESTS:BOOL=OFF
        -DSPIRV_HEADERS_ENABLE_INSTALL:BOOL=OFF
    BUILD_ALWAYS OFF
    # Source-only provider: it installs nothing, so SPIRV_HEADERS_ENABLE_INSTALL=OFF generates no
    # `install` target. The install step is explicitly a no-op rather than a failing default.
    INSTALL_COMMAND ""
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
