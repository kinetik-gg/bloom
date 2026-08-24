include_guard(GLOBAL)

function(bloom_add_test)
    set(options)
    set(one_value_args NAME TIMEOUT WORKING_DIRECTORY)
    set(multi_value_args COMMAND LABELS ENVIRONMENT)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "${options}" "${one_value_args}" "${multi_value_args}")

    if(NOT ARG_NAME)
        message(FATAL_ERROR "bloom_add_test requires NAME")
    endif()
    if(NOT ARG_COMMAND)
        message(FATAL_ERROR "bloom_add_test(${ARG_NAME}) requires COMMAND")
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()

    add_test(NAME "${ARG_NAME}" COMMAND ${ARG_COMMAND})
    set_tests_properties("${ARG_NAME}" PROPERTIES TIMEOUT "${ARG_TIMEOUT}")

    # Production targets are the always-on static-analysis boundary. Tests retain strict compiler
    # warnings, formatting, hygiene, and sanitizer coverage; their broader clang-tidy sweep is an
    # explicit opt-in so fixture assertions do not dilute the production signal.
    if(TARGET "${ARG_COMMAND}" AND NOT BLOOM_ENABLE_TEST_CLANG_TIDY)
        set_property(TARGET "${ARG_COMMAND}" PROPERTY CXX_CLANG_TIDY "")
    endif()

    if(ARG_LABELS)
        set_tests_properties("${ARG_NAME}" PROPERTIES LABELS "${ARG_LABELS}")
    endif()
    if(ARG_ENVIRONMENT)
        set_tests_properties("${ARG_NAME}" PROPERTIES ENVIRONMENT "${ARG_ENVIRONMENT}")
    endif()
    if(ARG_WORKING_DIRECTORY)
        set_tests_properties(
            "${ARG_NAME}"
            PROPERTIES WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}"
        )
    endif()
endfunction()
