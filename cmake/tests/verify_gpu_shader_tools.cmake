# Verify a staged bloom-gpu-tools directory against the qualified dependency prefix.
#
# Invoked as: cmake -DGPU_TOOLS_STAGED=<dir> -DGPU_TOOLS_PREFIX_BIN=<prefix/bin> \
#                  -DGPU_TOOLS_GLSLANG=glslangValidator -DGPU_TOOLS_SPIRV=spirv-val \
#                  -P verify_gpu_shader_tools.cmake
#
# Checks the executable-relative layout and the generated inventory, distinguishing the recorded
# source identity from the staged identity (which may differ after $ORIGIN relocation). It then
# compiles and validates a tiny compute shader using ONLY the staged executables with PATH and the
# dynamic-loader environment scrubbed, and rejects absolute NEEDED/RPATH/RUNPATH entries.

foreach(required IN ITEMS GPU_TOOLS_STAGED GPU_TOOLS_PREFIX_BIN GPU_TOOLS_GLSLANG GPU_TOOLS_SPIRV)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "verify_gpu_shader_tools.cmake requires -D${required}=...")
    endif()
endforeach()

set(_glslang "${GPU_TOOLS_STAGED}/${GPU_TOOLS_GLSLANG}")
set(_spirv "${GPU_TOOLS_STAGED}/${GPU_TOOLS_SPIRV}")
set(_failures "")

set(_inventory_path "${GPU_TOOLS_STAGED}/inventory.json")
if(NOT EXISTS "${_inventory_path}")
    list(APPEND _failures "inventory.json is absent from the staged directory")
else()
    file(READ "${_inventory_path}" _inventory)
    string(JSON _layout GET "${_inventory}" "layout")
    if(NOT _layout STREQUAL "executable-relative")
        list(APPEND _failures "inventory layout is not executable-relative")
    endif()
    string(JSON _count ERROR_VARIABLE _ignored LENGTH "${_inventory}" "tools")
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last})
        string(JSON _name GET "${_inventory}" "tools" ${_index} "name")
        string(JSON _file GET "${_inventory}" "tools" ${_index} "file")
        string(JSON _source GET "${_inventory}" "tools" ${_index} "source")
        string(JSON _source_sha GET "${_inventory}" "tools" ${_index} "sourceSha256")
        string(JSON _staged_sha GET "${_inventory}" "tools" ${_index} "stagedSha256")
        set(_expected_name "")
        if(_name STREQUAL "glslangValidator")
            set(_expected_name "${GPU_TOOLS_GLSLANG}")
        elseif(_name STREQUAL "spirv-val")
            set(_expected_name "${GPU_TOOLS_SPIRV}")
        endif()
        if(NOT _file STREQUAL _expected_name)
            list(APPEND _failures "inventory file for ${_name} is '${_file}'")
        endif()
        if(_source MATCHES "^/")
            list(APPEND _failures "inventory embeds an absolute source path for ${_name}")
        endif()
        string(LENGTH "${_staged_sha}" _staged_sha_length)
        if(NOT _staged_sha MATCHES "^sha256:[0-9a-f]+$" OR NOT _staged_sha_length EQUAL 71)
            list(APPEND _failures "inventory stagedSha256 for ${_name} is not a sha256 digest")
        endif()
        set(_staged_file "${GPU_TOOLS_STAGED}/${_file}")
        if(EXISTS "${_staged_file}")
            file(SHA256 "${_staged_file}" _measured)
            if(NOT _staged_sha STREQUAL "sha256:${_measured}")
                list(APPEND _failures
                    "inventory stagedSha256 for ${_name} disagrees with the staged bytes")
            endif()
        else()
            list(APPEND _failures "inventory names a missing staged file: ${_staged_file}")
        endif()
        set(_source_file "${GPU_TOOLS_PREFIX_BIN}/${_file}")
        if(EXISTS "${_source_file}")
            file(SHA256 "${_source_file}" _measured_source)
            if(NOT _source_sha STREQUAL "sha256:${_measured_source}")
                list(APPEND _failures
                    "inventory sourceSha256 for ${_name} disagrees with the prefix bytes")
            endif()
        else()
            list(APPEND _failures "prefix source tool missing: ${_source_file}")
        endif()
    endforeach()
    string(JSON _licenses_count ERROR_VARIABLE _ignored LENGTH "${_inventory}" "licenses")
    if(_licenses_count LESS 1)
        list(APPEND _failures "inventory records no license evidence")
    endif()
