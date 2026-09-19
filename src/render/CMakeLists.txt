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
        image.cpp
        image_types.cpp
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
            include/bloom/render/gpu_device.hpp
            include/bloom/render/image.hpp
            include/bloom/render/image_types.hpp
            include/bloom/render/text_raster.hpp
            include/bloom/render/path_raster.hpp
)

# Exactly one of the two implementations defines the shared API symbols. The Vulkan translation
# unit keeps its include requirements PRIVATE so no Vulkan header reaches bloom_render consumers or
# the desktop application.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    target_sources(bloom_render PRIVATE vulkan/gpu_device.cpp vulkan/gpu_device_private.hpp)
else()
    target_sources(bloom_render PRIVATE gpu_device_stub.cpp)
endif()

target_compile_features(bloom_render PUBLIC cxx_std_20)
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
    add_subdirectory(tests)
endif()
