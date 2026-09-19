# Standalone qualified-consumption smoke consumer. Not part of the root Bloom build: CI
# configures this project against the superbuild prefix in qualified mode to prove the
# lock-validated consume path end to end (mode enforcement, restricted package search, imported
# target resolution, and a runtime parse call).
cmake_minimum_required(VERSION 3.25)

project(BloomDependencyConsumptionSmoke LANGUAGES CXX)

include(CTest)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BloomDependencyPrefix.cmake")
bloom_consume_dependency_prefix()
bloom_find_dependency(yyjson)
bloom_find_dependency(FFmpeg)

file(WRITE "${CMAKE_BINARY_DIR}/consume_smoke.cpp" [[
#include <yyjson.h>

int main() {
    const char document[] = "{\"bloom\":[1,2,3]}";
    yyjson_doc* parsed = yyjson_read(document, sizeof(document) - 1, 0);
    const bool valid = parsed != nullptr && yyjson_is_obj(yyjson_doc_get_root(parsed));
    yyjson_doc_free(parsed);
    return valid ? 0 : 1;
}
]])

add_executable(bloom_consume_smoke "${CMAKE_BINARY_DIR}/consume_smoke.cpp")
target_link_libraries(bloom_consume_smoke PRIVATE yyjson::yyjson)
target_compile_features(bloom_consume_smoke PRIVATE cxx_std_20)

file(WRITE "${CMAKE_BINARY_DIR}/ffmpeg_consume_smoke.cpp" [[
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

int main() {
    return avcodec_version() != 0 && avformat_version() != 0 ? 0 : 1;
}
]])

add_executable(bloom_ffmpeg_consume_smoke "${CMAKE_BINARY_DIR}/ffmpeg_consume_smoke.cpp")
target_link_libraries(bloom_ffmpeg_consume_smoke PRIVATE FFmpeg::avcodec FFmpeg::avformat)
target_compile_features(bloom_ffmpeg_consume_smoke PRIVATE cxx_std_20)

# libavcodec records DT_NEEDED libopenh264.so.8, but the qualified prefix deliberately carries only
# a text import stub and never an OpenH264 runtime binary (see openh264-boundary.cmake). The worker
# runtime directory holds the six-symbol OpenH264 shim that satisfies the two symbols libavcodec
# references, so resolve the link and run against it when the caller supplies that directory.
set(BLOOM_FFMPEG_CONSUME_LIBRARY_PATH "${BLOOM_DEPENDENCY_PREFIX}/lib")
if(DEFINED BLOOM_MEDIA_WORKER_RUNTIME)
    target_link_directories(bloom_ffmpeg_consume_smoke PRIVATE "${BLOOM_MEDIA_WORKER_RUNTIME}")
    target_link_options(bloom_ffmpeg_consume_smoke PRIVATE
        "LINKER:-rpath-link,${BLOOM_MEDIA_WORKER_RUNTIME}")
    string(APPEND BLOOM_FFMPEG_CONSUME_LIBRARY_PATH ":${BLOOM_MEDIA_WORKER_RUNTIME}")
endif()

add_test(NAME bloom.dependency.ffmpeg-consume COMMAND bloom_ffmpeg_consume_smoke)
set_tests_properties(bloom.dependency.ffmpeg-consume PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${BLOOM_FFMPEG_CONSUME_LIBRARY_PATH}")

set(BLOOM_MEDIA_WORKER_RUNTIME_ARGUMENT)
if(DEFINED BLOOM_MEDIA_WORKER_RUNTIME)
    list(APPEND BLOOM_MEDIA_WORKER_RUNTIME_ARGUMENT
         -DBLOOM_MEDIA_WORKER_RUNTIME=${BLOOM_MEDIA_WORKER_RUNTIME})
endif()
add_test(
    NAME bloom.dependency.openh264-boundary
    COMMAND ${CMAKE_COMMAND}
        -DBLOOM_DEPENDENCY_PREFIX=${BLOOM_DEPENDENCY_PREFIX}
        ${BLOOM_MEDIA_WORKER_RUNTIME_ARGUMENT}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/openh264-boundary.cmake")

get_filename_component(BLOOM_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(BLOOM_DESKTOP_BINARY_ARGUMENT)
if(DEFINED BLOOM_DESKTOP_BINARY)
    list(APPEND BLOOM_DESKTOP_BINARY_ARGUMENT
        -DBLOOM_DESKTOP_BINARY=${BLOOM_DESKTOP_BINARY})
endif()
add_test(
    NAME bloom.dependency.desktop-no-ffmpeg
    COMMAND ${CMAKE_COMMAND}
        -DBLOOM_REPOSITORY_ROOT=${BLOOM_REPOSITORY_ROOT}
        ${BLOOM_DESKTOP_BINARY_ARGUMENT}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/desktop-no-ffmpeg.cmake")
