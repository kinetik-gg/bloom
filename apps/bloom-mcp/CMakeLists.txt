bloom_find_dependency(yyjson)
add_executable(bloom-mcp main.cpp json.cpp server.cpp tools.cpp projections.cpp render.cpp tool_schemas.cpp transport.cpp)
target_link_libraries(bloom-mcp PRIVATE bloom_scripting yyjson::yyjson)
target_compile_definitions(bloom-mcp PRIVATE BLOOM_VERSION="${PROJECT_VERSION}")
if(TARGET bloom-media-worker)
    add_dependencies(bloom-mcp bloom-media-worker)
endif()
bloom_enable_warnings(bloom-mcp)
install(TARGETS bloom-mcp RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    include(BloomTesting)
    bloom_add_test(NAME bloom.scripting.mcp-pipes
        COMMAND "${Python3_EXECUTABLE}" -I
            "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/mcp_protocol.py"
            $<TARGET_FILE:bloom-mcp> $<TARGET_FILE:bloom-cli>
        LABELS scripting mcp integration)
endif()
