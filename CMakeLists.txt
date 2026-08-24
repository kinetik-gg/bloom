cmake_minimum_required(VERSION 3.25)

project(
    Bloom
    VERSION 0.1.0
    DESCRIPTION "Native motion, compositing, and editing software"
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(BloomQuality)
include(BloomWarnings)

bloom_configure_quality_tools()

add_subdirectory(src/core)
add_subdirectory(src/document)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

add_subdirectory(src/ui)
add_subdirectory(apps/bloom)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
