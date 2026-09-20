add_executable(bloom-cli main.cpp)

target_link_libraries(bloom-cli PRIVATE bloom_scripting)
bloom_enable_warnings(bloom-cli)

install(TARGETS bloom-cli RUNTIME DESTINATION bin)

if(BLOOM_BUILD_PYTHON)
    target_link_libraries(bloom-cli PRIVATE bloom_python_runtime)
    target_compile_definitions(bloom-cli PRIVATE BLOOM_BUILD_PYTHON=1)
endif()

# Stage the qualified GPU shader tools beside this executable so CLI/export shader compilation
# works out of the box; an unqualified or CPU-stub build gets a typed unavailable definition.
include(BloomGpuShaderTools)
bloom_package_gpu_shader_tools(bloom-cli)

if(BUILD_TESTING)
    include(BloomTesting)
    if(BLOOM_GPU_SHADER_TOOLS_AVAILABLE)
        bloom_add_test(
            NAME bloom.gpu.shader-tools-verify-cli
            COMMAND "${CMAKE_COMMAND}"
                "-DGPU_TOOLS_STAGED=$<TARGET_FILE_DIR:bloom-cli>/bloom-gpu-tools"
                "-DGPU_TOOLS_PREFIX_BIN=${BLOOM_DEPENDENCY_PREFIX}/bin"
                "-DGPU_TOOLS_GLSLANG=glslangValidator${CMAKE_EXECUTABLE_SUFFIX}"
                "-DGPU_TOOLS_SPIRV=spirv-val${CMAKE_EXECUTABLE_SUFFIX}"
                -P "${PROJECT_SOURCE_DIR}/cmake/tests/verify_gpu_shader_tools.cmake"
            LABELS gpu unit)
    endif()
    foreach(bloom_gpu_tools_case IN ITEMS missing-tools cpu-stub unqualified-mode)
        set(bloom_gpu_tools_vulkan ON)
        if(bloom_gpu_tools_case STREQUAL "missing-tools")
            set(bloom_gpu_tools_prefix "${CMAKE_CURRENT_BINARY_DIR}")
            set(bloom_gpu_tools_mode "qualified")
        elseif(bloom_gpu_tools_case STREQUAL "cpu-stub")
            set(bloom_gpu_tools_prefix "${BLOOM_DEPENDENCY_PREFIX}")
            set(bloom_gpu_tools_mode "qualified")
            set(bloom_gpu_tools_vulkan OFF)
        else()
            set(bloom_gpu_tools_prefix "${BLOOM_DEPENDENCY_PREFIX}")
            set(bloom_gpu_tools_mode "developer-system")
        endif()
        set(bloom_gpu_tools_command "${CMAKE_COMMAND}"
            -DBLOOM_DEPENDENCY_MODE=${bloom_gpu_tools_mode}
            "-DBLOOM_DEPENDENCY_PREFIX=${bloom_gpu_tools_prefix}"
            -DBLOOM_RENDER_ENABLE_VULKAN=${bloom_gpu_tools_vulkan}
            -DEXPECT_REASON=${bloom_gpu_tools_case})
        list(APPEND bloom_gpu_tools_command -P
            "${PROJECT_SOURCE_DIR}/cmake/tests/probe_gpu_shader_tools.cmake")
        bloom_add_test(
            NAME bloom.gpu.shader-tools-${bloom_gpu_tools_case}
            COMMAND ${bloom_gpu_tools_command}
            LABELS gpu unit)
    endforeach()
    bloom_add_test(
        NAME bloom.gpu.shader-tools-reconfigure
        COMMAND "${CMAKE_COMMAND}"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/gpu-tools-reconfigure"
            "-DHARNESS_DIR=${PROJECT_SOURCE_DIR}/cmake/tests/gpu_tools_reconfigure"
            "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/reconfigure_gpu_shader_tools.cmake"
        LABELS gpu unit)
    bloom_add_test(
        NAME bloom.gpu.shader-tools-floor
        COMMAND "${CMAKE_COMMAND}"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/gpu-tools-floor"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/floor_gpu_shader_tools.cmake"
        LABELS gpu unit)
    bloom_add_test(
        NAME bloom.gpu.shader-tools-relocation-identity
        COMMAND "${CMAKE_COMMAND}"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/gpu-tools-relocation"
            -P "${PROJECT_SOURCE_DIR}/cmake/tests/relocation_gpu_shader_tools.cmake"
        LABELS gpu unit)
endif()
