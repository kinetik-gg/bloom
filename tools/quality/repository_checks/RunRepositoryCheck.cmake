foreach(required_variable IN ITEMS BLOOM_REPOSITORY_CHECKER BLOOM_REPOSITORY_ROOT
                                   BLOOM_REPOSITORY_FILE_MANIFEST)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(checker_arguments --root "${BLOOM_REPOSITORY_ROOT}")
find_program(BLOOM_GIT_EXECUTABLE NAMES git)
if(BLOOM_GIT_EXECUTABLE AND EXISTS "${BLOOM_REPOSITORY_ROOT}/.git")
    get_filename_component(manifest_directory "${BLOOM_REPOSITORY_FILE_MANIFEST}" DIRECTORY)
    file(MAKE_DIRECTORY "${manifest_directory}")
    execute_process(
        COMMAND
            "${BLOOM_GIT_EXECUTABLE}"
            -C
            "${BLOOM_REPOSITORY_ROOT}"
            ls-files
            --cached
            --others
            --exclude-standard
            -z
        OUTPUT_FILE "${BLOOM_REPOSITORY_FILE_MANIFEST}"
        RESULT_VARIABLE git_result
        ERROR_QUIET
    )
    if(git_result EQUAL 0)
        list(APPEND checker_arguments --files-from "${BLOOM_REPOSITORY_FILE_MANIFEST}")
    endif()
endif()

execute_process(
    COMMAND "${BLOOM_REPOSITORY_CHECKER}" ${checker_arguments}
    RESULT_VARIABLE checker_result
)
if(NOT checker_result EQUAL 0)
    message(FATAL_ERROR "Repository checker failed with exit code ${checker_result}")
endif()
