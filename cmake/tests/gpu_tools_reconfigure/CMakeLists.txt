cmake_minimum_required(VERSION 3.25)

project(BloomGpuToolsReconfigure NONE)

if(NOT DEFINED BLOOM_GPU_TOOLS_MODULE_DIR)
    message(FATAL_ERROR "BLOOM_GPU_TOOLS_MODULE_DIR is required")
endif()
list(APPEND CMAKE_MODULE_PATH "${BLOOM_GPU_TOOLS_MODULE_DIR}")
include(BloomGpuShaderTools)

bloom_gpu_shader_tools_probe()
file(WRITE "${CMAKE_BINARY_DIR}/probe-result.txt"
    "${bloom_gpu_tools_available}|${bloom_gpu_tools_reason}\n")
