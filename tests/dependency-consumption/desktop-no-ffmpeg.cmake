if(NOT DEFINED BLOOM_REPOSITORY_ROOT)
    message(FATAL_ERROR "BLOOM_REPOSITORY_ROOT is required")
endif()

file(GLOB_RECURSE bloom_desktop_cmake_files LIST_DIRECTORIES false
    "${BLOOM_REPOSITORY_ROOT}/apps/bloom/CMakeLists.txt"
    "${BLOOM_REPOSITORY_ROOT}/apps/bloom/*.cmake")
foreach(bloom_desktop_cmake_file IN LISTS bloom_desktop_cmake_files)
    file(READ "${bloom_desktop_cmake_file}" bloom_desktop_cmake_text)
    if(bloom_desktop_cmake_text MATCHES "FFmpeg|FFMPEG|libav")
        message(FATAL_ERROR
            "desktop CMake boundary mentions FFmpeg or libav: ${bloom_desktop_cmake_file}")
    endif()
endforeach()

if(DEFINED BLOOM_DESKTOP_BINARY AND EXISTS "${BLOOM_DESKTOP_BINARY}")
    execute_process(
        COMMAND ldd "${BLOOM_DESKTOP_BINARY}"
        RESULT_VARIABLE bloom_ldd_result
        OUTPUT_VARIABLE bloom_ldd_output
        ERROR_VARIABLE bloom_ldd_error)
    if(NOT bloom_ldd_result EQUAL 0)
        message(FATAL_ERROR "ldd failed for ${BLOOM_DESKTOP_BINARY}: ${bloom_ldd_error}")
    endif()
    if(bloom_ldd_output MATCHES "libav(codec|device|filter|format|util|swresample|swscale)")
        message(FATAL_ERROR
            "desktop binary links an FFmpeg library: ${BLOOM_DESKTOP_BINARY}")
    endif()
endif()

message(STATUS "Desktop dependency boundary contains no FFmpeg or libav references")
