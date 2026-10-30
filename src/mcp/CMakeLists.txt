# ADR 0034 (proposed): the shared Qt-free MCP server core (JSON-RPC, handshake, tools, schemas,
# projections, stdio transport) and the in-app Streamable HTTP server service. Only the contract
# headers exist so far, so the module is an interface library; the implementation turns it into a
# static library with the same name and dependencies.
add_library(bloom_mcp INTERFACE)

target_sources(
    bloom_mcp
    INTERFACE
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/mcp/protocol.hpp
            include/bloom/mcp/server_core.hpp
            include/bloom/mcp/server_service.hpp
            include/bloom/mcp/streamable_http.hpp
)

target_compile_features(bloom_mcp INTERFACE cxx_std_20)
target_link_libraries(bloom_mcp INTERFACE bloom_scripting bloom_host bloom_platform bloom_core)

if(BUILD_TESTING)
    include(BloomTesting)
    # ADR 0034 (proposed): the MCP contract headers compile and their value types and constexpr
    # helpers behave as documented. The server itself is not implemented yet.
    add_executable(bloom_mcp_server_contract_test tests/mcp_server_contract_tests.cpp)
    target_link_libraries(bloom_mcp_server_contract_test PRIVATE bloom_mcp)
    bloom_enable_warnings(bloom_mcp_server_contract_test)
    bloom_add_test(
        NAME bloom.mcp.server-contract
        COMMAND bloom_mcp_server_contract_test
        LABELS unit mcp
    )
endif()
