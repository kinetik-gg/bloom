add_executable(bloom-cli main.cpp)

target_link_libraries(bloom-cli PRIVATE bloom_scripting)
bloom_enable_warnings(bloom-cli)

install(TARGETS bloom-cli RUNTIME DESTINATION bin)

if(BLOOM_BUILD_PYTHON)
    target_link_libraries(bloom-cli PRIVATE bloom_python_runtime)
    target_compile_definitions(bloom-cli PRIVATE BLOOM_BUILD_PYTHON=1)
endif()
