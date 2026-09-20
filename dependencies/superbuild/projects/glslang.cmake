# glslang at the Khronos `vulkan-sdk-1.4.357.0` source ref, commit
# 168d452a4f460d24b588fed08477a81c44ee27a1. The SDK ref is a Vulkan SDK source tag, not the
# glslang software version: glslang derives its own version from CHANGES.md, and the lock records
# that emitted project version while retaining this source ref and commit separately.
#
# Offline compiler only. BUILD_EXTERNAL=OFF and ALLOW_EXTERNAL_SPIRV_TOOLS=OFF mean no submodule
# fetch and no SPIRV-Tools linkage; ENABLE_OPT=OFF so the optimizer is not required (glslang vendors
# its own SPIRV/spirv.hpp11). GLSLANG_TESTS=OFF so no test tree or extra dependency (googletest) is
# reached. ENABLE_GLSLANG_BINARIES=ON builds the standalone `glslang`/`glslangValidator` tool;
# ENABLE_HLSL=OFF keeps the unused HLSL front end out. Build-only tooling: no end-user runtime
# artifact ships from this component, so shippingRoles is empty.
set(BLOOM_GLSLANG_SDK_REF vulkan-sdk-1.4.357.0)
set(BLOOM_GLSLANG_COMMIT 168d452a4f460d24b588fed08477a81c44ee27a1)
set(BLOOM_GLSLANG_URL
    "https://codeload.github.com/KhronosGroup/glslang/tar.gz/refs/tags/${BLOOM_GLSLANG_SDK_REF}")
set(BLOOM_GLSLANG_SHA256
    81038794e20494556edbcc0fc70fa984d71d1b440f9c49adf2cbaaa60a519757)

ExternalProject_Add(bloom_dependency_glslang
    URL "${BLOOM_GLSLANG_URL}"
    URL_HASH SHA256=${BLOOM_GLSLANG_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME glslang-${BLOOM_GLSLANG_SDK_REF}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DBUILD_EXTERNAL:BOOL=OFF
        -DALLOW_EXTERNAL_SPIRV_TOOLS:BOOL=OFF
        -DENABLE_OPT:BOOL=OFF
        -DGLSLANG_TESTS:BOOL=OFF
        -DENABLE_GLSLANG_BINARIES:BOOL=ON
        -DGLSLANG_ENABLE_INSTALL:BOOL=ON
        -DENABLE_HLSL:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
