include_guard(GLOBAL)

option(BLOOM_WARNINGS_AS_ERRORS "Treat warnings in Bloom-owned targets as errors" ON)

function(bloom_enable_warnings target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "bloom_enable_warnings expected an existing target: ${target}")
    endif()

    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE
                /W4
                /permissive-
                /w14242
                /w14254
                /w14263
                /w14265
                /w14287
                /w14296
                /w14311
                /w14545
                /w14546
                /w14547
                /w14549
                /w14555
                /w14619
                /w14640
                /w14826
                /w14905
                /w14906
                /w14928
                /external:anglebrackets
                /external:W0
        )
        if(BLOOM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wcast-align
                -Wcast-qual
                -Wconversion
                -Wdouble-promotion
                -Wextra-semi
                -Wformat=2
                -Wimplicit-fallthrough
                -Wmissing-declarations
                -Wnon-virtual-dtor
                -Wnull-dereference
                -Wold-style-cast
                -Woverloaded-virtual
                -Wshadow
                -Wsign-conversion
                -Wundef
        )
        if(BLOOM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wcast-align=strict
                -Wcast-qual
                -Wconversion
                -Wdouble-promotion
                -Wduplicated-cond
                -Wduplicated-branches
                -Wformat=2
                -Wimplicit-fallthrough
                -Wlogical-op
                -Wmissing-declarations
                -Wnon-virtual-dtor
                -Wnull-dereference
                -Wold-style-cast
                -Woverloaded-virtual
                -Wshadow
                -Wsign-conversion
                -Wundef
                -Wuseless-cast
        )
        if(BLOOM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    else()
        message(WARNING "Bloom has no strict warning profile for ${CMAKE_CXX_COMPILER_ID}")
    endif()

    bloom_enable_clang_tidy(${target})
endfunction()
