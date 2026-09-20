# Serialized staging of the qualified GPU shader tools into an executable-relative directory.
#
# Several app/test targets can share one output directory (for example the runtime GPU tests all
# link beside src/runtime/tests), so their staging targets would otherwise copy the tools, license
# evidence, and inventory into the SAME destination concurrently. Cross-process directory copies
# are not safe to race: one target can observe the destination directory mid-recreate created by
# another and fail with "No such file or directory".
#
# Every invocation therefore takes an exclusive file(LOCK) keyed on the exact destination and then
# performs the whole stage install-and-relocate sequence while holding it. Concurrent targets in the
# same directory serialize; targets that stage into different directories never contend. The lock is
# released at the end, and also automatically if this process exits early.
#
# Invoked as:
#   cmake -DGPU_TOOLS_STAGE=<dir> -DGPU_TOOLS_GENERATED=<dir> \
#         -DGPU_TOOLS_GLSLANG_SOURCE=<path> -DGPU_TOOLS_GLSLANG_NAME=<name> \
#         -DGPU_TOOLS_SPIRV_SOURCE=<path> -DGPU_TOOLS_SPIRV_NAME=<name> \
#         [-DGPU_TOOLS_PRIVATE_COUNT=<n> -DGPU_TOOLS_PRIVATE_SOURCE_<i>=<path> \
#          -DGPU_TOOLS_PRIVATE_NAME_<i>=<name> -DGPU_TOOLS_PATCHELF=<path> \
#          -DGPU_TOOLS_RECORD_SCRIPT=<path>] \
#         -P BloomGpuShaderToolsStage.cmake

foreach(_required IN ITEMS
        GPU_TOOLS_STAGE GPU_TOOLS_GENERATED
        GPU_TOOLS_GLSLANG_SOURCE GPU_TOOLS_GLSLANG_NAME
        GPU_TOOLS_SPIRV_SOURCE GPU_TOOLS_SPIRV_NAME)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "BloomGpuShaderToolsStage.cmake requires -D${_required}=...")
    endif()
endforeach()

if(NOT DEFINED GPU_TOOLS_PRIVATE_COUNT OR "${GPU_TOOLS_PRIVATE_COUNT}" STREQUAL "")
    set(GPU_TOOLS_PRIVATE_COUNT 0)
endif()
if(NOT DEFINED GPU_TOOLS_LOCK_TIMEOUT OR "${GPU_TOOLS_LOCK_TIMEOUT}" STREQUAL "")
    set(GPU_TOOLS_LOCK_TIMEOUT 600)
endif()

set(_licenses_src "${GPU_TOOLS_GENERATED}/licenses")
if(NOT IS_DIRECTORY "${_licenses_src}")
    message(FATAL_ERROR
        "BloomGpuShaderToolsStage.cmake: generated license directory is absent: ${_licenses_src}")
endif()

# Create the destination before locking so the destination-scoped lock file always has a parent.
file(MAKE_DIRECTORY "${GPU_TOOLS_STAGE}")
set(_lock_path "${GPU_TOOLS_STAGE}.lock")
file(LOCK "${_lock_path}" GUARD PROCESS TIMEOUT "${GPU_TOOLS_LOCK_TIMEOUT}"
    RESULT_VARIABLE _lock_result)
if(NOT _lock_result EQUAL 0)
    message(FATAL_ERROR
        "BloomGpuShaderToolsStage.cmake: could not lock staging destination '${GPU_TOOLS_STAGE}' "
        "(result ${_lock_result}); refusing to stage concurrently.")
endif()

file(MAKE_DIRECTORY "${GPU_TOOLS_STAGE}/licenses")
file(COPY_FILE "${GPU_TOOLS_GLSLANG_SOURCE}"
    "${GPU_TOOLS_STAGE}/${GPU_TOOLS_GLSLANG_NAME}" ONLY_IF_DIFFERENT)
