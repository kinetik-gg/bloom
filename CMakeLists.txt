cmake_minimum_required(VERSION 3.25)

project(
    Bloom
    VERSION 0.1.0
    DESCRIPTION "Artist-focused motion graphics and visual effects software"
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BLOOM_BUILD_DESKTOP "Build the Qt desktop application and UI tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(BloomQuality)
include(BloomWarnings)
include(BloomDependencyPrefix)

# Dependency-prefix consumption is implemented, so an explicit mode is required: 'qualified'
# validates the reviewed lock and resolves dependency packages only from the superbuild prefix;
# 'developer-system' iterates against host packages and is labeled Unqualified. CI, release,
# package, and conformance builds use qualified mode.
bloom_consume_dependency_prefix()

bloom_configure_quality_tools()

add_subdirectory(tools/quality/json)
add_subdirectory(tools/quality/project_schema_checks)
add_subdirectory(tools/quality/repository_checks)

add_subdirectory(src/core)
add_subdirectory(tools/quality/dependency_artifact_checks)
add_subdirectory(src/platform)
add_subdirectory(src/document)
add_subdirectory(src/project)
# src/color must precede src/runtime: bloom_runtime_evaluator's qualified display-preparation
# stage (issue #97, task C3) links bloom_color_ocio, and target_link_libraries() requires that
# target to already exist. src/color itself only depends on src/core/src/render (both already
# added above), never on src/runtime, so this reordering introduces no cycle.
add_subdirectory(src/render)
add_subdirectory(src/color)
add_subdirectory(src/runtime)
add_subdirectory(src/output)
add_subdirectory(src/commands)
add_subdirectory(src/host)

add_custom_target(
    bloom-quality-check
    DEPENDS
        bloom-format-check
        bloom-hygiene-check
        bloom-architecture-check
        bloom-project-schema-check
        bloom-dependency-artifact-check
)

if(BLOOM_BUILD_DESKTOP)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
    qt_standard_project_setup()

    add_subdirectory(src/ui)
    add_subdirectory(apps/bloom)

    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()
