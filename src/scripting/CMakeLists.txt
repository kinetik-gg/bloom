add_library(bloom_scripting STATIC)

target_sources(
    bloom_scripting
    PRIVATE
        json_script.cpp
        operation_registry.cpp
        render.cpp
        session.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/scripting/events.hpp
            include/bloom/scripting/facade.hpp
            include/bloom/scripting/json_script.hpp
            include/bloom/scripting/operation_registry.hpp
            include/bloom/scripting/query.hpp
            include/bloom/scripting/render.hpp
            include/bloom/scripting/session.hpp
            include/bloom/scripting/tasks.hpp
            include/bloom/scripting/value.hpp
)

target_compile_features(bloom_scripting PUBLIC cxx_std_20)
target_link_libraries(
    bloom_scripting
    PUBLIC bloom_commands bloom_core bloom_document bloom_host bloom_runtime
)
bloom_enable_warnings(bloom_scripting)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_scripting_facade_test tests/facade_tests.cpp)
    target_link_libraries(bloom_scripting_facade_test PRIVATE bloom_scripting)
    bloom_enable_warnings(bloom_scripting_facade_test)
    bloom_add_test(
        NAME bloom.scripting.facade
        COMMAND bloom_scripting_facade_test
        LABELS unit scripting host commands document
    )
endif()
