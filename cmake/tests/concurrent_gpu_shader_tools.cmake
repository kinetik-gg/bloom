# Reproduce concurrent GPU shader-tool staging into one destination directory and prove the
# destination-scoped lock keeps it correct across repeated parallel builds.
#
# Several targets can share an output directory (all the runtime GPU tests do), so their staging
# targets copy the tools, license evidence, and inventory into the same executable-relative
# directory at the same time. Before serialization this raced `cmake -E copy_directory` and failed
# with "No such file or directory". This harness configures a scratch project whose custom targets
# all stage into ONE directory, builds it with real parallelism, and repeats the build to confirm
# the inventories and staged hashes stay valid.
#
# Invoked as: cmake -DWORK_DIR=<scratch> -DMODULE_DIR=<cmake> [-DGENERATOR=<cmake generator>] \
#                  -P concurrent_gpu_shader_tools.cmake
#
# Registered as bloom.gpu.shader-tools-concurrent-staging in apps/bloom-cli/CMakeLists.txt. It needs
# no qualified prefix, so it runs regardless of BLOOM_GPU_SHADER_TOOLS_AVAILABLE.

foreach(_required IN ITEMS WORK_DIR MODULE_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "concurrent_gpu_shader_tools.cmake requires -D${_required}=...")
    endif()
endforeach()

set(_helper "${MODULE_DIR}/BloomGpuShaderToolsStage.cmake")
if(NOT EXISTS "${_helper}")
    message(FATAL_ERROR "staging helper is absent: ${_helper}")
endif()

set(_work "${WORK_DIR}")
set(_project "${_work}/project")
set(_build "${_work}/build")
set(_bin "${_work}/bin")
set(_generated "${_work}/generated")
set(_stage "${_work}/shared-stage/bloom-gpu-tools")
set(_target_count 8)
set(_license_count 48)
set(_rounds 3)

file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_bin}" "${_generated}/licenses")

