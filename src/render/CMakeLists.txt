# --- bloom_render_fonts: the Qt-free runtime font the CPU text path rasterizes -----------------
#
# Configure-time embed of the already-vendored DejaVu Sans Book and Inter faces into C++ byte arrays,
# mirroring src/color/CMakeLists.txt's embed of the Bloom Neutral v1 OCIO config. src/render may not
# use Qt, and an evaluator must not read media from the filesystem, so the bytes have to be build-time
# constants rather than a Qt resource or a runtime file open. No second copy of a font is checked in:
# this reads the exact same files src/ui registers with Qt, whose provenance and per-file SHA-256 are
# recorded in their third-party provenance records. Reading checked-in assets at configure time is not
# a module dependency -- no src/render translation unit includes a src/ui header, and bloom_render
# links nothing from src/ui.
set(bloom_embedded_face_names DejaVuSans InterRegular InterMedium InterSemiBold)
set(
    bloom_embedded_face_assets
    "${CMAKE_CURRENT_SOURCE_DIR}/../ui/kit/third_party/dejavu-sans/DejaVuSans.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ui/kit/third_party/inter/Inter-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ui/kit/third_party/inter/Inter-Medium.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ui/kit/third_party/inter/Inter-SemiBold.ttf"
)
set(bloom_embedded_face_expected_bytes 757076 411640 417300 419744)

list(LENGTH bloom_embedded_face_names bloom_embedded_face_count)
math(EXPR bloom_embedded_face_last "${bloom_embedded_face_count} - 1")
foreach(face_index RANGE ${bloom_embedded_face_last})
    list(GET bloom_embedded_face_names ${face_index} face_name)
    list(GET bloom_embedded_face_assets ${face_index} face_asset)
    list(GET bloom_embedded_face_expected_bytes ${face_index} expected_byte_count)
    string(TOUPPER "${face_name}" face_upper_name)
    cmake_path(NORMAL_PATH face_asset)
    file(READ "${face_asset}" face_hex HEX)
    string(LENGTH "${face_hex}" face_hex_length)
    math(EXPR actual_byte_count "${face_hex_length} / 2")
    # Each asset's exact byte count is pinned so a silent font swap cannot reach the embed
    # unnoticed. A genuine face change is a provenance change: update the asset's provenance.md
    # and this number together, deliberately, rather than letting a different font ship as if it
    # were this one.
    if(NOT actual_byte_count EQUAL expected_byte_count)
        message(
            FATAL_ERROR
            "${face_asset} is expected to be exactly ${expected_byte_count} bytes "
            "(found ${actual_byte_count}); its bytes and SHA-256 are recorded in the asset's "
            "provenance.md. If the face genuinely changed, update that record and this count "
            "together."
        )
    endif()
    string(
        REGEX REPLACE
        "([0-9a-f][0-9a-f])"
        "0x\\1,"
        face_bytes
        "${face_hex}"
    )
    # Wrap at 16 bytes per line. CMake's regex has no brace repetition and only nine backreferences,
    # so the group is spelled out once and captured whole; a trailing partial group simply stays on
    # the last line. Without this the generated file would be one multi-megabyte line.
    string(
        REGEX REPLACE
        "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)"
        "\\1\n"
        face_payload_bytes
        "${face_bytes}"
    )
    set(BLOOM_${face_upper_name}_BYTE_COUNT "${actual_byte_count}")
    set(BLOOM_${face_upper_name}_PAYLOAD_BYTES "${face_payload_bytes}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${face_asset}")
endforeach()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/embedded_fonts.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_fonts.generated.cpp"
    @ONLY
)

add_library(
    bloom_render_fonts
    OBJECT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_fonts.generated.cpp"
)
target_compile_features(bloom_render_fonts PUBLIC cxx_std_20)
target_include_directories(bloom_render_fonts PUBLIC include "${CMAKE_CURRENT_SOURCE_DIR}/../core/include")
bloom_enable_warnings(bloom_render_fonts)

# --- bloom_render_stb_truetype: the one vendored translation unit ------------------------------
#
# Exists only to scope the vendored header's warning suppression to a single translation unit, which
# is why it deliberately does NOT call bloom_enable_warnings(): that function is also what attaches
# clang-tidy (warnings-as-errors) to a target, and clang-tidy's header filter covers everything under
# src/. Bloom's own text layout code lives in text_raster.cpp, inside bloom_render, under the full
# strict-warning and clang-tidy profile. The strict floating-point flags the rest of src/render uses
# ARE applied here, so glyph coverage stays reproducible build to build.
add_library(bloom_render_stb_truetype OBJECT text_glyph_source.cpp text_glyph_source.hpp)
target_compile_features(bloom_render_stb_truetype PUBLIC cxx_std_20)
target_include_directories(
    bloom_render_stb_truetype
    PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../core/include"
)
if(MSVC)
    target_compile_options(bloom_render_stb_truetype PRIVATE /w)
else()
    target_compile_options(bloom_render_stb_truetype PRIVATE -w)
endif()
bloom_enable_strict_floating_point(bloom_render_stb_truetype)

# --- Vulkan GPU device bootstrap: optional, exact-prefix, CPU stub fallback ---------------------
#
# The Vulkan device surface is an optional accelerated capability, not a build requirement. When the
# locked prefix provides Vulkan-Headers and Vulkan Memory Allocator, the Vulkan bootstrap is
# compiled; otherwise src/render compiles the portable CPU-unavailable stub with the exact same
# Bloom-owned API, so a build without any GPU dependency still links and the CPU reference path
# remains the correctness oracle.
#
# Qualified mode never falls back to a host package: detection searches only BLOOM_DEPENDENCY_PREFIX
# with NO_DEFAULT_PATH. developer-system mode may resolve host packages for local iteration and is
# labeled Unqualified. The governed bloom_find_dependency() helper is called only after a QUIET
# availability check has proven both package targets exist.
option(BLOOM_RENDER_ENABLE_VULKAN
    "Compile the Vulkan GPU device bootstrap when the locked prefix provides Vulkan headers and VMA"
    ON)

