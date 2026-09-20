# Reconfigure the same CMake cache while switching/removing the prefix and while pointing a
# tool symlink outside the prefix. Each configure must re-derive availability from the current
# prefix; no stale cache may keep a removed tool alive and no symlink may escape containment.
#
# Invoked as: cmake -DWORK_DIR=<scratch> -DHARNESS_DIR=<cmake/tests/gpu_tools_reconfigure> \
#                  -DMODULE_DIR=<cmake> -P reconfigure_gpu_shader_tools.cmake

foreach(required IN ITEMS WORK_DIR HARNESS_DIR MODULE_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "reconfigure_gpu_shader_tools.cmake requires -D${required}=...")
    endif()
endforeach()

set(_work "${WORK_DIR}")
set(_build "${_work}/build")
set(_failures "")

file(MAKE_DIRECTORY "${_work}/good/bin" "${_work}/missing" "${_work}/outside"
     "${_work}/good2/bin")

function(_bloom_reconfigure_write_tool path label)
    file(WRITE "${path}" "#!/bin/sh\n# fake ${label} for probe containment tests\n")
    file(CHMOD "${path}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endfunction()

_bloom_reconfigure_write_tool("${_work}/good/bin/glslangValidator" "glslangValidator")
_bloom_reconfigure_write_tool("${_work}/good/bin/spirv-val" "spirv-val")
_bloom_reconfigure_write_tool("${_work}/good2/bin/spirv-val" "spirv-val")
_bloom_reconfigure_write_tool("${_work}/outside/glslangValidator" "outside glslangValidator")
file(CREATE_LINK "${_work}/outside/glslangValidator"
    "${_work}/good2/bin/glslangValidator" SYMBOLIC)

function(_bloom_reconfigure_case name prefix expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${HARNESS_DIR}" -B "${_build}"
                "-DBLOOM_GPU_TOOLS_MODULE_DIR=${MODULE_DIR}"
                -DBLOOM_DEPENDENCY_MODE=qualified
                "-DBLOOM_DEPENDENCY_PREFIX=${prefix}"
        RESULT_VARIABLE _configure_result
        OUTPUT_VARIABLE _configure_output
        ERROR_VARIABLE _configure_error)
    if(NOT _configure_result EQUAL 0)
        set(_failures "${_failures};${name}: configure failed ${_configure_error}"
            PARENT_SCOPE)
        return()
    endif()
    file(READ "${_build}/probe-result.txt" _result)
    string(STRIP "${_result}" _result)
    if(NOT _result STREQUAL "${expected}")
        set(_failures "${_failures};${name}: expected '${expected}', got '${_result}'"
            PARENT_SCOPE)
    endif()
    message(STATUS "reconfigure case ${name}: ${_result}")
endfunction()

_bloom_reconfigure_case("switch-to-good" "${_work}/good" "TRUE|available")
_bloom_reconfigure_case("switch-to-missing" "${_work}/missing" "FALSE|missing-tools")
_bloom_reconfigure_case("switch-back-to-good" "${_work}/good" "TRUE|available")

file(REMOVE "${_work}/good/bin/spirv-val")
_bloom_reconfigure_case("tool-removed" "${_work}/good" "FALSE|missing-tools")

_bloom_reconfigure_write_tool("${_work}/good/bin/spirv-val" "spirv-val")
_bloom_reconfigure_case("tool-restored" "${_work}/good" "TRUE|available")

_bloom_reconfigure_case("symlink-outside" "${_work}/good2" "FALSE|tool-outside-prefix")

if(_failures)
    message(FATAL_ERROR "GPU shader tools reconfigure failures:${_failures}")
endif()
message(STATUS "GPU shader tools reconfigure cases passed (switch/removal/symlink containment)")