file(WRITE "${_bin}/glslang" "#!/bin/sh\n# staged glslang stand-in\n")
file(WRITE "${_bin}/spirv-val" "#!/bin/sh\n# staged spirv-val stand-in\n")
file(CHMOD "${_bin}/glslang" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(CHMOD "${_bin}/spirv-val" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

foreach(_index RANGE 1 ${_license_count})
    file(WRITE "${_generated}/licenses/license-${_index}.txt" "reviewed license ${_index}\n")
endforeach()

file(SHA256 "${_bin}/glslang" _glslang_sha)
file(SHA256 "${_bin}/spirv-val" _spirv_sha)
string(CONCAT _inventory
    "{\"format\":\"org.kinetik.bloom.gpu-shader-tools.inventory\",\"version\":1,"
    "\"layout\":\"executable-relative\",\"directory\":\"bloom-gpu-tools\",\"relocated\":false,"
    "\"tools\":["
    "{\"name\":\"glslangValidator\",\"file\":\"glslangValidator\","
    "\"sourceSha256\":\"sha256:${_glslang_sha}\","
    "\"stagedSha256\":\"sha256:${_glslang_sha}\"},"
    "{\"name\":\"spirv-val\",\"file\":\"spirv-val\","
    "\"sourceSha256\":\"sha256:${_spirv_sha}\","
    "\"stagedSha256\":\"sha256:${_spirv_sha}\"}],"
    "\"licenses\":[]}\n")
file(WRITE "${_generated}/inventory.json" "${_inventory}")

file(WRITE "${_project}/CMakeLists.txt" [==[
cmake_minimum_required(VERSION 3.21)
project(bloom_concurrent_stage_probe NONE)
set(_helper "${PROBE_MODULE_DIR}/BloomGpuShaderToolsStage.cmake")
foreach(_index RANGE 1 ${PROBE_TARGET_COUNT})
    add_custom_target(stage_${_index} ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DGPU_TOOLS_STAGE=${PROBE_STAGE_DIR}"
            "-DGPU_TOOLS_GENERATED=${PROBE_GENERATED_DIR}"
            "-DGPU_TOOLS_GLSLANG_SOURCE=${PROBE_GLSLANG_SOURCE}"
            "-DGPU_TOOLS_GLSLANG_NAME=glslangValidator"
            "-DGPU_TOOLS_SPIRV_SOURCE=${PROBE_SPIRV_SOURCE}"
            "-DGPU_TOOLS_SPIRV_NAME=spirv-val"
            "-DGPU_TOOLS_PRIVATE_COUNT=0"
            -P "${_helper}"
        VERBATIM)
endforeach()
]==])

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${_project}" -B "${_build}"
    "-DPROBE_MODULE_DIR=${MODULE_DIR}"
    "-DPROBE_STAGE_DIR=${_stage}"
    "-DPROBE_GENERATED_DIR=${_generated}"
    "-DPROBE_GLSLANG_SOURCE=${_bin}/glslang"
    "-DPROBE_SPIRV_SOURCE=${_bin}/spirv-val"
    "-DPROBE_TARGET_COUNT=${_target_count}")
if(DEFINED GENERATOR AND NOT "${GENERATOR}" STREQUAL "")
    list(APPEND _configure_command -G "${GENERATOR}")
endif()
execute_process(COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "concurrent staging probe configure failed:\n${_configure_output}\n${_configure_error}")
endif()

function(_bloom_concurrent_stage_verify label out_failures)
    set(_local_failures "")
    foreach(_tool IN ITEMS "glslangValidator" "spirv-val")
        if(NOT EXISTS "${_stage}/${_tool}")
            list(APPEND _local_failures "${label}: staged ${_tool} is absent")
        endif()
    endforeach()
    if(NOT EXISTS "${_stage}/inventory.json")
        list(APPEND _local_failures "${label}: staged inventory.json is absent")
    else()
        file(READ "${_stage}/inventory.json" _json)
        foreach(_index RANGE 0 1)
            string(JSON _file GET "${_json}" "tools" ${_index} "file")
            string(JSON _recorded GET "${_json}" "tools" ${_index} "stagedSha256")
            if(NOT EXISTS "${_stage}/${_file}")
                list(APPEND _local_failures "${label}: inventory names missing ${_file}")
                continue()
            endif()
            file(SHA256 "${_stage}/${_file}" _actual)
            if(NOT _recorded STREQUAL "sha256:${_actual}")
                list(APPEND _local_failures
                    "${label}: staged hash for ${_file} disagrees with the staged bytes")
            endif()
        endforeach()
    endif()
    file(GLOB _staged_licenses "${_stage}/licenses/*")
    list(LENGTH _staged_licenses _staged_license_count)
    if(NOT _staged_license_count EQUAL ${_license_count})
        list(APPEND _local_failures
            "${label}: expected ${_license_count} staged licenses, found ${_staged_license_count}")
    endif()
    set(${out_failures} "${_local_failures}" PARENT_SCOPE)
endfunction()

foreach(_round RANGE 1 ${_rounds})
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_build}" --parallel ${_target_count}
        RESULT_VARIABLE _build_result
        OUTPUT_VARIABLE _build_output
        ERROR_VARIABLE _build_error)
    if(NOT _build_result EQUAL 0)
        message(FATAL_ERROR
            "concurrent staging round ${_round} failed:\n${_build_output}\n${_build_error}")
    endif()
    _bloom_concurrent_stage_verify("round ${_round}" _failures)
    if(_failures)
        message(FATAL_ERROR "concurrent staging verification failed:${_failures}")
    endif()
    message(STATUS "concurrent staging round ${_round}: ${_target_count} targets, "
        "${_license_count} licenses, inventory hashes valid")
endforeach()

if(NOT EXISTS "${_stage}.lock")
    message(FATAL_ERROR "concurrent staging did not use the destination-scoped lock ${_stage}.lock")
endif()

message(STATUS "GPU shader tools concurrent staging passed (${_rounds} repeated parallel builds "
    "into one destination)")