# --- Fixed Bloom Neutral v1 display shader: source pin and regeneration check -------------------
#
# The compute shader is an offline artifact from tools/gpu-shaders. Its source text is pinned by
# SHA-256 here, the manifest must bind that same source digest, and, when the locked glslangValidator
# and spirv-val are present, the configure step recompiles the checked-in .comp and confirms the
# resulting SPIR-V digest matches the digest embedded in neutral_display_spirv.inc. That closes the
# source -> SPIR-V -> embedded-array relationship; runtime code never compiles or loads a shader.
set(BLOOM_NEUTRAL_DISPLAY_COMP
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/neutral_display.comp")
set(BLOOM_NEUTRAL_DISPLAY_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/neutral_display.manifest")
set(BLOOM_NEUTRAL_DISPLAY_INC
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/neutral_display_spirv.inc")
set(BLOOM_NEUTRAL_DISPLAY_COMP_SHA256
    "63fa4699a9a6980312d34ad7c0c7f6ad5d20b7c249b0e050942a50baac367057")
set(BLOOM_NEUTRAL_DISPLAY_SPV_SHA256
    "fc0a97d36c42aafd3cfce56dcfdd7ff3c75d3ec7fd0e52a83c7f272c54feb3f9")

file(SHA256 "${BLOOM_NEUTRAL_DISPLAY_COMP}" bloom_neutral_display_comp_sha256)
if(NOT bloom_neutral_display_comp_sha256 STREQUAL BLOOM_NEUTRAL_DISPLAY_COMP_SHA256)
    message(FATAL_ERROR
        "neutral_display.comp SHA-256 mismatch: measured ${bloom_neutral_display_comp_sha256}, "
        "pinned ${BLOOM_NEUTRAL_DISPLAY_COMP_SHA256}. Regenerate only through tools/gpu-shaders and "
        "update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_NEUTRAL_DISPLAY_MANIFEST}" bloom_neutral_display_manifest_text)
if(NOT bloom_neutral_display_manifest_text MATCHES "${BLOOM_NEUTRAL_DISPLAY_COMP_SHA256}")
    message(FATAL_ERROR
        "neutral_display.manifest does not bind the pinned neutral_display.comp SHA-256.")
endif()

set(BLOOM_GLSLANG_VALIDATOR "")
set(BLOOM_SPIRV_VAL "")
set(BLOOM_SPIRV_DIS "")
if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/bin")
    find_program(bloom_locked_glslang_validator
        NAMES glslangValidator
        HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
        NO_DEFAULT_PATH)
    find_program(bloom_locked_spirv_val
        NAMES spirv-val
        HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
        NO_DEFAULT_PATH)
    find_program(bloom_locked_spirv_dis
        NAMES spirv-dis
        HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
        NO_DEFAULT_PATH)
    set(BLOOM_GLSLANG_VALIDATOR "${bloom_locked_glslang_validator}")
    set(BLOOM_SPIRV_VAL "${bloom_locked_spirv_val}")
    set(BLOOM_SPIRV_DIS "${bloom_locked_spirv_dis}")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_neutral_display_generated_spv
        "${CMAKE_CURRENT_BINARY_DIR}/neutral_display.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_NEUTRAL_DISPLAY_COMP}" -o "${bloom_neutral_display_generated_spv}"
        RESULT_VARIABLE bloom_neutral_display_glslang_result)
    if(NOT bloom_neutral_display_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile neutral_display.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2
                "${bloom_neutral_display_generated_spv}"
        RESULT_VARIABLE bloom_neutral_display_spirv_val_result)
    if(NOT bloom_neutral_display_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated neutral_display SPIR-V.")
    endif()
    file(SHA256 "${bloom_neutral_display_generated_spv}" bloom_neutral_display_spv_sha256)
    if(NOT bloom_neutral_display_spv_sha256 STREQUAL BLOOM_NEUTRAL_DISPLAY_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated neutral_display SPIR-V SHA-256 ${bloom_neutral_display_spv_sha256} does not "
            "match the embedded ${BLOOM_NEUTRAL_DISPLAY_SPV_SHA256}; the checked-in .comp and "
            "neutral_display_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: neutral_display.comp regenerated and SPIR-V-validated "
        "(${bloom_neutral_display_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "neutral_display.comp digest is still enforced.")
endif()

# --- Fixed SolidV1 resident-image shader: source pin and regeneration check -----------------------
#
# Offline artifact from the existing tools/gpu-shaders compile/validate/embed recipe (no new
# generator). The checked-in .comp is pinned here, the manifest must bind that source digest, and,
# when the locked glslangValidator/spirv-val are present, configure recompiles it and confirms the
# SPIR-V digest embedded in solid_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_SOLID_COMP "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/solid.comp")
set(BLOOM_SOLID_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/solid.manifest")
set(BLOOM_SOLID_INC "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/solid_spirv.inc")
set(BLOOM_SOLID_COMP_SHA256
    "ac0d5f31bc8b23e2c3744f3edd1ceb2308566d853cb36aea1bffbe5f009a110c")
set(BLOOM_SOLID_SPV_SHA256
    "2fae4aef67267e98a53034448639bfb00a69568ec3be1d8ed1a1bc60b31a0318")

file(SHA256 "${BLOOM_SOLID_COMP}" bloom_solid_comp_sha256)
if(NOT bloom_solid_comp_sha256 STREQUAL BLOOM_SOLID_COMP_SHA256)
    message(FATAL_ERROR
        "solid.comp SHA-256 mismatch: measured ${bloom_solid_comp_sha256}, pinned "
        "${BLOOM_SOLID_COMP_SHA256}. Regenerate only through the tools/gpu-shaders recipe and update "
        "the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_SOLID_MANIFEST}" bloom_solid_manifest_text)
if(NOT bloom_solid_manifest_text MATCHES "${BLOOM_SOLID_COMP_SHA256}")
    message(FATAL_ERROR "solid.manifest does not bind the pinned solid.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_solid_generated_spv "${CMAKE_CURRENT_BINARY_DIR}/solid.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_SOLID_COMP}" -o "${bloom_solid_generated_spv}"
        RESULT_VARIABLE bloom_solid_glslang_result)
    if(NOT bloom_solid_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile solid.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2 "${bloom_solid_generated_spv}"
        RESULT_VARIABLE bloom_solid_spirv_val_result)
    if(NOT bloom_solid_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated SolidV1 SPIR-V.")
    endif()
    file(SHA256 "${bloom_solid_generated_spv}" bloom_solid_spv_sha256)
    if(NOT bloom_solid_spv_sha256 STREQUAL BLOOM_SOLID_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated SolidV1 SPIR-V SHA-256 ${bloom_solid_spv_sha256} does not match the "
            "embedded ${BLOOM_SOLID_SPV_SHA256}; the checked-in .comp and solid_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: solid.comp regenerated and SPIR-V-validated "
        "(${bloom_solid_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "solid.comp digest is still enforced.")
endif()

# --- Fixed GpuPathCoverage shader: source pin and regeneration check -----------------------------
#
# Offline artifact from the existing tools/gpu-shaders compile/validate/embed recipe (no new
# generator, and the portable variant is checked to declare no Float64/Int64). The checked-in .comp is
# pinned here, the manifest must bind that source digest, and, when the locked
# glslangValidator/spirv-val are present, configure recompiles it and confirms the SPIR-V digest
# embedded in path_coverage_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_PATH_COVERAGE_COMP "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/path_coverage.comp")
set(BLOOM_PATH_COVERAGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/path_coverage.manifest")
set(BLOOM_PATH_COVERAGE_INC "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/path_coverage_spirv.inc")
set(BLOOM_PATH_COVERAGE_COMP_SHA256
    "5dd4e2486ddea9750b6755997c77816cce7c3b4ef83a6ce1a5d59fb21c932219")
set(BLOOM_PATH_COVERAGE_SPV_SHA256
    "f5aff832e47c64e42c95d0ae10429a5434e28566ccc1eb8d8c8d33e456bf2a4a")

file(SHA256 "${BLOOM_PATH_COVERAGE_COMP}" bloom_path_coverage_comp_sha256)
if(NOT bloom_path_coverage_comp_sha256 STREQUAL BLOOM_PATH_COVERAGE_COMP_SHA256)
    message(FATAL_ERROR
        "path_coverage.comp SHA-256 mismatch: measured ${bloom_path_coverage_comp_sha256}, pinned "
        "${BLOOM_PATH_COVERAGE_COMP_SHA256}. Regenerate only through the tools/gpu-shaders recipe and "
        "update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_PATH_COVERAGE_MANIFEST}" bloom_path_coverage_manifest_text)
if(NOT bloom_path_coverage_manifest_text MATCHES "${BLOOM_PATH_COVERAGE_COMP_SHA256}")
    message(FATAL_ERROR "path_coverage.manifest does not bind the pinned path_coverage.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_path_coverage_generated_spv "${CMAKE_CURRENT_BINARY_DIR}/path_coverage.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_PATH_COVERAGE_COMP}" -o "${bloom_path_coverage_generated_spv}"
        RESULT_VARIABLE bloom_path_coverage_glslang_result)
    if(NOT bloom_path_coverage_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile path_coverage.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2 "${bloom_path_coverage_generated_spv}"
        RESULT_VARIABLE bloom_path_coverage_spirv_val_result)
    if(NOT bloom_path_coverage_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated GpuPathCoverage SPIR-V.")
    endif()
    if(BLOOM_SPIRV_DIS)
        set(bloom_path_coverage_generated_spvasm
            "${CMAKE_CURRENT_BINARY_DIR}/path_coverage.generated.spvasm")
        execute_process(
            COMMAND "${BLOOM_SPIRV_DIS}" "${bloom_path_coverage_generated_spv}" -o
                    "${bloom_path_coverage_generated_spvasm}"
            RESULT_VARIABLE bloom_path_coverage_spirv_dis_result)
        if(NOT bloom_path_coverage_spirv_dis_result EQUAL 0)
            message(FATAL_ERROR "spirv-dis failed to disassemble the GpuPathCoverage SPIR-V.")
        endif()
        file(READ "${bloom_path_coverage_generated_spvasm}" bloom_path_coverage_spvasm_text)
        if(bloom_path_coverage_spvasm_text MATCHES "OpCapability Float64" OR
           bloom_path_coverage_spvasm_text MATCHES "OpCapability Int64" OR
           bloom_path_coverage_spvasm_text MATCHES "OpTypeFloat 64" OR
           bloom_path_coverage_spvasm_text MATCHES "OpTypeInt 64")
            message(FATAL_ERROR
                "the GpuPathCoverage SPIR-V declares a forbidden Float64/Int64 capability or type; "
                "the coverage kernel must stay pure integer sample counting.")
        endif()
    endif()
    file(SHA256 "${bloom_path_coverage_generated_spv}" bloom_path_coverage_spv_sha256)
    if(NOT bloom_path_coverage_spv_sha256 STREQUAL BLOOM_PATH_COVERAGE_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated GpuPathCoverage SPIR-V SHA-256 ${bloom_path_coverage_spv_sha256} does not "
            "match the embedded ${BLOOM_PATH_COVERAGE_SPV_SHA256}; the checked-in .comp and "
            "path_coverage_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: path_coverage.comp regenerated and SPIR-V-validated "
        "(${bloom_path_coverage_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "path_coverage.comp digest is still enforced.")
endif()

# --- Fixed TranslationOpacityBilinearV1 shader: source pin and regeneration check ----------------
#
# Offline artifact from the existing tools/gpu-shaders compile/validate/embed recipe. The checked-in
# .comp is pinned here, the manifest must bind that source digest, and, when the locked
# glslangValidator/spirv-val are present, configure recompiles it and confirms the SPIR-V digest
# embedded in translation_opacity_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_COMPOSITE_TRANSLATION_COMP
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/translation_opacity.comp")
set(BLOOM_COMPOSITE_TRANSLATION_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/translation_opacity.manifest")
set(BLOOM_COMPOSITE_TRANSLATION_INC
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/translation_opacity_spirv.inc")
set(BLOOM_COMPOSITE_TRANSLATION_COMP_SHA256
    "b92912217b61368c8b65cfda59620d9aad1a4fec5d6d9a31ce843982ee057650")
set(BLOOM_COMPOSITE_TRANSLATION_SPV_SHA256
    "0049b132bf98214375023a82472f2839a0e8f30e1b1209a0f214ef283b97c3b8")

file(SHA256 "${BLOOM_COMPOSITE_TRANSLATION_COMP}" bloom_composite_translation_comp_sha256)
if(NOT bloom_composite_translation_comp_sha256 STREQUAL BLOOM_COMPOSITE_TRANSLATION_COMP_SHA256)
    message(FATAL_ERROR
        "translation_opacity.comp SHA-256 mismatch: measured "
        "${bloom_composite_translation_comp_sha256}, pinned "
        "${BLOOM_COMPOSITE_TRANSLATION_COMP_SHA256}. Regenerate only through the tools/gpu-shaders "
        "recipe and update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_COMPOSITE_TRANSLATION_MANIFEST}" bloom_composite_translation_manifest_text)
if(NOT bloom_composite_translation_manifest_text MATCHES "${BLOOM_COMPOSITE_TRANSLATION_COMP_SHA256}")
    message(FATAL_ERROR
        "translation_opacity.manifest does not bind the pinned translation_opacity.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_composite_translation_generated_spv
        "${CMAKE_CURRENT_BINARY_DIR}/translation_opacity.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_COMPOSITE_TRANSLATION_COMP}" -o "${bloom_composite_translation_generated_spv}"
        RESULT_VARIABLE bloom_composite_translation_glslang_result)
    if(NOT bloom_composite_translation_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile translation_opacity.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2
                "${bloom_composite_translation_generated_spv}"
        RESULT_VARIABLE bloom_composite_translation_spirv_val_result)
    if(NOT bloom_composite_translation_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated TranslationOpacityBilinearV1 SPIR-V.")
    endif()
    file(SHA256 "${bloom_composite_translation_generated_spv}"
         bloom_composite_translation_spv_sha256)
    if(NOT bloom_composite_translation_spv_sha256 STREQUAL BLOOM_COMPOSITE_TRANSLATION_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated TranslationOpacityBilinearV1 SPIR-V SHA-256 "
            "${bloom_composite_translation_spv_sha256} does not match the embedded "
            "${BLOOM_COMPOSITE_TRANSLATION_SPV_SHA256}; the checked-in .comp and "
            "translation_opacity_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: translation_opacity.comp regenerated and SPIR-V-validated "
        "(${bloom_composite_translation_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "translation_opacity.comp digest is still enforced.")
endif()

# --- Fixed SourceOverV1 shader: source pin and regeneration check --------------------------------
set(BLOOM_COMPOSITE_SOURCE_OVER_COMP
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/source_over.comp")
set(BLOOM_COMPOSITE_SOURCE_OVER_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/source_over.manifest")
set(BLOOM_COMPOSITE_SOURCE_OVER_INC
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/source_over_spirv.inc")
set(BLOOM_COMPOSITE_SOURCE_OVER_COMP_SHA256
    "217423fab13d2fae83bc61f00ed37803cc23e7284df17bebee1a8cdf7e2e017f")
set(BLOOM_COMPOSITE_SOURCE_OVER_SPV_SHA256
    "2aea19b4e3620e9e99f09f7f0e22f97822119ea798b44a1077179c4fc58194ee")

file(SHA256 "${BLOOM_COMPOSITE_SOURCE_OVER_COMP}" bloom_composite_source_over_comp_sha256)
if(NOT bloom_composite_source_over_comp_sha256 STREQUAL BLOOM_COMPOSITE_SOURCE_OVER_COMP_SHA256)
    message(FATAL_ERROR
        "source_over.comp SHA-256 mismatch: measured ${bloom_composite_source_over_comp_sha256}, "
        "pinned ${BLOOM_COMPOSITE_SOURCE_OVER_COMP_SHA256}. Regenerate only through the "
        "tools/gpu-shaders recipe and update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_COMPOSITE_SOURCE_OVER_MANIFEST}" bloom_composite_source_over_manifest_text)
if(NOT bloom_composite_source_over_manifest_text MATCHES "${BLOOM_COMPOSITE_SOURCE_OVER_COMP_SHA256}")
    message(FATAL_ERROR "source_over.manifest does not bind the pinned source_over.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_composite_source_over_generated_spv
        "${CMAKE_CURRENT_BINARY_DIR}/source_over.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_COMPOSITE_SOURCE_OVER_COMP}" -o "${bloom_composite_source_over_generated_spv}"
        RESULT_VARIABLE bloom_composite_source_over_glslang_result)
    if(NOT bloom_composite_source_over_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile source_over.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2
                "${bloom_composite_source_over_generated_spv}"
        RESULT_VARIABLE bloom_composite_source_over_spirv_val_result)
    if(NOT bloom_composite_source_over_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated SourceOverV1 SPIR-V.")
    endif()
    file(SHA256 "${bloom_composite_source_over_generated_spv}"
         bloom_composite_source_over_spv_sha256)
    if(NOT bloom_composite_source_over_spv_sha256 STREQUAL BLOOM_COMPOSITE_SOURCE_OVER_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated SourceOverV1 SPIR-V SHA-256 ${bloom_composite_source_over_spv_sha256} "
            "does not match the embedded ${BLOOM_COMPOSITE_SOURCE_OVER_SPV_SHA256}; the checked-in "
            ".comp and source_over_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: source_over.comp regenerated and SPIR-V-validated "
        "(${bloom_composite_source_over_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "source_over.comp digest is still enforced.")
endif()

# --- Fixed AffineBilinearV1 shader: source pin and regeneration check ----------------------------
#
# Offline artifact from the existing tools/gpu-shaders compile/validate/embed recipe. The checked-in
# .comp is pinned here, the manifest must bind that source digest, and, when the locked
# glslangValidator/spirv-val are present, configure recompiles it and confirms the SPIR-V digest
# embedded in affine_bilinear_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_AFFINE_BILINEAR_COMP
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/affine_bilinear.comp")
set(BLOOM_AFFINE_BILINEAR_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/affine_bilinear.manifest")
set(BLOOM_AFFINE_BILINEAR_INC
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/affine_bilinear_spirv.inc")
set(BLOOM_AFFINE_BILINEAR_COMP_SHA256
    "8b728e94e85e2d83bd7604e43062f371f0d8578d5a71f06691df30bf747fdd54")
set(BLOOM_AFFINE_BILINEAR_SPV_SHA256
    "3db9745132a3ee0721675dddf5f8d041f4e89eb4a2e5fed0410f80d6468facef")

file(SHA256 "${BLOOM_AFFINE_BILINEAR_COMP}" bloom_affine_bilinear_comp_sha256)
if(NOT bloom_affine_bilinear_comp_sha256 STREQUAL BLOOM_AFFINE_BILINEAR_COMP_SHA256)
    message(FATAL_ERROR
        "affine_bilinear.comp SHA-256 mismatch: measured ${bloom_affine_bilinear_comp_sha256}, "
        "pinned ${BLOOM_AFFINE_BILINEAR_COMP_SHA256}. Regenerate only through the tools/gpu-shaders "
        "recipe and update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_AFFINE_BILINEAR_MANIFEST}" bloom_affine_bilinear_manifest_text)
if(NOT bloom_affine_bilinear_manifest_text MATCHES "${BLOOM_AFFINE_BILINEAR_COMP_SHA256}")
    message(FATAL_ERROR
        "affine_bilinear.manifest does not bind the pinned affine_bilinear.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_affine_bilinear_generated_spv
        "${CMAKE_CURRENT_BINARY_DIR}/affine_bilinear.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_AFFINE_BILINEAR_COMP}" -o "${bloom_affine_bilinear_generated_spv}"
        RESULT_VARIABLE bloom_affine_bilinear_glslang_result)
    if(NOT bloom_affine_bilinear_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile affine_bilinear.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2
                "${bloom_affine_bilinear_generated_spv}"
        RESULT_VARIABLE bloom_affine_bilinear_spirv_val_result)
    if(NOT bloom_affine_bilinear_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated AffineBilinearV1 SPIR-V.")
    endif()
    file(SHA256 "${bloom_affine_bilinear_generated_spv}" bloom_affine_bilinear_spv_sha256)
    if(NOT bloom_affine_bilinear_spv_sha256 STREQUAL BLOOM_AFFINE_BILINEAR_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated AffineBilinearV1 SPIR-V SHA-256 ${bloom_affine_bilinear_spv_sha256} does "
            "not match the embedded ${BLOOM_AFFINE_BILINEAR_SPV_SHA256}; the checked-in .comp and "
            "affine_bilinear_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: affine_bilinear.comp regenerated and SPIR-V-validated "
        "(${bloom_affine_bilinear_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "affine_bilinear.comp digest is still enforced.")
endif()

# --- Fixed PointResampleV1 shader: source pin and regeneration check -----------------------------
#
# Offline artifact from the existing tools/gpu-shaders compile/validate/embed recipe. The checked-in
# .comp is pinned here, the manifest must bind that source digest, and, when the locked
# glslangValidator/spirv-val are present, configure recompiles it and confirms the SPIR-V digest
# embedded in point_resample_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_POINT_RESAMPLE_COMP
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/point_resample.comp")
set(BLOOM_POINT_RESAMPLE_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/point_resample.manifest")
set(BLOOM_POINT_RESAMPLE_INC
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/point_resample_spirv.inc")
set(BLOOM_POINT_RESAMPLE_COMP_SHA256
    "9cad004e3e4a28f282c2ca6c0f088c42cc6d7bec8c9dba9d0e727ecf365705b8")
set(BLOOM_POINT_RESAMPLE_SPV_SHA256
    "4c053f341d4c23b84cf1717ef3d2a523347ae84f9e7638dd3c75410c9eeaa4a6")

file(SHA256 "${BLOOM_POINT_RESAMPLE_COMP}" bloom_point_resample_comp_sha256)
if(NOT bloom_point_resample_comp_sha256 STREQUAL BLOOM_POINT_RESAMPLE_COMP_SHA256)
    message(FATAL_ERROR
        "point_resample.comp SHA-256 mismatch: measured ${bloom_point_resample_comp_sha256}, "
        "pinned ${BLOOM_POINT_RESAMPLE_COMP_SHA256}. Regenerate only through the tools/gpu-shaders "
        "recipe and update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_POINT_RESAMPLE_MANIFEST}" bloom_point_resample_manifest_text)
if(NOT bloom_point_resample_manifest_text MATCHES "${BLOOM_POINT_RESAMPLE_COMP_SHA256}")
    message(FATAL_ERROR
        "point_resample.manifest does not bind the pinned point_resample.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_point_resample_generated_spv
        "${CMAKE_CURRENT_BINARY_DIR}/point_resample.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_POINT_RESAMPLE_COMP}" -o "${bloom_point_resample_generated_spv}"
        RESULT_VARIABLE bloom_point_resample_glslang_result)
    if(NOT bloom_point_resample_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile point_resample.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2
                "${bloom_point_resample_generated_spv}"
        RESULT_VARIABLE bloom_point_resample_spirv_val_result)
    if(NOT bloom_point_resample_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated PointResampleV1 SPIR-V.")
    endif()
    if(BLOOM_SPIRV_DIS)
        set(bloom_point_resample_generated_spvasm
            "${CMAKE_CURRENT_BINARY_DIR}/point_resample.generated.spvasm")
        execute_process(
            COMMAND "${BLOOM_SPIRV_DIS}" "${bloom_point_resample_generated_spv}" -o
                    "${bloom_point_resample_generated_spvasm}"
            RESULT_VARIABLE bloom_point_resample_spirv_dis_result)
        if(NOT bloom_point_resample_spirv_dis_result EQUAL 0)
            message(FATAL_ERROR "spirv-dis failed to disassemble the PointResampleV1 SPIR-V.")
        endif()
        file(READ "${bloom_point_resample_generated_spvasm}" bloom_point_resample_spvasm_text)
        if(bloom_point_resample_spvasm_text MATCHES "OpCapability Float64" OR
           bloom_point_resample_spvasm_text MATCHES "OpCapability Int64" OR
           bloom_point_resample_spvasm_text MATCHES "OpTypeFloat 64" OR
           bloom_point_resample_spvasm_text MATCHES "OpTypeInt 64")
            message(FATAL_ERROR
                "the PointResampleV1 SPIR-V declares a forbidden Float64/Int64 capability or "
                "type; it must run on standard Float32 hardware without shaderFloat64.")
        endif()
    endif()
    file(SHA256 "${bloom_point_resample_generated_spv}" bloom_point_resample_spv_sha256)
    if(NOT bloom_point_resample_spv_sha256 STREQUAL BLOOM_POINT_RESAMPLE_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated PointResampleV1 SPIR-V SHA-256 ${bloom_point_resample_spv_sha256} does "
            "not match the embedded ${BLOOM_POINT_RESAMPLE_SPV_SHA256}; the checked-in .comp and "
            "point_resample_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: point_resample.comp regenerated, SPIR-V-validated, and "
        "capability-checked (${bloom_point_resample_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "point_resample.comp digest is still enforced.")
endif()

# --- Fixed BlendV1 shader: source pin and regeneration check -------------------------------------
#
# Offline artifact from the existing tools/gpu-shaders compile/validate/embed recipe. The checked-in
# .comp is pinned here, the manifest must bind that source digest, and, when the locked
# glslangValidator/spirv-val are present, configure recompiles it and confirms the SPIR-V digest
# embedded in blend_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_BLEND_COMP "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend.comp")
set(BLOOM_BLEND_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend.manifest")
set(BLOOM_BLEND_INC "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend_spirv.inc")
set(BLOOM_BLEND_COMP_SHA256
    "d9168cc4915ae1fd31a984b00f198e68e85e2c67d1a755ca8c18ba886c873410")
set(BLOOM_BLEND_SPV_SHA256
    "4b9cc2009f9a1fcbdc05558bbbbb9e9aeb7573a4fc88a8127e25e22bc6c208f8")

file(SHA256 "${BLOOM_BLEND_COMP}" bloom_blend_comp_sha256)
if(NOT bloom_blend_comp_sha256 STREQUAL BLOOM_BLEND_COMP_SHA256)
    message(FATAL_ERROR
        "blend.comp SHA-256 mismatch: measured ${bloom_blend_comp_sha256}, pinned "
        "${BLOOM_BLEND_COMP_SHA256}. Regenerate only through the tools/gpu-shaders recipe and update "
        "the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_BLEND_MANIFEST}" bloom_blend_manifest_text)
if(NOT bloom_blend_manifest_text MATCHES "${BLOOM_BLEND_COMP_SHA256}")
    message(FATAL_ERROR "blend.manifest does not bind the pinned blend.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_blend_generated_spv "${CMAKE_CURRENT_BINARY_DIR}/blend.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_BLEND_COMP}" -o "${bloom_blend_generated_spv}"
        RESULT_VARIABLE bloom_blend_glslang_result)
    if(NOT bloom_blend_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile blend.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2 "${bloom_blend_generated_spv}"
        RESULT_VARIABLE bloom_blend_spirv_val_result)
    if(NOT bloom_blend_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated BlendV1 SPIR-V.")
    endif()
    file(SHA256 "${bloom_blend_generated_spv}" bloom_blend_spv_sha256)
    if(NOT bloom_blend_spv_sha256 STREQUAL BLOOM_BLEND_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated BlendV1 SPIR-V SHA-256 ${bloom_blend_spv_sha256} does not match the "
            "embedded ${BLOOM_BLEND_SPV_SHA256}; the checked-in .comp and blend_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: blend.comp regenerated and SPIR-V-validated "
        "(${bloom_blend_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "blend.comp digest is still enforced.")
endif()

# --- Fixed BlendV1 Float64 shader: source pin and regeneration check -----------------------------
#
# The exact-arithmetic companion to blend.comp, used only when the device advertised and enabled the
# core shaderFloat64 feature. Same offline recipe and pins.
set(BLOOM_BLEND_F64_COMP "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend_f64.comp")
set(BLOOM_BLEND_F64_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend_f64.manifest")
set(BLOOM_BLEND_F64_INC "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend_f64_spirv.inc")
set(BLOOM_BLEND_F64_COMP_SHA256
    "80b1805269f394051a86231638cf0fcc5fa46a31fd1299a7def14e81ecfc7c91")
set(BLOOM_BLEND_F64_SPV_SHA256
    "a910e190a05875c22f16d1571dea9e05496b32d5a588ede8cc8d95520ea7dbcf")

file(SHA256 "${BLOOM_BLEND_F64_COMP}" bloom_blend_f64_comp_sha256)
if(NOT bloom_blend_f64_comp_sha256 STREQUAL BLOOM_BLEND_F64_COMP_SHA256)
    message(FATAL_ERROR
        "blend_f64.comp SHA-256 mismatch: measured ${bloom_blend_f64_comp_sha256}, pinned "
        "${BLOOM_BLEND_F64_COMP_SHA256}. Regenerate only through the tools/gpu-shaders recipe and "
        "update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_BLEND_F64_MANIFEST}" bloom_blend_f64_manifest_text)
if(NOT bloom_blend_f64_manifest_text MATCHES "${BLOOM_BLEND_F64_COMP_SHA256}")
    message(FATAL_ERROR "blend_f64.manifest does not bind the pinned blend_f64.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_blend_f64_generated_spv "${CMAKE_CURRENT_BINARY_DIR}/blend_f64.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_BLEND_F64_COMP}" -o "${bloom_blend_f64_generated_spv}"
        RESULT_VARIABLE bloom_blend_f64_glslang_result)
    if(NOT bloom_blend_f64_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile blend_f64.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2 "${bloom_blend_f64_generated_spv}"
        RESULT_VARIABLE bloom_blend_f64_spirv_val_result)
    if(NOT bloom_blend_f64_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated BlendV1 Float64 SPIR-V.")
    endif()
    file(SHA256 "${bloom_blend_f64_generated_spv}" bloom_blend_f64_spv_sha256)
    if(NOT bloom_blend_f64_spv_sha256 STREQUAL BLOOM_BLEND_F64_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated BlendV1 Float64 SPIR-V SHA-256 ${bloom_blend_f64_spv_sha256} does not "
            "match the embedded ${BLOOM_BLEND_F64_SPV_SHA256}; the checked-in .comp and "
            "blend_f64_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: blend_f64.comp regenerated and SPIR-V-validated "
        "(${bloom_blend_f64_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "blend_f64.comp digest is still enforced.")
endif()

# --- Portable BlendV1 Float32 shader: source pin, capability, and regeneration check ------------
#
# The compensated-Float32 companion for the six general modes, used on devices without the core
# shaderFloat64 feature. It must declare no Float64/Int64 capability or 64-bit type, so configure
# disassembles the regenerated SPIR-V with spirv-dis and rejects either. The checked-in .comp is
# pinned here, the manifest must bind that source digest, and, when the locked glslangValidator/
# spirv-val are present, configure recompiles it and confirms the SPIR-V digest embedded in
# blend_portable_spirv.inc. Runtime code never compiles or loads a shader. The deterministic
# regeneration path is the standalone tools/gpu-shaders project: its
# bloom_gpu_blend_spirv_generator (embed_blend_spirv.cpp) writes the checked-in .inc and manifest
# pins and its bloom_gpu_blend_spirv_verify target --checks and regenerates every blend family.
set(BLOOM_BLEND_PORTABLE_COMP "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend_portable.comp")
set(BLOOM_BLEND_PORTABLE_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend_portable.manifest")
set(BLOOM_BLEND_PORTABLE_INC
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/blend_portable_spirv.inc")
set(BLOOM_BLEND_PORTABLE_COMP_SHA256
    "7b9234c2d33ccdf5cbcab8f7c8502542d6a35ed9e0740e850a1694c9f3bbad0d")
set(BLOOM_BLEND_PORTABLE_SPV_SHA256
    "41c0272525b19c8dd43439244411b078e6c2936926fd8456c33fabf60cf68841")

file(SHA256 "${BLOOM_BLEND_PORTABLE_COMP}" bloom_blend_portable_comp_sha256)
if(NOT bloom_blend_portable_comp_sha256 STREQUAL BLOOM_BLEND_PORTABLE_COMP_SHA256)
    message(FATAL_ERROR
        "blend_portable.comp SHA-256 mismatch: measured ${bloom_blend_portable_comp_sha256}, pinned "
        "${BLOOM_BLEND_PORTABLE_COMP_SHA256}. Regenerate only through the tools/gpu-shaders recipe "
        "and update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_BLEND_PORTABLE_MANIFEST}" bloom_blend_portable_manifest_text)
if(NOT bloom_blend_portable_manifest_text MATCHES "${BLOOM_BLEND_PORTABLE_COMP_SHA256}")
    message(FATAL_ERROR "blend_portable.manifest does not bind the pinned blend_portable.comp "
        "SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_blend_portable_generated_spv
        "${CMAKE_CURRENT_BINARY_DIR}/blend_portable.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_BLEND_PORTABLE_COMP}" -o "${bloom_blend_portable_generated_spv}"
        RESULT_VARIABLE bloom_blend_portable_glslang_result)
    if(NOT bloom_blend_portable_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile blend_portable.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2
                "${bloom_blend_portable_generated_spv}"
        RESULT_VARIABLE bloom_blend_portable_spirv_val_result)
    if(NOT bloom_blend_portable_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated portable BlendV1 SPIR-V.")
    endif()
    if(BLOOM_SPIRV_DIS)
        set(bloom_blend_portable_generated_spvasm
            "${CMAKE_CURRENT_BINARY_DIR}/blend_portable.generated.spvasm")
        execute_process(
            COMMAND "${BLOOM_SPIRV_DIS}" "${bloom_blend_portable_generated_spv}" -o
                    "${bloom_blend_portable_generated_spvasm}"
            RESULT_VARIABLE bloom_blend_portable_spirv_dis_result)
        if(NOT bloom_blend_portable_spirv_dis_result EQUAL 0)
            message(FATAL_ERROR "spirv-dis failed to disassemble the portable BlendV1 SPIR-V.")
        endif()
        file(READ "${bloom_blend_portable_generated_spvasm}" bloom_blend_portable_spvasm_text)
        if(bloom_blend_portable_spvasm_text MATCHES "OpCapability Float64" OR
           bloom_blend_portable_spvasm_text MATCHES "OpCapability Int64" OR
           bloom_blend_portable_spvasm_text MATCHES "OpTypeFloat 64" OR
           bloom_blend_portable_spvasm_text MATCHES "OpTypeInt 64")
            message(FATAL_ERROR
                "the portable BlendV1 SPIR-V declares a forbidden Float64/Int64 capability or type; "
                "it must run on standard Float32 hardware.")
        endif()
    endif()
    file(SHA256 "${bloom_blend_portable_generated_spv}" bloom_blend_portable_spv_sha256)
    if(NOT bloom_blend_portable_spv_sha256 STREQUAL BLOOM_BLEND_PORTABLE_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated portable BlendV1 SPIR-V SHA-256 ${bloom_blend_portable_spv_sha256} does "
            "not match the embedded ${BLOOM_BLEND_PORTABLE_SPV_SHA256}; the checked-in .comp and "
            "blend_portable_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: blend_portable.comp regenerated, SPIR-V-validated, and "
        "capability-checked (${bloom_blend_portable_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "blend_portable.comp digest is still enforced.")
endif()

# --- Fixed CoveredSolidV1 resident-image shader: source pin and regeneration check ---------------
#
# Offline artifact from the same tools/gpu-shaders compile/validate/embed recipe (no new generator).
# The checked-in .comp is pinned here, the manifest must bind that source digest, and, when the
# locked glslangValidator/spirv-val are present, configure recompiles it and confirms the SPIR-V
# digest embedded in solid_covered_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_SOLID_COVERED_COMP "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/solid_covered.comp")
set(BLOOM_SOLID_COVERED_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/solid_covered.manifest")
set(BLOOM_SOLID_COVERED_INC "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/solid_covered_spirv.inc")
set(BLOOM_SOLID_COVERED_COMP_SHA256
    "0675f8a7c1bdcafe68ffc45f21cc4b9437251d18f8e7652cc39f52bcc486947b")
set(BLOOM_SOLID_COVERED_SPV_SHA256
    "3d54bcb0b9394b381df9f271cdfd05af4cb2f80620ba142227fec53ba26e1a9b")

file(SHA256 "${BLOOM_SOLID_COVERED_COMP}" bloom_solid_covered_comp_sha256)
if(NOT bloom_solid_covered_comp_sha256 STREQUAL BLOOM_SOLID_COVERED_COMP_SHA256)
    message(FATAL_ERROR
        "solid_covered.comp SHA-256 mismatch: measured ${bloom_solid_covered_comp_sha256}, pinned "
        "${BLOOM_SOLID_COVERED_COMP_SHA256}. Regenerate only through the tools/gpu-shaders recipe and "
        "update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_SOLID_COVERED_MANIFEST}" bloom_solid_covered_manifest_text)
if(NOT bloom_solid_covered_manifest_text MATCHES "${BLOOM_SOLID_COVERED_COMP_SHA256}")
    message(FATAL_ERROR "solid_covered.manifest does not bind the pinned solid_covered.comp SHA-256.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_solid_covered_generated_spv "${CMAKE_CURRENT_BINARY_DIR}/solid_covered.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_SOLID_COVERED_COMP}" -o "${bloom_solid_covered_generated_spv}"
        RESULT_VARIABLE bloom_solid_covered_glslang_result)
    if(NOT bloom_solid_covered_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile solid_covered.comp.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2 "${bloom_solid_covered_generated_spv}"
        RESULT_VARIABLE bloom_solid_covered_spirv_val_result)
    if(NOT bloom_solid_covered_spirv_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated CoveredSolidV1 SPIR-V.")
    endif()
    file(SHA256 "${bloom_solid_covered_generated_spv}" bloom_solid_covered_spv_sha256)
    if(NOT bloom_solid_covered_spv_sha256 STREQUAL BLOOM_SOLID_COVERED_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated CoveredSolidV1 SPIR-V SHA-256 ${bloom_solid_covered_spv_sha256} does not "
            "match the embedded ${BLOOM_SOLID_COVERED_SPV_SHA256}; the checked-in .comp and "
            "solid_covered_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: solid_covered.comp regenerated and SPIR-V-validated "
        "(${bloom_solid_covered_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "solid_covered.comp digest is still enforced.")
endif()

# --- Fixed ViewerPresentV2 vertex/fragment shaders: source pins and regeneration checks ----------
#
# Offline artifacts from the same tools/gpu-shaders compile/validate/embed recipe (no new generator).
# Both checked-in sources are pinned here, the manifest must bind both source digests, and, when the
# locked glslangValidator/spirv-val are present, configure recompiles them and confirms the SPIR-V
# digests embedded in viewer_present_spirv.inc. Runtime code never compiles or loads a shader.
set(BLOOM_VIEWER_PRESENT_VERT
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/viewer_present.vert")
set(BLOOM_VIEWER_PRESENT_FRAG
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/viewer_present.frag")
set(BLOOM_VIEWER_PRESENT_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/viewer_present.manifest")
set(BLOOM_VIEWER_PRESENT_INC
    "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/viewer_present_spirv.inc")
set(BLOOM_VIEWER_PRESENT_VERT_SHA256
    "c72b8de52cab61b39d3441a18d7bad08f5116154206a0e59b653b49e5589e6ff")
set(BLOOM_VIEWER_PRESENT_FRAG_SHA256
    "817fdaeb48bf7037b69aa38e8a51ee9aee2b28d7beecbfdacbece59fe34ee7b6")
set(BLOOM_VIEWER_PRESENT_VERT_SPV_SHA256
    "02530de5a4b2abf9944d5b9ee40c610069c97b20fdbbfdc27e9f99f2f9aa415a")
set(BLOOM_VIEWER_PRESENT_FRAG_SPV_SHA256
    "8a2120e1c7580034e85c1954c8cce9702ceb4344868d5458e9f9980a08b15105")

file(SHA256 "${BLOOM_VIEWER_PRESENT_VERT}" bloom_viewer_present_vert_sha256)
if(NOT bloom_viewer_present_vert_sha256 STREQUAL BLOOM_VIEWER_PRESENT_VERT_SHA256)
    message(FATAL_ERROR
        "viewer_present.vert SHA-256 mismatch: measured ${bloom_viewer_present_vert_sha256}, "
        "pinned ${BLOOM_VIEWER_PRESENT_VERT_SHA256}. Regenerate only through the tools/gpu-shaders "
        "recipe and update the pin and the embedded SPIR-V together.")
endif()
file(SHA256 "${BLOOM_VIEWER_PRESENT_FRAG}" bloom_viewer_present_frag_sha256)
if(NOT bloom_viewer_present_frag_sha256 STREQUAL BLOOM_VIEWER_PRESENT_FRAG_SHA256)
    message(FATAL_ERROR
        "viewer_present.frag SHA-256 mismatch: measured ${bloom_viewer_present_frag_sha256}, "
        "pinned ${BLOOM_VIEWER_PRESENT_FRAG_SHA256}. Regenerate only through the tools/gpu-shaders "
        "recipe and update the pin and the embedded SPIR-V together.")
endif()
file(READ "${BLOOM_VIEWER_PRESENT_MANIFEST}" bloom_viewer_present_manifest_text)
if(NOT bloom_viewer_present_manifest_text MATCHES "${BLOOM_VIEWER_PRESENT_VERT_SHA256}" OR
   NOT bloom_viewer_present_manifest_text MATCHES "${BLOOM_VIEWER_PRESENT_FRAG_SHA256}")
    message(FATAL_ERROR
        "viewer_present.manifest does not bind both pinned viewer_present.vert/frag SHA-256 digests.")
endif()
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(bloom_viewer_present_vert_spv
        "${CMAKE_CURRENT_BINARY_DIR}/viewer_present.vert.generated.spv")
    set(bloom_viewer_present_frag_spv
        "${CMAKE_CURRENT_BINARY_DIR}/viewer_present.frag.generated.spv")
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_VIEWER_PRESENT_VERT}" -o "${bloom_viewer_present_vert_spv}"
        RESULT_VARIABLE bloom_viewer_present_vert_glslang_result)
    if(NOT bloom_viewer_present_vert_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile viewer_present.vert.")
    endif()
    execute_process(
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_VIEWER_PRESENT_FRAG}" -o "${bloom_viewer_present_frag_spv}"
        RESULT_VARIABLE bloom_viewer_present_frag_glslang_result)
    if(NOT bloom_viewer_present_frag_glslang_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed to compile viewer_present.frag.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2 "${bloom_viewer_present_vert_spv}"
        RESULT_VARIABLE bloom_viewer_present_vert_val_result)
    if(NOT bloom_viewer_present_vert_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated ViewerPresentV2 vertex SPIR-V.")
    endif()
    execute_process(
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2 "${bloom_viewer_present_frag_spv}"
        RESULT_VARIABLE bloom_viewer_present_frag_val_result)
    if(NOT bloom_viewer_present_frag_val_result EQUAL 0)
        message(FATAL_ERROR "spirv-val rejected the regenerated ViewerPresentV2 fragment SPIR-V.")
    endif()
    file(SHA256 "${bloom_viewer_present_vert_spv}" bloom_viewer_present_vert_spv_sha256)
    file(SHA256 "${bloom_viewer_present_frag_spv}" bloom_viewer_present_frag_spv_sha256)
    if(NOT bloom_viewer_present_vert_spv_sha256 STREQUAL BLOOM_VIEWER_PRESENT_VERT_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated viewer_present vertex SPIR-V SHA-256 "
            "${bloom_viewer_present_vert_spv_sha256} does not match the embedded "
            "${BLOOM_VIEWER_PRESENT_VERT_SPV_SHA256}; the checked-in .vert and "
            "viewer_present_spirv.inc disagree.")
    endif()
    if(NOT bloom_viewer_present_frag_spv_sha256 STREQUAL BLOOM_VIEWER_PRESENT_FRAG_SPV_SHA256)
        message(FATAL_ERROR
            "regenerated viewer_present fragment SPIR-V SHA-256 "
            "${bloom_viewer_present_frag_spv_sha256} does not match the embedded "
            "${BLOOM_VIEWER_PRESENT_FRAG_SPV_SHA256}; the checked-in .frag and "
            "viewer_present_spirv.inc disagree.")
    endif()
    message(STATUS "Bloom render: viewer_present.vert/frag regenerated and SPIR-V-validated "
        "(${bloom_viewer_present_vert_spv_sha256}, ${bloom_viewer_present_frag_spv_sha256}).")
else()
    message(STATUS "Bloom render: glslangValidator/spirv-val not found in the prefix; the pinned "
        "viewer_present.vert/frag digests are still enforced.")
endif()

set(BLOOM_RENDER_VULKAN_AVAILABLE OFF)
if(BLOOM_RENDER_ENABLE_VULKAN AND DEFINED BLOOM_DEPENDENCY_MODE)
    if(BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        find_package(VulkanHeaders QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
        find_package(VulkanMemoryAllocator QUIET CONFIG
            PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    elseif(BLOOM_DEPENDENCY_MODE STREQUAL "developer-system")
        find_package(VulkanHeaders QUIET CONFIG)
        find_package(VulkanMemoryAllocator QUIET CONFIG)
    endif()

    if(TARGET Vulkan::Headers AND TARGET GPUOpen::VulkanMemoryAllocator)
        # Availability is proven; now bind through the governed, mode-aware helper so qualified
        # mode is enforced by the same fail-closed path as every other dependency.
        bloom_find_dependency(VulkanHeaders)
        bloom_find_dependency(VulkanMemoryAllocator)
        set(BLOOM_RENDER_VULKAN_AVAILABLE ON)
    else()
        message(STATUS "Bloom render: Vulkan headers/VMA not found in this dependency mode; "
            "building the CPU-unavailable GPU device stub.")
    endif()
endif()
# Exported so sibling directories (e.g. src/color) can register GPU-native test targets only when
# the real Vulkan backend is compiled; a CPU-stub build must not link Vulkan-only test helpers.
set(BLOOM_RENDER_VULKAN_AVAILABLE "${BLOOM_RENDER_VULKAN_AVAILABLE}" CACHE INTERNAL
    "bloom_render compiled the real Vulkan backend")

# VMA's implementation is isolated in its own object target, exactly like the vendored stb_truetype
# target above: third-party code gets its own warning profile and never inherits Bloom's
# warnings-as-errors, while the strict floating-point flags the rest of src/render uses still apply.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_library(bloom_render_vma OBJECT vulkan/vma.cpp)
    target_compile_features(bloom_render_vma PUBLIC cxx_std_20)
    target_link_libraries(
        bloom_render_vma
        PRIVATE Vulkan::Headers GPUOpen::VulkanMemoryAllocator
    )
    if(MSVC)
        target_compile_options(bloom_render_vma PRIVATE /w)
    else()
        target_compile_options(bloom_render_vma PRIVATE -w)
    endif()
    bloom_enable_strict_floating_point(bloom_render_vma)
endif()

add_library(bloom_render STATIC)

target_sources(
    bloom_render
    PRIVATE
        cpu_image_primitives.cpp
        display_buffer.cpp
        gpu_affine_host.cpp
        gpu_point_resample_host.cpp
        image.cpp
        image_types.cpp
        ocio_gpu_program.cpp
        text_glyph_source.hpp
        text_raster.cpp
        path_raster.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/render/cpu_image_primitives.hpp
            include/bloom/render/display_buffer.hpp
            include/bloom/render/embedded_fonts.hpp
            include/bloom/render/gpu_affine.hpp
            include/bloom/render/gpu_blend.hpp
            include/bloom/render/gpu_composite.hpp
            include/bloom/render/gpu_device.hpp
            include/bloom/render/gpu_image.hpp
            include/bloom/render/gpu_image_upload.hpp
            include/bloom/render/gpu_neutral_display.hpp
            include/bloom/render/gpu_ocio_program.hpp
            include/bloom/render/gpu_output_color_readback.hpp
            include/bloom/render/gpu_path_coverage.hpp
            include/bloom/render/gpu_present_image.hpp
            include/bloom/render/ocio_gpu_program.hpp
            include/bloom/render/gpu_presentation_target.hpp
            include/bloom/render/gpu_point_resample.hpp
            include/bloom/render/gpu_presentation_types.hpp
            include/bloom/render/gpu_process_readback.hpp
            include/bloom/render/gpu_resident_display.hpp
            include/bloom/render/gpu_shader_artifact.hpp
            include/bloom/render/gpu_solid.hpp
            include/bloom/render/image.hpp
            include/bloom/render/image_types.hpp
            include/bloom/render/text_raster.hpp
            include/bloom/render/path_raster.hpp
)

# Exactly one of the two implementations defines the shared API symbols. The Vulkan translation
# unit keeps its include requirements PRIVATE so no Vulkan header reaches bloom_render consumers or
# the desktop application.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    target_sources(bloom_render PRIVATE
        vulkan/gpu_affine.cpp
        vulkan/gpu_affine_fault.hpp
        vulkan/gpu_affine_private.hpp
        vulkan/gpu_affine_resources.cpp
        vulkan/gpu_affine_retirement.cpp
        vulkan/gpu_blend.cpp
        vulkan/gpu_blend_fault.hpp
        vulkan/gpu_blend_private.hpp
        vulkan/gpu_blend_resources.cpp
        vulkan/gpu_blend_retirement.cpp
        vulkan/gpu_bounded_retirement.hpp
        vulkan/gpu_composite.cpp
        vulkan/gpu_composite_fault.hpp
        vulkan/gpu_composite_private.hpp
        vulkan/gpu_composite_resources.cpp
        vulkan/gpu_composite_retirement.cpp
        vulkan/gpu_device.cpp
        vulkan/gpu_device_bootstrap.cpp
        vulkan/gpu_device_private.hpp
        vulkan/gpu_image.cpp
        vulkan/gpu_image_private.hpp
        vulkan/gpu_image_upload.cpp
        vulkan/gpu_image_upload_fault.hpp
        vulkan/gpu_image_upload_private.hpp
        vulkan/gpu_image_upload_resources.cpp
        vulkan/gpu_image_upload_retirement.cpp
        vulkan/gpu_neutral_display.cpp
        vulkan/gpu_neutral_display_private.hpp
        vulkan/gpu_neutral_display_resources.cpp
        vulkan/gpu_ocio_program.cpp
        vulkan/gpu_ocio_program_api.cpp
        vulkan/gpu_ocio_program_dispatch.cpp
        vulkan/gpu_ocio_program_private.hpp
        vulkan/gpu_ocio_program_resources.cpp
        vulkan/gpu_output_color_readback.cpp
        vulkan/gpu_point_resample.cpp
        vulkan/gpu_point_resample_private.hpp
        vulkan/gpu_point_resample_resources.cpp
        vulkan/gpu_present_image.cpp
        vulkan/gpu_present_image_private.hpp
        vulkan/gpu_present_image_resources.cpp
        vulkan/gpu_presentation_target.cpp
        vulkan/gpu_presentation_target_private.hpp
        vulkan/gpu_presentation_target_resources.cpp
        vulkan/gpu_process_readback.cpp
        vulkan/gpu_resident_display.cpp
        vulkan/gpu_resident_display_private.hpp
        vulkan/gpu_resident_display_readback.cpp
        vulkan/gpu_resident_display_readback_private.hpp
        vulkan/gpu_resident_display_resources.cpp
        vulkan/gpu_solid.cpp
        vulkan/gpu_solid_covered.cpp
        vulkan/gpu_solid_fault.hpp
        vulkan/gpu_solid_private.hpp
        vulkan/gpu_solid_retirement.cpp
        vulkan/gpu_path_coverage.cpp
        vulkan/gpu_path_coverage_lifecycle.cpp
        vulkan/gpu_path_coverage_resources.cpp
        vulkan/gpu_path_coverage_fault.hpp
        vulkan/gpu_path_coverage_private.hpp
        vulkan/shaders/path_coverage.comp
        vulkan/shaders/path_coverage.manifest
        vulkan/shaders/path_coverage_spirv.inc
        vulkan/shaders/neutral_display.comp
        vulkan/shaders/neutral_display.manifest
        vulkan/shaders/neutral_display_spirv.inc
        vulkan/shaders/solid.comp
        vulkan/shaders/solid.manifest
        vulkan/shaders/solid_spirv.inc
        vulkan/shaders/affine_bilinear.comp
        vulkan/shaders/affine_bilinear.manifest
        vulkan/shaders/affine_bilinear_spirv.inc
        vulkan/shaders/point_resample.comp
        vulkan/shaders/point_resample.manifest
        vulkan/shaders/point_resample_spirv.inc
        vulkan/shaders/solid_covered.comp
        vulkan/shaders/solid_covered.manifest
        vulkan/shaders/solid_covered_spirv.inc
        vulkan/shaders/translation_opacity.comp
        vulkan/shaders/translation_opacity.manifest
        vulkan/shaders/translation_opacity_spirv.inc
        vulkan/shaders/source_over.comp
        vulkan/shaders/source_over.manifest
        vulkan/shaders/source_over_spirv.inc
        vulkan/shaders/blend.comp
        vulkan/shaders/blend.manifest
        vulkan/shaders/blend_spirv.inc
        vulkan/shaders/blend_f64.comp
        vulkan/shaders/blend_f64.manifest
        vulkan/shaders/blend_f64_spirv.inc
        vulkan/shaders/blend_portable.comp
        vulkan/shaders/blend_portable.manifest
        vulkan/shaders/blend_portable_spirv.inc
        vulkan/shaders/viewer_present.vert
        vulkan/shaders/viewer_present.frag
        vulkan/shaders/viewer_present.manifest
        vulkan/shaders/viewer_present_spirv.inc
    )
else()
    target_sources(bloom_render PRIVATE gpu_affine_stub.cpp gpu_blend_stub.cpp
                                     gpu_composite_stub.cpp gpu_device_stub.cpp
                                     gpu_image_upload_stub.cpp gpu_neutral_display_stub.cpp
                                     gpu_point_resample_stub.cpp
                                     gpu_ocio_program_stub.cpp
                                     gpu_output_color_readback_stub.cpp
                                     gpu_present_image_stub.cpp gpu_presentation_target_stub.cpp
                                     gpu_process_readback_stub.cpp gpu_resident_display_stub.cpp
                                     gpu_solid_stub.cpp gpu_path_coverage_stub.cpp)
endif()

target_compile_features(bloom_render PUBLIC cxx_std_20)
target_include_directories(bloom_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    bloom_render
    PUBLIC bloom_core
    PRIVATE bloom_render_fonts bloom_render_stb_truetype
)
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    target_link_libraries(
        bloom_render
        PRIVATE
            bloom_render_vma
            Vulkan::Headers
            GPUOpen::VulkanMemoryAllocator
            ${CMAKE_DL_LIBS}
    )
endif()
bloom_enable_warnings(bloom_render)
bloom_enable_strict_floating_point(bloom_render)

if(BUILD_TESTING)
    # Test-only fault setters, deliberately NOT part of bloom_render: production targets never link
    # this library, so shipping builds cannot expose fault behavior. The setters call into the
    # Vulkan resource translation unit, so they exist only when the real backend is compiled.
    if(BLOOM_RENDER_VULKAN_AVAILABLE)
        add_library(bloom_render_ocio_test_faults STATIC vulkan/gpu_ocio_program_fault_test.cpp)
        target_compile_features(bloom_render_ocio_test_faults PUBLIC cxx_std_20)
        target_include_directories(bloom_render_ocio_test_faults PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/vulkan")
        bloom_enable_warnings(bloom_render_ocio_test_faults)

        # Same pattern for the combined output-colour readback fault seam.
        add_library(bloom_render_output_color_readback_test_faults STATIC
            vulkan/gpu_output_color_readback_fault_test.cpp)
        target_compile_features(bloom_render_output_color_readback_test_faults PUBLIC cxx_std_20)
        target_include_directories(bloom_render_output_color_readback_test_faults PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/vulkan")
        bloom_enable_warnings(bloom_render_output_color_readback_test_faults)
        # Same pattern for the PointResampleV1 lifecycle fault seam.
        add_library(bloom_render_point_resample_test_faults STATIC
            vulkan/gpu_point_resample_fault_test.cpp)
        target_compile_features(bloom_render_point_resample_test_faults PUBLIC cxx_std_20)
        target_include_directories(bloom_render_point_resample_test_faults PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/vulkan")
        bloom_enable_warnings(bloom_render_point_resample_test_faults)
    endif()
    add_subdirectory(tests)
endif()
