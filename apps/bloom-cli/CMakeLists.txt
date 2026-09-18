add_executable(bloom-cli main.cpp)

target_link_libraries(bloom-cli PRIVATE bloom_scripting)
bloom_enable_warnings(bloom-cli)

install(TARGETS bloom-cli RUNTIME DESTINATION bin)
