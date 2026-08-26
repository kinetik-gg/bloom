# Standalone qualified-consumption smoke consumer. Not part of the root Bloom build: CI
# configures this project against the superbuild prefix in qualified mode to prove the
# lock-validated consume path end to end (mode enforcement, restricted package search, imported
# target resolution, and a runtime parse call).
cmake_minimum_required(VERSION 3.25)

project(BloomDependencyConsumptionSmoke LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/BloomDependencyPrefix.cmake")
bloom_consume_dependency_prefix()

find_package(yyjson REQUIRED CONFIG)

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
