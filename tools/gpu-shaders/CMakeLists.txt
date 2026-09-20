cmake_minimum_required(VERSION 3.25)

# Standalone, offline generator for the first fixed Bloom Neutral v1 display compute shader.
#
# Configure and build it out-of-tree with an explicit locked dependency prefix; system
# OpenColorIO is refused.
#
#   cmake -S tools/gpu-shaders -B <build-dir> -G Ninja \
#         -DBLOOM_DEPENDENCY_PREFIX=<locked-prefix>
#   cmake --build <build-dir>
#
# See README.md for the exact regenerate/verify commands and the numeric contract.

project(BloomGpuShaders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Locked dependency prefix (OpenColorIO + glslang/spirv-tools live here).
# ---------------------------------------------------------------------------
set(BLOOM_DEPENDENCY_PREFIX "" CACHE PATH
    "Locked Bloom dependency prefix that owns OpenColorIO and the Vulkan shader tools")
if(NOT BLOOM_DEPENDENCY_PREFIX)
    message(FATAL_ERROR
        "BLOOM_DEPENDENCY_PREFIX is required; point it at the locked Bloom dependency prefix "
        "(the same prefix used by the application dependency build). System OpenColorIO is not "
        "an accepted substitute.")
endif()
cmake_path(ABSOLUTE_PATH BLOOM_DEPENDENCY_PREFIX NORMALIZE)
if(NOT IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}")
    message(FATAL_ERROR "BLOOM_DEPENDENCY_PREFIX does not exist: ${BLOOM_DEPENDENCY_PREFIX}")
endif()

# Resolve OpenColorIO and its transitive closure only from the locked prefix. This mirrors the
# qualified-mode search restriction in cmake/BloomDependencyPrefix.cmake (function-scoped so
# nothing leaks to the caller): environment search paths, CMake system paths, the install-prefix
# root, and both package registries are disabled; CMAKE_PREFIX_PATH and ZLIB_ROOT point at the
# prefix so OpenColorIO's nested find_dependency(expat/Imath/pystring/yaml-cpp/ZLIB/minizip-ng)
# calls cannot silently fall through to host packages.
function(bloom_gpu_assert_imported_target_under_prefix target)
    get_target_property(_bloom_location "${target}" IMPORTED_LOCATION)
    if(NOT _bloom_location)
        get_target_property(_bloom_location "${target}" IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT _bloom_location)
        get_target_property(_bloom_location "${target}" IMPORTED_LOCATION_NOCONFIG)
    endif()
    if(NOT _bloom_location OR _bloom_location MATCHES "-NOTFOUND$")
        return()
    endif()
    cmake_path(IS_PREFIX BLOOM_DEPENDENCY_PREFIX "${_bloom_location}" NORMALIZE
        _bloom_target_under_prefix)
    if(NOT _bloom_target_under_prefix)
        message(FATAL_ERROR
            "dependency target ${target} resolved outside the locked prefix: ${_bloom_location}")
    endif()
endfunction()

function(bloom_gpu_find_locked_ocio)
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
    set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG OFF)
    set(ZLIB_ROOT "${BLOOM_DEPENDENCY_PREFIX}")
    set(CMAKE_PREFIX_PATH "${BLOOM_DEPENDENCY_PREFIX}")
    set(CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH OFF)
    set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
    set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)
    set(CMAKE_FIND_USE_INSTALL_PREFIX OFF)

    find_package(OpenColorIO 2.5.2 EXACT REQUIRED CONFIG
        PATHS "${BLOOM_DEPENDENCY_PREFIX}"
        NO_DEFAULT_PATH)

    get_target_property(_bloom_ocio_location OpenColorIO::OpenColorIO IMPORTED_LOCATION)
    if(NOT _bloom_ocio_location)
        get_target_property(_bloom_ocio_location OpenColorIO::OpenColorIO IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT _bloom_ocio_location OR _bloom_ocio_location MATCHES "-NOTFOUND$")
        message(FATAL_ERROR "Locked OpenColorIO has no imported library location.")
    endif()
    cmake_path(IS_PREFIX BLOOM_DEPENDENCY_PREFIX "${_bloom_ocio_location}" NORMALIZE
        _bloom_ocio_under_prefix)
    if(NOT _bloom_ocio_under_prefix)
        message(FATAL_ERROR
            "OpenColorIO resolved outside the locked prefix: ${_bloom_ocio_location}")
    endif()

    foreach(_bloom_dependency_target
            expat::expat
            Imath::Imath
            pystring::pystring
            yaml-cpp::yaml-cpp
            MINIZIP::minizip-ng
            ZLIB::ZLIB)
        if(TARGET "${_bloom_dependency_target}")
            bloom_gpu_assert_imported_target_under_prefix("${_bloom_dependency_target}")
        endif()
    endforeach()
    if(DEFINED ZLIB_LIBRARY AND NOT ZLIB_LIBRARY STREQUAL "")
        cmake_path(IS_PREFIX BLOOM_DEPENDENCY_PREFIX "${ZLIB_LIBRARY}" NORMALIZE
            _bloom_zlib_under_prefix)
        if(NOT _bloom_zlib_under_prefix)
            message(FATAL_ERROR
                "ZLIB resolved outside the locked prefix: ${ZLIB_LIBRARY}; a transitive "
                "dependency must never escape ${BLOOM_DEPENDENCY_PREFIX}.")
        endif()
    endif()
