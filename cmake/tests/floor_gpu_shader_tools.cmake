# Unit probe for the explicit dynamic-loader floor and prefix resolution classifier. An unknown
# soname must classify as unresolved (never silently treated as an operating-system library) and a
# prefix-private soname must classify as private with its prefix path.
#
# Invoked as: cmake -DWORK_DIR=<scratch> -P floor_gpu_shader_tools.cmake

if(NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "floor_gpu_shader_tools.cmake requires -DWORK_DIR=...")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/..")
include(BloomGpuShaderTools)

set(_prefix "${WORK_DIR}/floor-prefix")
file(MAKE_DIRECTORY "${_prefix}/lib")
file(WRITE "${_prefix}/lib/libprivate.so.1" "placeholder\n")
set(_failures "")

function(_bloom_floor_expect soname expected)
    _bloom_gpu_tools_is_os_floor("${soname}" _is_floor)
    if(NOT "${_is_floor}" STREQUAL "${expected}")
        set(_failures "${_failures};floor(${soname}): expected ${expected}, got ${_is_floor}"
            PARENT_SCOPE)
    endif()
endfunction()

_bloom_floor_expect("libc.so.6" TRUE)
_bloom_floor_expect("libstdc++.so.6" TRUE)
_bloom_floor_expect("libgcc_s.so.1" TRUE)
_bloom_floor_expect("libmystery.so.9" FALSE)
_bloom_floor_expect("libGL.so.1" FALSE)

function(_bloom_classify_expect soname expected_kind)
    _bloom_gpu_tools_classify_needed("${_prefix}" "${soname}" _kind _path)
    if(NOT _kind STREQUAL "${expected_kind}")
        set(_failures "${_failures};classify(${soname}): expected ${expected_kind}, got ${_kind}"
            PARENT_SCOPE)
    endif()
    if(_kind STREQUAL "private" AND NOT _path STREQUAL "${_prefix}/lib/${soname}")
        set(_failures "${_failures};classify(${soname}): wrong private path ${_path}"
            PARENT_SCOPE)
    endif()
endfunction()

_bloom_classify_expect("libc.so.6" "floor")
_bloom_classify_expect("libprivate.so.1" "private")
_bloom_classify_expect("libabsent.so.1" "unresolved")

if(_failures)
    message(FATAL_ERROR "GPU shader tools floor classifier failures:${_failures}")
endif()
message(STATUS "GPU shader tools floor/classifier probes passed")
