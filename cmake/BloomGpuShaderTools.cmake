# Bloom GPU shader-tool packaging, per docs/architecture/gpu-backend.md.
#
# bloom_package_gpu_shader_tools(<target>) stages glslangValidator and spirv-val into an
# executable-relative private directory (bundle-relative on macOS) for desktop/CLI/MCP shader
# compilation. It never consults the ambient PATH, never substitutes a host tool, and never embeds
# an absolute build path in a definition: a non-qualified mode, CPU stub, or missing prefix tool
# yields a typed unavailable definition and no files, with the CPU reference path as fallback.
# Validated every configure (no cached find_program) with realpath containment. ELF NEEDED names
# resolve through an explicit OS-floor allowlist, a recursive prefix-private copy, or a hard error.
# When relocation rewrites bytes, inventory records sourceSha256 and stagedSha256 separately.
# Staging runs through BloomGpuShaderToolsStage.cmake, which holds a destination-scoped file lock,
# so targets sharing one output directory (all the runtime GPU tests, for example) serialize instead
# of racing concurrent directory copies into the same executable-relative package.

include_guard(GLOBAL)

# Probe availability fresh on every configure without touching the ambient PATH. Outputs
# (PARENT_SCOPE): available, reason, and realpath-resolved glslangValidator / spirv-val.
function(bloom_gpu_shader_tools_probe)
    set(bloom_gpu_tools_available FALSE PARENT_SCOPE)
    set(bloom_gpu_tools_glslang "" PARENT_SCOPE)
    set(bloom_gpu_tools_spirv_val "" PARENT_SCOPE)

    if(NOT BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        set(bloom_gpu_tools_reason "unqualified-mode" PARENT_SCOPE)
        return()
    endif()
    if(DEFINED BLOOM_RENDER_ENABLE_VULKAN AND NOT BLOOM_RENDER_ENABLE_VULKAN)
        set(bloom_gpu_tools_reason "cpu-stub" PARENT_SCOPE)
        return()
    endif()
    if(NOT DEFINED BLOOM_DEPENDENCY_PREFIX OR NOT IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}")
        set(bloom_gpu_tools_reason "missing-prefix" PARENT_SCOPE)
        return()
    endif()

    file(REAL_PATH "${BLOOM_DEPENDENCY_PREFIX}" _prefix_real)
    set(_suffix "${CMAKE_EXECUTABLE_SUFFIX}")
    set(_resolved_glslang "")
    set(_resolved_spirv "")
    foreach(_probe IN ITEMS "glslangValidator>_resolved_glslang" "spirv-val>_resolved_spirv")
        string(REPLACE ">" ";" _probe_parts "${_probe}")
        list(GET _probe_parts 0 _tool_name)
        list(GET _probe_parts 1 _result_var)
        set(_candidate "${BLOOM_DEPENDENCY_PREFIX}/bin/${_tool_name}${_suffix}")
        # Re-checked every configure: no cached find_program result can keep a removed tool alive.
        if(NOT EXISTS "${_candidate}" OR IS_DIRECTORY "${_candidate}")
            set(bloom_gpu_tools_reason "missing-tools" PARENT_SCOPE)
            return()
        endif()
        file(REAL_PATH "${_candidate}" _tool_real)
        cmake_path(IS_PREFIX _prefix_real "${_tool_real}" NORMALIZE _under_prefix)
        if(NOT _under_prefix)
            set(bloom_gpu_tools_reason "tool-outside-prefix" PARENT_SCOPE)
            return()
        endif()
        set(${_result_var} "${_tool_real}")
    endforeach()

    set(bloom_gpu_tools_available TRUE PARENT_SCOPE)
    set(bloom_gpu_tools_reason "available" PARENT_SCOPE)
    set(bloom_gpu_tools_glslang "${_resolved_glslang}" PARENT_SCOPE)
    set(bloom_gpu_tools_spirv_val "${_resolved_spirv}" PARENT_SCOPE)
endfunction()

# Explicit Linux dynamic-loader floor, defined inside the predicate so it is independent of the
# including directory scope. A NEEDED name off this list and absent from the prefix is an error.
function(_bloom_gpu_tools_is_os_floor soname out_var)
    set(_floor
        "libc.so.6"
        "libm.so.6"
        "libpthread.so.0"
        "libdl.so.2"
        "librt.so.1"
        "libresolv.so.2"
        "libutil.so.1"
        "libnsl.so.1"
        "libgcc_s.so.1"
        "libstdc++.so.6"
        "libatomic.so.1"
        "libgomp.so.1"
        "ld-linux-x86-64.so.2"
        "ld-linux-aarch64.so.1")
    if(soname IN_LIST _floor)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Classify one NEEDED name: kind floor|private|unresolved and the prefix path when private.
function(_bloom_gpu_tools_classify_needed prefix soname out_kind out_path)
    set(_kind "unresolved")
    set(_path "")
    _bloom_gpu_tools_is_os_floor("${soname}" _is_floor)
    if(_is_floor)
        set(_kind "floor")
    elseif(EXISTS "${prefix}/lib/${soname}")
        set(_kind "private")
        set(_path "${prefix}/lib/${soname}")
    endif()
    set(${out_kind} "${_kind}" PARENT_SCOPE)
    set(${out_path} "${_path}" PARENT_SCOPE)
endfunction()

# Recursively resolve a tool's private runtime closure: skip floor, collect/enqueue prefix names,
# fail on unknown. Outputs PARENT_SCOPE realpath and soname lists (deduplicated).
function(_bloom_gpu_tools_private_runtime prefix tool_path out_paths out_names)
    set(_paths "")
    set(_names "")
    if(UNIX AND NOT APPLE)
        find_program(_bloom_readelf NAMES readelf llvm-readelf HINTS /usr/bin /usr/local/bin)
        if(NOT _bloom_readelf)
            message(FATAL_ERROR
                "Bloom GPU shader tools: readelf is required to inspect ${tool_path} linked "
                "dependencies; refusing to guess whether a private runtime library is needed.")
        endif()
        file(REAL_PATH "${prefix}" _prefix_real)
        set(_queue "${tool_path}")
        set(_seen "")
        while(_queue)
            list(POP_FRONT _queue _object)
            execute_process(
                COMMAND "${_bloom_readelf}" -d "${_object}"
                RESULT_VARIABLE _readelf_result
                OUTPUT_VARIABLE _readelf_output
                ERROR_VARIABLE _readelf_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _readelf_result EQUAL 0)
                message(FATAL_ERROR
                    "Bloom GPU shader tools: readelf failed on ${_object}: ${_readelf_error}")
            endif()
            string(REGEX MATCHALL "Shared library: \\[([^]]+)\\]" _needed_matches
                "${_readelf_output}")
            foreach(_needed IN LISTS _needed_matches)
                string(REGEX REPLACE "Shared library: \\[([^]]+)\\]" "\\1" _needed_name "${_needed}")
                if(_needed_name IN_LIST _seen)
                    continue()
                endif()
                list(APPEND _seen "${_needed_name}")
                _bloom_gpu_tools_classify_needed("${prefix}" "${_needed_name}"
                    _kind _path)
                if(_kind STREQUAL "unresolved")
                    message(FATAL_ERROR
                        "Bloom GPU shader tools: ${_object} needs '${_needed_name}', which is "
                        "neither an allowlisted operating-system floor library nor a library under "
                        "${prefix}/lib; refusing to depend on an accidental ambient library.")
                elseif(_kind STREQUAL "private")
                    file(REAL_PATH "${_path}" _path_real)
                    cmake_path(IS_PREFIX _prefix_real "${_path_real}" NORMALIZE _under_prefix)
                    if(NOT _under_prefix)
                        message(FATAL_ERROR
                            "Bloom GPU shader tools: private library ${_path} resolves outside the "
                            "qualified prefix (${_path_real}); refusing to copy a symlink-escaped "
                            "dependency.")
                    endif()
                    list(APPEND _paths "${_path_real}")
                    list(APPEND _names "${_needed_name}")
                    list(APPEND _queue "${_path_real}")
                endif()
            endforeach()
        endwhile()
        if(_paths)
            list(REMOVE_DUPLICATES _paths)
            list(REMOVE_DUPLICATES _names)
        endif()
    endif()
    set(${out_paths} "${_paths}" PARENT_SCOPE)
    set(${out_names} "${_names}" PARENT_SCOPE)
endfunction()

# Record one component version from its reviewed lock input, for the inventory only. Empty when
# the record is absent; the lock remains the authority and is never re-serialized here.
function(_bloom_gpu_tools_component_version component out_var)
    set(_lock "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../dependencies/lock-components/${component}.json")
    set(_version "")
    if(EXISTS "${_lock}")
        file(READ "${_lock}" _lock_json)
        string(JSON _version ERROR_VARIABLE _ignored GET "${_lock_json}" "version")
    endif()
    set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()

# Stage reviewed license and notice evidence for both pinned components beside the tools.
function(_bloom_gpu_tools_collect_evidence out_pairs)
    set(_pairs "")
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../dependencies/licenses")
    file(GLOB _glslang_license_files "${_root}/glslang/LICENSE.txt"
         "${_root}/glslang/LICENSES/*.txt")
    file(GLOB _spirv_license_files "${_root}/spirv-tools/LICENSE"
         "${_root}/spirv-tools/*-LICENSE")
    foreach(_pair_file IN LISTS _glslang_license_files _spirv_license_files)
        get_filename_component(_file_name "${_pair_file}" NAME)
        if(_pair_file MATCHES "/glslang/")
            set(_staged_name "glslang-${_file_name}")
        else()
            set(_staged_name "spirv-tools-${_file_name}")
        endif()
        list(APPEND _pairs "${_pair_file}>${_staged_name}")
    endforeach()
    foreach(_reviewed IN ITEMS review.md security.md)
        list(APPEND _pairs "${_root}/glslang/${_reviewed}>glslang-${_reviewed}")
        list(APPEND _pairs "${_root}/spirv-tools/${_reviewed}>spirv-tools-${_reviewed}")
    endforeach()
    set(${out_pairs} "${_pairs}" PARENT_SCOPE)
endfunction()

# Render a CMake list as comma-separated JSON string literals (empty list -> empty string).
function(_bloom_gpu_tools_json_array items out_var)
    set(_rendered "")
    foreach(_item IN LISTS items)
        if(_rendered STREQUAL "")
            set(_rendered "\"${_item}\"")
        else()
            string(APPEND _rendered ",\"${_item}\"")
        endif()
    endforeach()
    set(${out_var} "${_rendered}" PARENT_SCOPE)
endfunction()

# bloom_package_gpu_shader_tools(<target>) attaches staging to <target>, creates the ALL target
# <target>-gpu-shader-tools, adds the install rules for the active platform, and publishes the
# relative-layout compile definitions. Callers are app targets; this function owns no process,
# no compiler invocation, and no runtime resolution policy.
function(bloom_package_gpu_shader_tools target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "bloom_package_gpu_shader_tools: unknown target '${target}'")
    endif()

    set(_dir "bloom-gpu-tools")
    # Drop the historical cached find_program results so no stale path survives a prefix change.
    unset(BLOOM_GPU_TOOLS_GLSLANG_EXECUTABLE CACHE)
    unset(BLOOM_GPU_TOOLS_SPIRV_VAL_EXECUTABLE CACHE)
    bloom_gpu_shader_tools_probe()
    set(BLOOM_GPU_SHADER_TOOLS_AVAILABLE "${bloom_gpu_tools_available}" CACHE INTERNAL
        "GPU shader tools staged for at least one runtime target" FORCE)
    set(BLOOM_GPU_SHADER_TOOLS_REASON "${bloom_gpu_tools_reason}" CACHE INTERNAL
        "GPU shader tools availability reason for diagnostics and tests" FORCE)

    if(NOT bloom_gpu_tools_available)
        target_compile_definitions(${target} PRIVATE
            BLOOM_GPU_TOOLS_AVAILABLE=0
            BLOOM_GPU_TOOLS_DIR="${_dir}"
            BLOOM_GPU_TOOLS_LAYOUT="executable-relative"
            BLOOM_GPU_TOOLS_UNAVAILABLE_REASON="${bloom_gpu_tools_reason}")
        message(STATUS "Bloom GPU shader tools: '${target}' disabled "
            "(${bloom_gpu_tools_reason}); the CPU reference path remains supported.")
        return()
    endif()

    set(_suffix "${CMAKE_EXECUTABLE_SUFFIX}")
    set(_glslang_name "glslangValidator${_suffix}")
    set(_spirv_name "spirv-val${_suffix}")
    set(_stage "$<TARGET_FILE_DIR:${target}>/${_dir}")
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/${target}-gpu-shader-tools")
    file(MAKE_DIRECTORY "${_generated}/licenses")

    _bloom_gpu_tools_private_runtime("${BLOOM_DEPENDENCY_PREFIX}"
        "${bloom_gpu_tools_glslang}" _glslang_private_paths _glslang_private_names)
    _bloom_gpu_tools_private_runtime("${BLOOM_DEPENDENCY_PREFIX}"
        "${bloom_gpu_tools_spirv_val}" _spirv_private_paths _spirv_private_names)
    set(_all_private_paths ${_glslang_private_paths} ${_spirv_private_paths})
    set(_all_private_names ${_glslang_private_names} ${_spirv_private_names})
    if(_all_private_paths)
        list(REMOVE_DUPLICATES _all_private_paths)
        list(REMOVE_DUPLICATES _all_private_names)
    endif()
    if(_all_private_names)
        set(_relocated TRUE)
        set(_relocated_json "true")
    else()
        set(_relocated FALSE)
        set(_relocated_json "false")
    endif()

    file(SHA256 "${bloom_gpu_tools_glslang}" _glslang_source_sha256)
    file(SHA256 "${bloom_gpu_tools_spirv_val}" _spirv_source_sha256)
    if(_relocated)
        # The build step rewrites these sentinels with the actual post-patchelf digest, keeping
        # staged identity distinct from the recorded source identity.
        set(_glslang_staged_sha256 "PENDING-${_glslang_name}")
        set(_spirv_staged_sha256 "PENDING-${_spirv_name}")
    else()
        set(_glslang_staged_sha256 "${_glslang_source_sha256}")
        set(_spirv_staged_sha256 "${_spirv_source_sha256}")
    endif()
    _bloom_gpu_tools_component_version("glslang" _glslang_version)
    _bloom_gpu_tools_component_version("spirv-tools" _spirv_version)
    _bloom_gpu_tools_collect_evidence(_evidence_pairs)
    set(_evidence_staged_names "")
    foreach(_pair IN LISTS _evidence_pairs)
        string(REPLACE ">" ";" _pair_parts "${_pair}")
        list(GET _pair_parts 0 _evidence_source)
        list(GET _pair_parts 1 _evidence_name)
        list(APPEND _evidence_staged_names "licenses/${_evidence_name}")
        file(COPY_FILE "${_evidence_source}" "${_generated}/licenses/${_evidence_name}")
    endforeach()

    _bloom_gpu_tools_json_array("${_glslang_private_names}" _glslang_runtime_json)
    _bloom_gpu_tools_json_array("${_spirv_private_names}" _spirv_runtime_json)
    _bloom_gpu_tools_json_array("${_evidence_staged_names}" _licenses_json)
    string(CONCAT _inventory
        "{\"format\":\"org.kinetik.bloom.gpu-shader-tools.inventory\",\"version\":1,"
        "\"layout\":\"executable-relative\",\"directory\":\"${_dir}\",\"relocated\":"
        "${_relocated_json},\"tools\":["
        "{\"name\":\"glslangValidator\",\"file\":\"${_glslang_name}\","
        "\"component\":\"glslang\",\"componentVersion\":\"${_glslang_version}\","
        "\"source\":\"bin/glslangValidator\","
        "\"sourceSha256\":\"sha256:${_glslang_source_sha256}\","
        "\"stagedSha256\":\"sha256:${_glslang_staged_sha256}\","
        "\"runtimeLibraries\":[${_glslang_runtime_json}]},"
        "{\"name\":\"spirv-val\",\"file\":\"${_spirv_name}\","
        "\"component\":\"spirv-tools\",\"componentVersion\":\"${_spirv_version}\","
        "\"source\":\"bin/spirv-val\","
        "\"sourceSha256\":\"sha256:${_spirv_source_sha256}\","
        "\"stagedSha256\":\"sha256:${_spirv_staged_sha256}\","
        "\"runtimeLibraries\":[${_spirv_runtime_json}]}],"
        "\"licenses\":[${_licenses_json}]}\n")
    file(WRITE "${_generated}/inventory.json" "${_inventory}")

    # All staging runs through one helper that holds a destination-scoped file lock, so targets
    # which share an output directory (all the runtime GPU tests, for example) serialize instead of
    # racing concurrent directory copies into the same executable-relative package.
    set(_stage_args
        "${CMAKE_COMMAND}"
        "-DGPU_TOOLS_STAGE=${_stage}"
        "-DGPU_TOOLS_GENERATED=${_generated}"
        "-DGPU_TOOLS_GLSLANG_SOURCE=${bloom_gpu_tools_glslang}"
        "-DGPU_TOOLS_GLSLANG_NAME=${_glslang_name}"
        "-DGPU_TOOLS_SPIRV_SOURCE=${bloom_gpu_tools_spirv_val}"
        "-DGPU_TOOLS_SPIRV_NAME=${_spirv_name}")
    list(LENGTH _all_private_paths _private_count)
    if(_private_count GREATER 0)
        math(EXPR _private_last "${_private_count} - 1")
        foreach(_private_index RANGE 0 ${_private_last})
            list(GET _all_private_paths ${_private_index} _private_path)
            list(GET _all_private_names ${_private_index} _private_name)
            list(APPEND _stage_args
                "-DGPU_TOOLS_PRIVATE_SOURCE_${_private_index}=${_private_path}"
                "-DGPU_TOOLS_PRIVATE_NAME_${_private_index}=${_private_name}")
        endforeach()
        find_program(BLOOM_GPU_TOOLS_PATCHELF NAMES patchelf)
        if(NOT BLOOM_GPU_TOOLS_PATCHELF)
            message(FATAL_ERROR
                "Bloom GPU shader tools: the pinned tools need private runtime libraries "
                "(${_all_private_names}) but patchelf was not found to set an $ORIGIN rpath.")
        endif()
        _bloom_gpu_tools_write_relocation_script("${_generated}/record-staged-hash.cmake"
            "${_glslang_name}" "${_spirv_name}")
        list(APPEND _stage_args
            "-DGPU_TOOLS_PRIVATE_COUNT=${_private_count}"
            "-DGPU_TOOLS_PATCHELF=${BLOOM_GPU_TOOLS_PATCHELF}"
            "-DGPU_TOOLS_RECORD_SCRIPT=${_generated}/record-staged-hash.cmake")
    else()
        list(APPEND _stage_args "-DGPU_TOOLS_PRIVATE_COUNT=0")
    endif()

    add_custom_target(${target}-gpu-shader-tools ALL
        COMMAND ${_stage_args}
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BloomGpuShaderToolsStage.cmake"
        COMMENT "Staging the qualified GPU shader tools beside ${target}"
        VERBATIM)
    add_dependencies(${target} ${target}-gpu-shader-tools)

    get_target_property(_is_bundle ${target} MACOSX_BUNDLE)
    if(APPLE AND _is_bundle)
        set(_install_dir "$<TARGET_BUNDLE_DIR_NAME:${target}>/Contents/MacOS/${_dir}")
    else()
        set(_install_dir "bin/${_dir}")
    endif()
    install(PROGRAMS
        "$<TARGET_FILE_DIR:${target}>/${_dir}/${_glslang_name}"
        "$<TARGET_FILE_DIR:${target}>/${_dir}/${_spirv_name}"
        DESTINATION "${_install_dir}")
    install(FILES
        "$<TARGET_FILE_DIR:${target}>/${_dir}/inventory.json"
        DESTINATION "${_install_dir}")
    set(_evidence_install "")
    foreach(_evidence_name IN LISTS _evidence_staged_names)
        list(APPEND _evidence_install
            "$<TARGET_FILE_DIR:${target}>/${_dir}/${_evidence_name}")
    endforeach()
    install(FILES ${_evidence_install} DESTINATION "${_install_dir}/licenses")
    if(_all_private_names)
        set(_private_install "")
        foreach(_private_name IN LISTS _all_private_names)
            list(APPEND _private_install
                "$<TARGET_FILE_DIR:${target}>/${_dir}/${_private_name}")
        endforeach()
        install(FILES ${_private_install} DESTINATION "${_install_dir}")
    endif()

    target_compile_definitions(${target} PRIVATE
        BLOOM_GPU_TOOLS_AVAILABLE=1
        BLOOM_GPU_TOOLS_DIR="${_dir}"
        BLOOM_GPU_TOOLS_LAYOUT="executable-relative"
        BLOOM_GPU_TOOLS_INVENTORY_NAME="inventory.json"
        BLOOM_GPU_TOOLS_RELOCATED=$<BOOL:${_relocated}>
        BLOOM_GPU_TOOLS_GLSLANG_NAME="${_glslang_name}"
        BLOOM_GPU_TOOLS_SPIRV_VAL_NAME="${_spirv_name}"
        BLOOM_GPU_TOOLS_GLSLANG_SOURCE_SHA256="sha256:${_glslang_source_sha256}"
        BLOOM_GPU_TOOLS_SPIRV_VAL_SOURCE_SHA256="sha256:${_spirv_source_sha256}")
    if(NOT _relocated)
        # Staged bytes equal the source bytes, so a compile-time staged pin is truthful.
        target_compile_definitions(${target} PRIVATE
            BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256="sha256:${_glslang_staged_sha256}"
            BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256="sha256:${_spirv_staged_sha256}")
    else()
        target_compile_definitions(${target} PRIVATE
            BLOOM_GPU_TOOLS_STAGED_IDENTITY_IN_INVENTORY=1)
    endif()
    if(APPLE AND _is_bundle)
        target_compile_definitions(${target} PRIVATE
            BLOOM_GPU_TOOLS_BUNDLE_RELATIVE=1)
    endif()
    message(STATUS "Bloom GPU shader tools: '${target}' stages ${_glslang_name} and ${_spirv_name} "
        "in ${_dir}/ (relocated=${_relocated}; private runtime: ${_all_private_names})")
endfunction()

# Emit the build-time script that records the actual staged digest after relocation. It rewrites
# only the stagedSha256 fields so inventory always states the bytes that will run, distinct from
# the recorded source identity.
function(_bloom_gpu_tools_write_relocation_script script_path glslang_name spirv_name)
    string(CONCAT _script
        "# Generated by BloomGpuShaderTools.cmake. Records staged identity after patchelf.\n"
        "if(NOT DEFINED GPU_TOOLS_STAGE OR NOT DEFINED GPU_TOOLS_INVENTORY)\n"
        "    message(FATAL_ERROR \"record-staged-hash.cmake requires STAGE and INVENTORY\")\n"
        "endif()\n"
        "file(READ \"\${GPU_TOOLS_INVENTORY}\" _json)\n"
        "file(SHA256 \"\${GPU_TOOLS_STAGE}/${glslang_name}\" _glslang_sha)\n"
        "string(REPLACE \"sha256:PENDING-${glslang_name}\" \"sha256:\${_glslang_sha}\""
        " _json \"\${_json}\")\n"
        "file(SHA256 \"\${GPU_TOOLS_STAGE}/${spirv_name}\" _spirv_sha)\n"
        "string(REPLACE \"sha256:PENDING-${spirv_name}\" \"sha256:\${_spirv_sha}\""
        " _json \"\${_json}\")\n"
        "file(WRITE \"\${GPU_TOOLS_INVENTORY}\" \"\${_json}\")\n")
    file(WRITE "${script_path}" "${_script}")
endfunction()
