# SPIRV-Tools @ b707790a898e44038547df54580022fc1cf89c3d - the Khronos SPIR-V tools, pinned to the
# exact revision glslang's known_good.json names for vulkan-sdk-1.4.357.0. It builds the offline
# `spirv-val` validator (and the other tools) into the shared prefix. Build-only tooling: no
# end-user runtime artifact ships from this component, so shippingRoles is empty.
#
# The SPIR-V headers are supplied as an explicit source directory from the sibling ExternalProject
# (SPIRV-Headers_SOURCE_DIR), with a build DEPENDS edge. No system header resolution and no
# in-recipe fetch occur: SPIRV-Tools' external/CMakeLists.txt requires one of those, and the
# explicit source dir satisfies it. Tests are OFF, so googletest/effcee/re2/abseil are never
# reached or downloaded. The optimizer is built as part of the tools but is not linked into glslang
# (glslang is configured ENABLE_OPT=OFF independently). SPIRV_WERROR is OFF so upstream warnings
# cannot fail the dependency build.
set(BLOOM_SPIRV_TOOLS_COMMIT b707790a898e44038547df54580022fc1cf89c3d)
set(BLOOM_SPIRV_TOOLS_URL
    "https://codeload.github.com/KhronosGroup/SPIRV-Tools/tar.gz/${BLOOM_SPIRV_TOOLS_COMMIT}")
set(BLOOM_SPIRV_TOOLS_SHA256
    05d8af89737bde57571c48dbd36714c9f520a69623e14de72c3be6b600e277d6)

# The ExternalProject source directory of the sibling SPIRV-Headers target. It exists once the
# spirv-headers download/extract step has run, which the DEPENDS edge below orders before this
# configure step.
set(BLOOM_SPIRV_HEADERS_SOURCE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/bloom_dependency_spirv-headers-prefix/src/bloom_dependency_spirv-headers")

ExternalProject_Add(bloom_dependency_spirv-tools
    URL "${BLOOM_SPIRV_TOOLS_URL}"
    URL_HASH SHA256=${BLOOM_SPIRV_TOOLS_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME spirv-tools-${BLOOM_SPIRV_TOOLS_COMMIT}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS
        bloom_dependency_spirv-headers
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_PREFIX_PATH:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DSPIRV-Headers_SOURCE_DIR:PATH=${BLOOM_SPIRV_HEADERS_SOURCE_DIR}
        -DSPIRV_SKIP_TESTS:BOOL=ON
        -DSPIRV_SKIP_EXECUTABLES:BOOL=OFF
        -DSPIRV_BUILD_FUZZER:BOOL=OFF
        -DSPIRV_WERROR:BOOL=OFF
    # Deterministic build-version identity. upstream utils/update_build_version.py reads
    # FORCED_BUILD_VERSION_DESCRIPTION when set, and otherwise falls back to `git describe` /
    # `git rev-parse` of the source tree. Because this archive is extracted inside the Bloom
    # checkout, that fallback would bake Bloom's parent git HEAD into `spirv-val --version`.
    # The build and install generation steps are therefore wrapped in `cmake -E env` with the
    # description pinned to this component's exact source commit. The default recursive-make
    # build/install commands are replaced with generator-agnostic `cmake --build` / `cmake --install`
    # so the environment is applied on every platform.
    BUILD_COMMAND
        ${CMAKE_COMMAND} -E env
        "FORCED_BUILD_VERSION_DESCRIPTION=${BLOOM_SPIRV_TOOLS_COMMIT}"
        ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E env
        "FORCED_BUILD_VERSION_DESCRIPTION=${BLOOM_SPIRV_TOOLS_COMMIT}"
        ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