endfunction()

bloom_gpu_find_locked_ocio()

# ---------------------------------------------------------------------------
# Frozen asset pin (raw config.ocio bytes), verified at configure time.
#
# The raw-bytes digest and the BloomOcioRevision v1 envelope digest are DISTINCT values: the
# first is SHA-256 over the file bytes alone; the second is SHA-256 over the domain-separated
# content-revision envelope documented in docs/architecture/color-management.md. Both are
# checked: this pin checks the raw bytes, and the generator independently rebuilds the revision
# envelope at runtime and compares it against the second constant.
# ---------------------------------------------------------------------------
set(BLOOM_NEUTRAL_V1_RAW_SHA256
    "6cb2d1460218958ffede3fb747f3a7d46d355ac0d3d5e6452eb9a5a47d63373f")
set(BLOOM_NEUTRAL_V1_REVISION_SHA256
    "a6fc07c95cea14897b725992a179d35055cccce2ffaf719ca73b4a289c89eacf")
set(BLOOM_NEUTRAL_V1_PAYLOAD_BYTE_COUNT 632)
set(BLOOM_NEUTRAL_V1_LOCATOR_KIND 1)
set(BLOOM_NEUTRAL_V1_REVISION_VERSION 1)

set(BLOOM_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
cmake_path(ABSOLUTE_PATH BLOOM_REPO_ROOT NORMALIZE)
set(BLOOM_NEUTRAL_V1_ASSET "${BLOOM_REPO_ROOT}/assets/ocio/neutral-v1/config.ocio")
if(NOT EXISTS "${BLOOM_NEUTRAL_V1_ASSET}")
    message(FATAL_ERROR "Frozen Bloom Neutral v1 asset is missing: ${BLOOM_NEUTRAL_V1_ASSET}")
endif()

file(SHA256 "${BLOOM_NEUTRAL_V1_ASSET}" BLOOM_NEUTRAL_V1_MEASURED_SHA256)
if(NOT BLOOM_NEUTRAL_V1_MEASURED_SHA256 STREQUAL BLOOM_NEUTRAL_V1_RAW_SHA256)
    message(FATAL_ERROR
        "Bloom Neutral v1 raw SHA-256 mismatch: measured ${BLOOM_NEUTRAL_V1_MEASURED_SHA256}, "
        "trusted ${BLOOM_NEUTRAL_V1_RAW_SHA256}. A content change requires a new built-in URI; "
        "never edit this payload in place.")
endif()
file(SIZE "${BLOOM_NEUTRAL_V1_ASSET}" BLOOM_NEUTRAL_V1_MEASURED_BYTE_COUNT)
if(NOT BLOOM_NEUTRAL_V1_MEASURED_BYTE_COUNT EQUAL BLOOM_NEUTRAL_V1_PAYLOAD_BYTE_COUNT)
    message(FATAL_ERROR
        "Bloom Neutral v1 byte count mismatch: measured ${BLOOM_NEUTRAL_V1_MEASURED_BYTE_COUNT}, "
        "trusted ${BLOOM_NEUTRAL_V1_PAYLOAD_BYTE_COUNT}.")
endif()

set(BLOOM_PINS_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${BLOOM_PINS_DIR}")
file(WRITE "${BLOOM_PINS_DIR}/bloom_neutral_v1_pins.hpp"
"#pragma once\n"
"// Generated by tools/gpu-shaders/CMakeLists.txt at configure time. Do not edit.\n"
"#define BLOOM_NEUTRAL_V1_RAW_SHA256 \"${BLOOM_NEUTRAL_V1_RAW_SHA256}\"\n"
"#define BLOOM_NEUTRAL_V1_REVISION_SHA256 \"${BLOOM_NEUTRAL_V1_REVISION_SHA256}\"\n"
"#define BLOOM_NEUTRAL_V1_PAYLOAD_BYTE_COUNT ${BLOOM_NEUTRAL_V1_PAYLOAD_BYTE_COUNT}\n"
"#define BLOOM_NEUTRAL_V1_LOCATOR_KIND ${BLOOM_NEUTRAL_V1_LOCATOR_KIND}\n"
"#define BLOOM_NEUTRAL_V1_REVISION_VERSION ${BLOOM_NEUTRAL_V1_REVISION_VERSION}\n")

# ---------------------------------------------------------------------------
# Generator
# ---------------------------------------------------------------------------
set(BLOOM_CORE_DIR "${BLOOM_REPO_ROOT}/src/core")
add_executable(bloom_gpu_neutral_display_generator
    neutral_display_generator.cpp
    "${BLOOM_CORE_DIR}/sha256.cpp")
target_include_directories(bloom_gpu_neutral_display_generator
    PRIVATE "${BLOOM_PINS_DIR}" "${BLOOM_CORE_DIR}/include")
target_link_libraries(bloom_gpu_neutral_display_generator PRIVATE OpenColorIO::OpenColorIO)
if(MSVC)
    target_compile_options(bloom_gpu_neutral_display_generator PRIVATE /W4 /WX)
else()
    target_compile_options(bloom_gpu_neutral_display_generator PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# Optional offline verification: generate twice, compile with the locked
# glslangValidator (Vulkan 1.2 target), and validate with the locked spirv-val.
# ---------------------------------------------------------------------------
find_program(BLOOM_GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
find_program(BLOOM_SPIRV_VAL
    NAMES spirv-val
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
find_program(BLOOM_SPIRV_DIS
    NAMES spirv-dis
    HINTS "${BLOOM_DEPENDENCY_PREFIX}/bin"
    NO_DEFAULT_PATH)
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL)
    set(BLOOM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
    add_custom_target(bloom_gpu_neutral_display_verify
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${BLOOM_GENERATED_DIR}/a" "${BLOOM_GENERATED_DIR}/b"
        COMMAND bloom_gpu_neutral_display_generator
                --asset "${BLOOM_NEUTRAL_V1_ASSET}"
                --output-dir "${BLOOM_GENERATED_DIR}/a"
        COMMAND bloom_gpu_neutral_display_generator
                --asset "${BLOOM_NEUTRAL_V1_ASSET}"
                --output-dir "${BLOOM_GENERATED_DIR}/b"
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${BLOOM_GENERATED_DIR}/a/neutral_display.comp"
                "${BLOOM_GENERATED_DIR}/b/neutral_display.comp"
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${BLOOM_GENERATED_DIR}/a/neutral_display.manifest"
                "${BLOOM_GENERATED_DIR}/b/neutral_display.manifest"
        COMMAND "${BLOOM_GLSLANG_VALIDATOR}" --target-env vulkan1.2 -V
                "${BLOOM_GENERATED_DIR}/a/neutral_display.comp"
                -o "${BLOOM_GENERATED_DIR}/a/neutral_display.spv"
        COMMAND "${BLOOM_SPIRV_VAL}" --target-env vulkan1.2
                "${BLOOM_GENERATED_DIR}/a/neutral_display.spv"
        DEPENDS bloom_gpu_neutral_display_generator
        COMMENT "Generating twice, diffing, compiling with glslangValidator, and validating SPIR-V"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# BlendV1 SPIR-V embed generator
#
# Deterministic offline embed of the hand-written blend*.comp kernels. It drives the pinned
# glslangValidator/spirv-val/spirv-dis itself and writes the checked-in .inc array plus the manifest
# pins, or --check verifies them. It reuses bloom::core::Sha256Hasher read-only, like the neutral
# display generator, and needs no OpenColorIO.
# ---------------------------------------------------------------------------
add_executable(bloom_gpu_blend_spirv_generator
    embed_blend_spirv.cpp
    "${BLOOM_CORE_DIR}/sha256.cpp")
target_include_directories(bloom_gpu_blend_spirv_generator PRIVATE "${BLOOM_CORE_DIR}/include")
if(MSVC)
    target_compile_options(bloom_gpu_blend_spirv_generator PRIVATE /W4 /WX)
else()
    target_compile_options(bloom_gpu_blend_spirv_generator PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# Optional offline verification for every blend family: --check the checked-in
# .inc/manifest against a fresh pinned compile, then generate each family twice
# and require byte-identical .inc and manifest outputs.
# ---------------------------------------------------------------------------
if(BLOOM_GLSLANG_VALIDATOR AND BLOOM_SPIRV_VAL AND BLOOM_SPIRV_DIS)
    set(BLOOM_BLEND_SHADER_DIR "${BLOOM_REPO_ROOT}/src/render/vulkan/shaders")
    set(BLOOM_BLEND_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/blend-spirv-verify")
    add_custom_target(bloom_gpu_blend_spirv_verify
        COMMAND "${CMAKE_COMMAND}"
                "-DBLOOM_BLEND_GENERATOR=$<TARGET_FILE:bloom_gpu_blend_spirv_generator>"
                "-DBLOOM_BLEND_GLSLANG=${BLOOM_GLSLANG_VALIDATOR}"
                "-DBLOOM_BLEND_SPIRV_VAL=${BLOOM_SPIRV_VAL}"
                "-DBLOOM_BLEND_SPIRV_DIS=${BLOOM_SPIRV_DIS}"
                "-DBLOOM_BLEND_SHADER_DIR=${BLOOM_BLEND_SHADER_DIR}"
                "-DBLOOM_BLEND_WORK_DIR=${BLOOM_BLEND_WORK_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_blend_spirv.cmake"
        DEPENDS bloom_gpu_blend_spirv_generator
        COMMENT "Checking and regenerating every BlendV1 SPIR-V embed twice"
        VERBATIM)
else()
    message(STATUS
        "bloom_gpu_blend_spirv_verify is unavailable: glslangValidator/spirv-val/spirv-dis not "
        "found in ${BLOOM_DEPENDENCY_PREFIX}/bin")
endif()
