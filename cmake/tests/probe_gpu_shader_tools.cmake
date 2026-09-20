# Script-mode test for the bloom_gpu_shader_tools_probe() availability contract.
#
# Invoked as: cmake -DGPU_TOOLS_TESTS_DIR=<cmake/tests> -DBLOOM_DEPENDENCY_MODE=... \
#                  -DBLOOM_DEPENDENCY_PREFIX=... -DBLOOM_RENDER_ENABLE_VULKAN=... \
#                  -DEXPECT_REASON=... -P probe_gpu_shader_tools.cmake
#
# A missing prefix, a prefix without the pinned tools, and an explicit CPU-stub build must each
# report a distinct typed reason and never fall back to the ambient PATH.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/..")
include(BloomGpuShaderTools)

bloom_gpu_shader_tools_probe()

if(bloom_gpu_tools_available)
    message(FATAL_ERROR
        "expected GPU shader tools to be unavailable, but the probe reported available")
endif()
if(NOT bloom_gpu_tools_reason STREQUAL EXPECT_REASON)
    message(FATAL_ERROR
        "expected probe reason '${EXPECT_REASON}', got '${bloom_gpu_tools_reason}'")
endif()
if(bloom_gpu_tools_glslang OR bloom_gpu_tools_spirv_val)
    message(FATAL_ERROR
        "an unavailable probe must not publish a tool path (never an ambient PATH fallback)")
endif()
message(STATUS "GPU shader tools probe: unavailable with reason '${bloom_gpu_tools_reason}' as expected")