file(COPY_FILE "${GPU_TOOLS_SPIRV_SOURCE}"
    "${GPU_TOOLS_STAGE}/${GPU_TOOLS_SPIRV_NAME}" ONLY_IF_DIFFERENT)
# file(COPY <dir> DESTINATION <parent>) installs <dir> under <parent>, matching
# `cmake -E copy_directory <dir> <parent>/<dir>` without racing a shared destination.
file(COPY "${_licenses_src}" DESTINATION "${GPU_TOOLS_STAGE}")
file(COPY_FILE "${GPU_TOOLS_GENERATED}/inventory.json"
    "${GPU_TOOLS_STAGE}/inventory.json" ONLY_IF_DIFFERENT)

if(GPU_TOOLS_PRIVATE_COUNT GREATER 0)
    if(NOT DEFINED GPU_TOOLS_PATCHELF OR "${GPU_TOOLS_PATCHELF}" STREQUAL "")
        message(FATAL_ERROR
            "BloomGpuShaderToolsStage.cmake: private runtime libraries require -DGPU_TOOLS_PATCHELF")
    endif()
    if(NOT DEFINED GPU_TOOLS_RECORD_SCRIPT OR "${GPU_TOOLS_RECORD_SCRIPT}" STREQUAL "")
        message(FATAL_ERROR
            "BloomGpuShaderToolsStage.cmake: relocated tools require -DGPU_TOOLS_RECORD_SCRIPT")
    endif()
    math(EXPR _private_last "${GPU_TOOLS_PRIVATE_COUNT} - 1")
    foreach(_private_index RANGE 0 ${_private_last})
        set(_source_var "GPU_TOOLS_PRIVATE_SOURCE_${_private_index}")
        set(_name_var "GPU_TOOLS_PRIVATE_NAME_${_private_index}")
        if(NOT DEFINED ${_source_var} OR "${${_source_var}}" STREQUAL ""
                OR NOT DEFINED ${_name_var} OR "${${_name_var}}" STREQUAL "")
            message(FATAL_ERROR
                "BloomGpuShaderToolsStage.cmake: missing private runtime entry ${_private_index}")
        endif()
        file(COPY_FILE "${${_source_var}}"
            "${GPU_TOOLS_STAGE}/${${_name_var}}" ONLY_IF_DIFFERENT)
    endforeach()
    execute_process(
        COMMAND "${GPU_TOOLS_PATCHELF}" --set-rpath "$ORIGIN"
            "${GPU_TOOLS_STAGE}/${GPU_TOOLS_GLSLANG_NAME}"
            "${GPU_TOOLS_STAGE}/${GPU_TOOLS_SPIRV_NAME}"
        RESULT_VARIABLE _patchelf_tools_result
        ERROR_VARIABLE _patchelf_tools_error)
    if(NOT _patchelf_tools_result EQUAL 0)
        message(FATAL_ERROR
            "BloomGpuShaderToolsStage.cmake: patchelf failed on the staged tools: "
            "${_patchelf_tools_error}")
    endif()
    foreach(_private_index RANGE 0 ${_private_last})
        set(_name_var "GPU_TOOLS_PRIVATE_NAME_${_private_index}")
        execute_process(
            COMMAND "${GPU_TOOLS_PATCHELF}" --set-rpath "$ORIGIN"
                "${GPU_TOOLS_STAGE}/${${_name_var}}"
            RESULT_VARIABLE _patchelf_lib_result
            ERROR_VARIABLE _patchelf_lib_error)
        if(NOT _patchelf_lib_result EQUAL 0)
            message(FATAL_ERROR
                "BloomGpuShaderToolsStage.cmake: patchelf failed on ${${_name_var}}: "
                "${_patchelf_lib_error}")
        endif()
    endforeach()
    set(GPU_TOOLS_INVENTORY "${GPU_TOOLS_STAGE}/inventory.json")
    include("${GPU_TOOLS_RECORD_SCRIPT}")
endif()

file(LOCK "${_lock_path}" RELEASE)
