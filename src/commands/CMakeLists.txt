add_library(bloom_commands STATIC)

target_sources(
    bloom_commands
    PRIVATE
        command_stack.cpp
        operations.cpp
        result.cpp
        transaction.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/commands/command_stack.hpp
            include/bloom/commands/operation.hpp
            include/bloom/commands/operations.hpp
            include/bloom/commands/result.hpp
            include/bloom/commands/transaction.hpp
)

target_compile_features(bloom_commands PUBLIC cxx_std_20)
target_link_libraries(bloom_commands PUBLIC bloom_document)
bloom_enable_warnings(bloom_commands)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
