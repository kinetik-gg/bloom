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
        stdio_transport.cpp
        streamable_http.cpp
        tool_schemas.cpp
        tools_document.cpp
        tools_export.cpp
        tools_render.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/mcp/protocol.hpp
            include/bloom/mcp/server_core.hpp
            include/bloom/mcp/server_service.hpp
            include/bloom/mcp/streamable_http.hpp
)

target_compile_features(bloom_mcp PUBLIC cxx_std_20)
target_link_libraries(bloom_mcp PUBLIC bloom_scripting bloom_host bloom_platform bloom_core)
target_link_libraries(bloom_mcp PRIVATE yyjson::yyjson)
bloom_enable_warnings(bloom_mcp)

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
    add_executable(bloom_mcp_http_parser_test tests/http_parser_tests.cpp)
    target_link_libraries(bloom_mcp_http_parser_test PRIVATE bloom_mcp)
    bloom_enable_warnings(bloom_mcp_http_parser_test)
    bloom_add_test(
        NAME bloom.mcp.http-parser
        COMMAND bloom_mcp_http_parser_test
        LABELS unit mcp
    )
endif()
