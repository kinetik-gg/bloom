# ADR 0034: the shared Qt-free MCP server core (JSON-RPC, handshake, tools, schemas, projections,
# stdio transport), Streamable HTTP framing and admission, and the in-app server service.
bloom_find_dependency(yyjson)
add_library(bloom_mcp STATIC)

target_sources(
    bloom_mcp
    PRIVATE
        core_internal.hpp
        json.cpp
        json.hpp
        projections.cpp
        server_core.cpp
        server_http.cpp
        server_service.cpp
        server_service_state.hpp
        stdio_transport.cpp
        streamable_http.cpp
        tool_schemas.cpp
        tools_document.cpp
        tools_project.cpp
        tools_export.cpp
        tools_render.cpp
        tools_session.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/mcp/live_ui_port.hpp
            include/bloom/mcp/protocol.hpp
            include/bloom/mcp/server_core.hpp
            include/bloom/mcp/server_service.hpp
            include/bloom/mcp/streamable_http.hpp
)

target_compile_features(bloom_mcp PUBLIC cxx_std_20)
target_link_libraries(bloom_mcp PUBLIC bloom_scripting bloom_host bloom_platform bloom_core)
target_link_libraries(bloom_mcp PRIVATE yyjson::yyjson)
bloom_enable_warnings(bloom_mcp)

# Test support: a driver that scripts a real McpServerService into every state the MCP Server panel
# shows (never linked into production).
add_library(bloom_mcp_testing STATIC testing/server_service_driver.cpp)
target_include_directories(bloom_mcp_testing PUBLIC testing/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bloom_mcp_testing PUBLIC bloom_mcp)
bloom_enable_warnings(bloom_mcp_testing)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_mcp_server_contract_test tests/mcp_server_contract_tests.cpp)
    target_link_libraries(bloom_mcp_server_contract_test PRIVATE bloom_mcp)
    bloom_enable_warnings(bloom_mcp_server_contract_test)
    bloom_add_test(
        NAME bloom.mcp.server-contract
        COMMAND bloom_mcp_server_contract_test
        LABELS unit mcp
    )
    # Streamable HTTP framing and admission against a hostile-input corpus (pure, byte strings).
    add_executable(bloom_mcp_core_tools_test tests/core_tools_tests.cpp)
    target_link_libraries(bloom_mcp_core_tools_test PRIVATE bloom_mcp bloom_host_export_testing)
    bloom_enable_warnings(bloom_mcp_core_tools_test)
    bloom_add_test(
        NAME bloom.mcp.core-tools
        COMMAND bloom_mcp_core_tools_test
        LABELS unit mcp
        TIMEOUT 120
    )
    add_executable(bloom_mcp_http_parser_test tests/http_parser_tests.cpp)
    target_link_libraries(bloom_mcp_http_parser_test PRIVATE bloom_mcp)
    bloom_enable_warnings(bloom_mcp_http_parser_test)
    bloom_add_test(
        NAME bloom.mcp.http-parser
        COMMAND bloom_mcp_http_parser_test
        LABELS unit mcp
    )
    add_executable(bloom_mcp_server_service_test tests/server_service_tests.cpp)
    target_link_libraries(bloom_mcp_server_service_test PRIVATE bloom_mcp_testing)
    bloom_enable_warnings(bloom_mcp_server_service_test)
    bloom_add_test(
        NAME bloom.mcp.server-service
        COMMAND bloom_mcp_server_service_test
        LABELS unit mcp
        TIMEOUT 120
    )
    # Streamable HTTP over a real loopback socket against a headless McpServerService.
    add_executable(bloom_mcp_http_test_host tests/http_test_host.cpp)
    target_link_libraries(bloom_mcp_http_test_host PRIVATE bloom_mcp)
    bloom_enable_warnings(bloom_mcp_http_test_host)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        bloom_add_test(
            NAME bloom.mcp.http-conformance
            COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tests/fixtures/scripting/mcp_http_conformance.py"
                $<TARGET_FILE:bloom_mcp_http_test_host>
            LABELS mcp integration
            TIMEOUT 180
        )
    endif()
endif()
