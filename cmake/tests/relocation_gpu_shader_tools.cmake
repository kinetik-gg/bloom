# Exercise the relocated-identity recorder: when patchelf rewrites staged bytes the inventory
# stagedSha256 must become the actual post-rewrite digest while sourceSha256 stays the source
# identity. This is the path the current tools do not take, so it is unit-tested directly.
#
# Invoked as: cmake -DWORK_DIR=<scratch> -P relocation_gpu_shader_tools.cmake

if(NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "relocation_gpu_shader_tools.cmake requires -DWORK_DIR=...")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/..")
include(BloomGpuShaderTools)

set(_stage "${WORK_DIR}/relocation-stage")
file(MAKE_DIRECTORY "${_stage}")
file(WRITE "${_stage}/glslangValidator" "staged glslang bytes after rpath\n")
file(WRITE "${_stage}/spirv-val" "staged spirv bytes after rpath\n")
file(SHA256 "${_stage}/glslangValidator" _glslang_staged)
file(SHA256 "${_stage}/spirv-val" _spirv_staged)

set(_placeholder "sha256:0000000000000000000000000000000000000000000000000000000000000000")
string(CONCAT _inventory
    "{\"tools\":["
    "{\"name\":\"glslangValidator\",\"file\":\"glslangValidator\","
    "\"sourceSha256\":\"${_placeholder}\","
    "\"stagedSha256\":\"sha256:PENDING-glslangValidator\"},"
    "{\"name\":\"spirv-val\",\"file\":\"spirv-val\","
    "\"sourceSha256\":\"${_placeholder}\","
    "\"stagedSha256\":\"sha256:PENDING-spirv-val\"}]}")
file(WRITE "${_stage}/inventory.json" "${_inventory}")

_bloom_gpu_tools_write_relocation_script("${WORK_DIR}/record-staged-hash.cmake"
    "glslangValidator" "spirv-val")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DGPU_TOOLS_STAGE=${_stage}"
            "-DGPU_TOOLS_INVENTORY=${_stage}/inventory.json"
            -P "${WORK_DIR}/record-staged-hash.cmake"
    RESULT_VARIABLE _record_result
    ERROR_VARIABLE _record_error)
if(NOT _record_result EQUAL 0)
    message(FATAL_ERROR "relocation recorder failed: ${_record_error}")
endif()

file(READ "${_stage}/inventory.json" _updated)
string(JSON _glslang_recorded GET "${_updated}" "tools" 0 "stagedSha256")
string(JSON _spirv_recorded GET "${_updated}" "tools" 1 "stagedSha256")
string(JSON _source_unchanged GET "${_updated}" "tools" 0 "sourceSha256")

if(NOT _glslang_recorded STREQUAL "sha256:${_glslang_staged}")
    message(FATAL_ERROR "glslang stagedSha256 not updated to actual staged bytes")
endif()
if(NOT _spirv_recorded STREQUAL "sha256:${_spirv_staged}")
    message(FATAL_ERROR "spirv-val stagedSha256 not updated to actual staged bytes")
endif()
if(NOT _source_unchanged STREQUAL _placeholder)
    message(FATAL_ERROR "sourceSha256 must stay the source identity")
endif()
message(STATUS "GPU shader tools relocated-identity recorder passed")