endif()

if(_failures)
    foreach(_failure IN LISTS _failures)
        message(FATAL_ERROR "GPU shader tools verification: ${_failure}")
    endforeach()
endif()

set(_workspace "${CMAKE_CURRENT_BINARY_DIR}/gpu-tools-verify")
file(MAKE_DIRECTORY "${_workspace}")
file(WRITE "${_workspace}/tiny.comp"
    "#version 450\nlayout(local_size_x = 1, local_size_y = 1) in;\nvoid main() {}\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=PATH --unset=LD_LIBRARY_PATH --unset=DYLD_LIBRARY_PATH
            "${_glslang}" --target-env vulkan1.2 -V "${_workspace}/tiny.comp"
            -o "${_workspace}/tiny.spv"
    WORKING_DIRECTORY "${_workspace}"
    RESULT_VARIABLE _compile_result
    OUTPUT_VARIABLE _compile_output
    ERROR_VARIABLE _compile_error)
if(NOT _compile_result EQUAL 0 OR NOT EXISTS "${_workspace}/tiny.spv")
    message(FATAL_ERROR
        "staged ${GPU_TOOLS_GLSLANG} could not compile a tiny shader with a scrubbed PATH:\n"
        "${_compile_output}\n${_compile_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=PATH --unset=LD_LIBRARY_PATH --unset=DYLD_LIBRARY_PATH
            "${_spirv}" --target-env vulkan1.2 "${_workspace}/tiny.spv"
    WORKING_DIRECTORY "${_workspace}"
    RESULT_VARIABLE _validate_result
    OUTPUT_VARIABLE _validate_output
    ERROR_VARIABLE _validate_error)
if(NOT _validate_result EQUAL 0)
    message(FATAL_ERROR
        "staged ${GPU_TOOLS_SPIRV} rejected the compiled SPIR-V with a scrubbed PATH:\n"
        "${_validate_output}\n${_validate_error}")
endif()

# Inspect dynamic metadata without trusting the host loader: no absolute NEEDED entry and no
# absolute RPATH/RUNPATH, so the staged tool resolves from OS-floor sonames and its own $ORIGIN.
find_program(_readelf NAMES readelf llvm-readelf)
if(_readelf)
    foreach(_tool IN ITEMS "${_glslang}" "${_spirv}")
        execute_process(COMMAND "${_readelf}" -d "${_tool}"
            RESULT_VARIABLE _readelf_result OUTPUT_VARIABLE _readelf_output)
        if(NOT _readelf_result EQUAL 0)
            continue()
        endif()
        if(_readelf_output MATCHES "Shared library: \\[/")
            message(FATAL_ERROR "staged ${_tool} has an absolute NEEDED entry")
        endif()
        string(REGEX MATCHALL "(RPATH|RUNPATH)[^]]*\\[([^]]+)\\]" _rpath_matches "${_readelf_output}")
        foreach(_rpath IN LISTS _rpath_matches)
            string(REGEX REPLACE "(RPATH|RUNPATH)[^]]*\\[([^]]+)\\]" "\\2" _entries "${_rpath}")
            string(REPLACE ":" ";" _entries "${_entries}")
            foreach(_entry IN LISTS _entries)
                if(_entry MATCHES "^/" OR (NOT _entry STREQUAL "$ORIGIN" AND NOT _entry MATCHES "^\\$ORIGIN/"))
                    message(FATAL_ERROR
                        "staged ${_tool} has a non-relocatable RPATH/RUNPATH entry '${_entry}'")
                endif()
            endforeach()
        endforeach()
    endforeach()
endif()

message(STATUS "GPU shader tools verified: ${GPU_TOOLS_STAGED} compiles and validates "
    "with a scrubbed PATH")
