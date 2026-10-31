# Post-install normalisation for a static codec archive (codec-common.cmake):
#   -DCODEC_PREFIX=<install prefix> -DCODEC_LIBRARY=<name> -DCODEC_PKGCONFIG=<pc name>
#   -DCODEC_PRIVATE_LIBS=<extra Libs.private, space separated> -DCODEC_WINDOWS=<ON|OFF>
# It appends the runtime libraries a static archive needs to the pkg-config record FFmpeg reads,
# gives MSVC's FFmpeg link the `<name>.lib` spelling it expects, and refuses a shared artifact so
# the codec stays inside libavcodec.
foreach(required CODEC_PREFIX CODEC_LIBRARY CODEC_PKGCONFIG)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "codec-static-install.cmake requires ${required}")
    endif()
endforeach()

set(codec_pc "${CODEC_PREFIX}/lib/pkgconfig/${CODEC_PKGCONFIG}.pc")
if(NOT EXISTS "${codec_pc}")
    message(FATAL_ERROR "${CODEC_LIBRARY}: missing pkg-config record ${codec_pc}")
endif()
if(CODEC_PRIVATE_LIBS)
    # Idempotent: a reinstall over an existing record adds only the libraries still missing.
    separate_arguments(codec_private UNIX_COMMAND "${CODEC_PRIVATE_LIBS}")
    file(STRINGS "${codec_pc}" codec_lines)
    set(codec_text)
    set(codec_found FALSE)
    foreach(line IN LISTS codec_lines)
        if(line MATCHES "^Libs\\.private:(.*)$")
            separate_arguments(codec_existing UNIX_COMMAND "${CMAKE_MATCH_1}")
            set(codec_merged ${codec_existing})
            foreach(library IN LISTS codec_private)
                if(NOT library IN_LIST codec_merged)
                    list(APPEND codec_merged "${library}")
                endif()
            endforeach()
            list(JOIN codec_merged " " codec_merged)
            set(line "Libs.private: ${codec_merged}")
            set(codec_found TRUE)
        endif()
        string(APPEND codec_text "${line}\n")
    endforeach()
    if(NOT codec_found)
        list(JOIN codec_private " " codec_private)
        string(APPEND codec_text "Libs.private: ${codec_private}\n")
    endif()
    file(WRITE "${codec_pc}" "${codec_text}")
endif()

if(CODEC_WINDOWS)
    if(EXISTS "${CODEC_PREFIX}/lib/lib${CODEC_LIBRARY}.a")
        file(COPY_FILE "${CODEC_PREFIX}/lib/lib${CODEC_LIBRARY}.a"
            "${CODEC_PREFIX}/lib/${CODEC_LIBRARY}.lib")
    endif()
    set(codec_shared "${CODEC_PREFIX}/bin/${CODEC_LIBRARY}*.dll")
else()
    set(codec_shared "${CODEC_PREFIX}/lib/lib${CODEC_LIBRARY}.so*"
        "${CODEC_PREFIX}/lib/lib${CODEC_LIBRARY}*.dylib")
endif()
file(GLOB codec_shared_files ${codec_shared})
if(codec_shared_files)
    message(FATAL_ERROR "${CODEC_LIBRARY} must install only a static archive: ${codec_shared_files}")
endif()
