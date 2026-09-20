# Offline verification of the BlendV1 SPIR-V embed recipe for every blend family.
#
# For each of blend, blend_f64, and blend_portable this script:
#   1. runs the C++ embed generator with --check against the checked-in .inc and manifest, which
#      recompiles the .comp with the pinned glslangValidator, validates it with the pinned
#      spirv-val, disassembles it with the pinned spirv-dis, and (for blend_portable) asserts no
#      Float64/Int64 capability or 64-bit type; and
#   2. regenerates the family twice into separate work directories and requires the .inc and the
#      manifest to be byte-identical.
#
# Invoked by the bloom_gpu_blend_spirv_verify target in tools/gpu-shaders/CMakeLists.txt.

foreach(required_variable
        BLOOM_BLEND_GENERATOR
        BLOOM_BLEND_GLSLANG
        BLOOM_BLEND_SPIRV_VAL
        BLOOM_BLEND_SPIRV_DIS
        BLOOM_BLEND_SHADER_DIR
        BLOOM_BLEND_WORK_DIR)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(bloom_blend_families blend blend_f64 blend_portable)
foreach(family IN LISTS bloom_blend_families)
    set(comp "${BLOOM_BLEND_SHADER_DIR}/${family}.comp")
    set(manifest "${BLOOM_BLEND_SHADER_DIR}/${family}.manifest")
    set(inc "${BLOOM_BLEND_SHADER_DIR}/${family}_spirv.inc")
    set(forbid_64bit "")
    if(family STREQUAL "blend_portable")
        set(forbid_64bit --forbid-64bit)
    endif()

    execute_process(
        COMMAND "${BLOOM_BLEND_GENERATOR}"
                --comp "${comp}"
                --manifest "${manifest}"
                --inc "${inc}"
                --glslang "${BLOOM_BLEND_GLSLANG}"
                --spirv-val "${BLOOM_BLEND_SPIRV_VAL}"
                --spirv-dis "${BLOOM_BLEND_SPIRV_DIS}"
                ${forbid_64bit}
                --check
        RESULT_VARIABLE check_result)
    if(NOT check_result EQUAL 0)
        message(FATAL_ERROR "blend SPIR-V --check failed for ${family}")
    endif()

    foreach(side a b)
        file(MAKE_DIRECTORY "${BLOOM_BLEND_WORK_DIR}/${family}/${side}")
        file(COPY "${manifest}" DESTINATION "${BLOOM_BLEND_WORK_DIR}/${family}/${side}")
        execute_process(
            COMMAND "${BLOOM_BLEND_GENERATOR}"
                    --comp "${comp}"
                    --manifest "${BLOOM_BLEND_WORK_DIR}/${family}/${side}/${family}.manifest"
                    --inc "${BLOOM_BLEND_WORK_DIR}/${family}/${side}/${family}_spirv.inc"
                    --glslang "${BLOOM_BLEND_GLSLANG}"
                    --spirv-val "${BLOOM_BLEND_SPIRV_VAL}"
                    --spirv-dis "${BLOOM_BLEND_SPIRV_DIS}"
                    ${forbid_64bit}
            RESULT_VARIABLE generate_result)
        if(NOT generate_result EQUAL 0)
            message(FATAL_ERROR "blend SPIR-V generation failed for ${family}/${side}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${BLOOM_BLEND_WORK_DIR}/${family}/a/${family}_spirv.inc"
                "${BLOOM_BLEND_WORK_DIR}/${family}/b/${family}_spirv.inc"
        RESULT_VARIABLE inc_compare)
    if(NOT inc_compare EQUAL 0)
        message(FATAL_ERROR "blend SPIR-V .inc generation is not deterministic for ${family}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${BLOOM_BLEND_WORK_DIR}/${family}/a/${family}.manifest"
                "${BLOOM_BLEND_WORK_DIR}/${family}/b/${family}.manifest"
        RESULT_VARIABLE manifest_compare)
    if(NOT manifest_compare EQUAL 0)
        message(FATAL_ERROR "blend manifest generation is not deterministic for ${family}")
    endif()
endforeach()

message(STATUS "blend SPIR-V embed verify passed for: ${bloom_blend_families}")
