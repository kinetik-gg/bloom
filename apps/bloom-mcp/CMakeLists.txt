bloom_find_dependency(yyjson)
add_executable(bloom-mcp main.cpp json.cpp server.cpp tools.cpp projections.cpp render.cpp tool_schemas.cpp transport.cpp)
target_link_libraries(bloom-mcp PRIVATE bloom_scripting yyjson::yyjson)
target_compile_definitions(bloom-mcp PRIVATE BLOOM_VERSION="${PROJECT_VERSION}")
if(TARGET bloom-media-worker)
    add_dependencies(bloom-mcp bloom-media-worker)
endif()
bloom_enable_warnings(bloom-mcp)
install(TARGETS bloom-mcp RUNTIME DESTINATION bin)

# Stage the qualified GPU shader tools beside this executable so MCP-driven export shader
# compilation works out of the box; an unqualified or CPU-stub build gets a typed definition.
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom-mcp)

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
    if(BLOOM_GPU_SHADER_TOOLS_AVAILABLE)
        bloom_add_test(
            NAME bloom.gpu.shader-tools-verify-mcp
            COMMAND "${CMAKE_COMMAND}"
                "-DGPU_TOOLS_STAGED=$<TARGET_FILE_DIR:bloom-mcp>/bloom-gpu-tools"
                "-DGPU_TOOLS_PREFIX_BIN=${BLOOM_DEPENDENCY_PREFIX}/bin"
                "-DGPU_TOOLS_GLSLANG=glslangValidator${CMAKE_EXECUTABLE_SUFFIX}"
                "-DGPU_TOOLS_SPIRV=spirv-val${CMAKE_EXECUTABLE_SUFFIX}"
                -P "${PROJECT_SOURCE_DIR}/cmake/tests/verify_gpu_shader_tools.cmake"
            LABELS gpu unit)
    endif()
endif()
