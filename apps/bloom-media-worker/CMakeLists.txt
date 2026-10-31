# Codec-bearing TUs belong exclusively to this worker-private target. The desktop and host
# contract/pool targets must never link it.
add_library(bloom_media_fake_worker_provider STATIC fake_provider.cpp)
target_link_libraries(bloom_media_fake_worker_provider PRIVATE bloom_media_provider)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${PROJECT_SOURCE_DIR}/dependencies/dependencies.lock.json")
file(SHA256 "${PROJECT_SOURCE_DIR}/dependencies/dependencies.lock.json" bloom_media_lock_digest)
# Bind source generation and compiler configuration without embedding host paths in the key.
file(GLOB_RECURSE bloom_media_identity_sources CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/media/provider/*.cpp"
     "${PROJECT_SOURCE_DIR}/src/media/provider/*.hpp"
     "${PROJECT_SOURCE_DIR}/src/platform/process_*.cpp"
     "${PROJECT_SOURCE_DIR}/src/platform/file_snapshot.cpp"
     "${PROJECT_SOURCE_DIR}/src/platform/include/bloom/platform/file_snapshot.hpp"
     "${PROJECT_SOURCE_DIR}/src/platform/include/bloom/platform/process_*.hpp"
     "${PROJECT_SOURCE_DIR}/src/core/sha256.cpp"
     "${PROJECT_SOURCE_DIR}/src/core/include/bloom/core/sha256.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*encode.ipp")
list(FILTER bloom_media_identity_sources EXCLUDE REGEX "/tests/")
list(APPEND bloom_media_identity_sources "${CMAKE_CURRENT_LIST_FILE}"
     "${PROJECT_SOURCE_DIR}/src/media/provider/CMakeLists.txt"
     "${PROJECT_SOURCE_DIR}/src/platform/CMakeLists.txt"
     "${PROJECT_SOURCE_DIR}/cmake/BloomWarnings.cmake")
list(SORT bloom_media_identity_sources)
string(TOUPPER "${CMAKE_BUILD_TYPE}" bloom_media_build_type)
set(bloom_media_build_material
    "${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}/${CMAKE_BUILD_TYPE}/${CMAKE_CXX_FLAGS}/${CMAKE_CXX_FLAGS_${bloom_media_build_type}}")
foreach(bloom_media_source IN LISTS bloom_media_identity_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bloom_media_source}")
    file(SHA256 "${bloom_media_source}" bloom_media_source_digest)
    file(RELATIVE_PATH bloom_media_source_name "${PROJECT_SOURCE_DIR}" "${bloom_media_source}")
    string(APPEND bloom_media_build_material "\n${bloom_media_source_name}:${bloom_media_source_digest}")
endforeach()
string(SHA256 bloom_media_build_digest "${bloom_media_build_material}")
set(bloom_media_build_id "fake-v1:${bloom_media_build_digest}")
target_compile_definitions(bloom_media_fake_worker_provider PRIVATE
    BLOOM_MEDIA_DEPENDENCY_LOCK="${bloom_media_lock_digest}"
    BLOOM_MEDIA_BUILD_ID="${bloom_media_build_id}"
    BLOOM_MEDIA_OS="${CMAKE_SYSTEM_NAME}" BLOOM_MEDIA_ARCH="${CMAKE_SYSTEM_PROCESSOR}")
bloom_enable_warnings(bloom_media_fake_worker_provider)
add_executable(bloom-media-fake-worker main.cpp)
target_compile_definitions(bloom-media-fake-worker PRIVATE BLOOM_FAKE_WORKER)
target_link_libraries(bloom-media-fake-worker PRIVATE bloom_media_provider bloom_media_fake_worker_provider bloom_platform_process)
bloom_enable_warnings(bloom-media-fake-worker)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_provider_fixtures.ipp")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_provider_fixtures.ipp" bloom_ffmpeg_fixture_digest)
set(bloom_encode_fixture_source "${PROJECT_SOURCE_DIR}/src/media/provider/tests/encode_tests.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bloom_encode_fixture_source}")
file(SHA256 "${bloom_encode_fixture_source}" bloom_ffmpeg_encode_fixture_digest)
# FFmpeg is the bundled provider on every platform and the last link of each platform's provider
# chain (ADR 0035). It stays private to this worker; the host sees only codec-free manifests.
set(bloom_ffmpeg_build_id "ffmpeg-v1:${bloom_media_build_digest}")
target_sources(bloom_media_provider PRIVATE "${PROJECT_SOURCE_DIR}/src/media/provider/ffmpeg_manifest.cpp")
target_compile_definitions(bloom_media_provider PRIVATE
    BLOOM_MEDIA_DEPENDENCY_LOCK="${bloom_media_lock_digest}"
    BLOOM_FFMPEG_BUILD_ID="${bloom_ffmpeg_build_id}"
    BLOOM_FFMPEG_FIXTURE_ID="${bloom_ffmpeg_fixture_digest}"
    BLOOM_FFMPEG_ENCODE_FIXTURE_ID="${bloom_ffmpeg_encode_fixture_digest}"
    BLOOM_MEDIA_OS="${CMAKE_SYSTEM_NAME}" BLOOM_MEDIA_ARCH="${CMAKE_SYSTEM_PROCESSOR}")
# Linux and every qualified build require the locked FFmpeg prefix. A macOS or Windows
# developer-system build without an FFmpeg package builds no FFmpeg worker; its provider chain then
# has only the platform provider, which a release build never ships.
set(bloom_ffmpeg_worker OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
    bloom_find_dependency(FFmpeg)
    set(bloom_ffmpeg_worker ON)
else()
    find_package(FFmpeg QUIET CONFIG)
    if(FFmpeg_FOUND)
        set(bloom_ffmpeg_worker ON)
    else()
        message(STATUS "FFmpeg package not found: this developer build has no FFmpeg media worker")
    endif()
endif()
# Installed worker generations, relative to the install prefix (media_workers.cpp resolves the
# same places relative to the running executable).
if(WIN32)
    set(bloom_media_install_root "bin/media")
elseif(APPLE)
    set(bloom_media_install_root "Bloom.app/Contents/libexec/bloom/media")
else()
    set(bloom_media_install_root "libexec/bloom/media")
endif()
if(bloom_ffmpeg_worker)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(BLOOM_OPENH264_SHIM_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib")
        add_subdirectory(openh264_shim)
        # FFmpeg's libopenh264 wrapper links the Linux import stub; Cisco's runtime binary or the
        # shim satisfies it at run time.
        set(bloom_ffmpeg_openh264_link "${BLOOM_DEPENDENCY_PREFIX}/lib/libopenh264.so")
    else()
        set(bloom_ffmpeg_openh264_link)
    endif()
    # The declared-sampling reopen comparison (tolerance profile v2): the encoder's output
    # conversion and the verifier's reference, shared by both provider builds and its unit test.
    add_library(bloom_media_ffmpeg_reopen_compare STATIC ffmpeg_reopen_compare.cpp)
    target_include_directories(bloom_media_ffmpeg_reopen_compare PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(bloom_media_ffmpeg_reopen_compare PUBLIC cxx_std_20)
    target_link_libraries(bloom_media_ffmpeg_reopen_compare PUBLIC FFmpeg::swscale FFmpeg::avutil)
    bloom_enable_warnings(bloom_media_ffmpeg_reopen_compare)
    add_library(bloom_media_ffmpeg_worker_provider STATIC ffmpeg_provider.cpp)
    target_link_libraries(bloom_media_ffmpeg_worker_provider PRIVATE bloom_media_provider bloom_platform_process
                          bloom_media_ffmpeg_reopen_compare
                          FFmpeg::avformat FFmpeg::avcodec FFmpeg::avfilter FFmpeg::swscale FFmpeg::swresample
                          ${bloom_ffmpeg_openh264_link})
    bloom_enable_warnings(bloom_media_ffmpeg_worker_provider)
    add_executable(bloom-media-worker main.cpp)
    target_link_libraries(bloom-media-worker PRIVATE bloom_media_provider bloom_media_ffmpeg_worker_provider
                          bloom_media_openh264_runtime bloom_platform_process)
    bloom_enable_warnings(bloom-media-worker)
    target_compile_definitions(bloom_media_worker_host
                               PRIVATE BLOOM_FFMPEG_WORKER_BUILD="$<TARGET_FILE:bloom-media-worker>")
    set(bloom_ffmpeg_install_dir "${bloom_media_install_root}/ffmpeg-v1")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_dependencies(bloom-media-worker bloom_openh264_shim)
        set_target_properties(bloom-media-worker PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE BUILD_WITH_INSTALL_RPATH TRUE
                              INSTALL_RPATH "$ORIGIN/lib" INSTALL_RPATH_USE_LINK_PATH FALSE)
        # Copy the shared intake closure beside the build and installed worker. Do not expose a
        # codec library search path to the desktop or rely on the caller's LD_LIBRARY_PATH.
        file(GLOB bloom_ffmpeg_shared "${BLOOM_DEPENDENCY_PREFIX}/lib/libav*.so*"
             "${BLOOM_DEPENDENCY_PREFIX}/lib/libswresample.so*" "${BLOOM_DEPENDENCY_PREFIX}/lib/libswscale.so*")
        file(COPY ${bloom_ffmpeg_shared} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/lib")
        find_program(bloom_patchelf patchelf REQUIRED)
        file(GLOB bloom_ffmpeg_private "${CMAKE_CURRENT_BINARY_DIR}/lib/libav*.so*"
             "${CMAKE_CURRENT_BINARY_DIR}/lib/libswresample.so*" "${CMAKE_CURRENT_BINARY_DIR}/lib/libswscale.so*")
        foreach(bloom_ffmpeg_library IN LISTS bloom_ffmpeg_private)
            if(NOT IS_SYMLINK "${bloom_ffmpeg_library}")
                execute_process(COMMAND "${bloom_patchelf}" --set-rpath "$ORIGIN" "${bloom_ffmpeg_library}"
                                COMMAND_ERROR_IS_FATAL ANY)
            endif()
        endforeach()
        install(TARGETS bloom_openh264_shim
                LIBRARY DESTINATION "${bloom_ffmpeg_install_dir}/lib"
                RUNTIME DESTINATION "${bloom_ffmpeg_install_dir}/lib")
        install(FILES ${bloom_ffmpeg_private} DESTINATION "${bloom_ffmpeg_install_dir}/lib")
    elseif(APPLE)
        # The dylibs live in the worker's private lib/ inside the bundle and are found through the
        # worker's @loader_path/lib rpath. The superbuild installs them with @rpath install names
        # (--install-name-dir=@rpath), so they are copied unmodified and keep their signatures.
        set_target_properties(bloom-media-worker PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
                              INSTALL_RPATH "@loader_path/lib" INSTALL_RPATH_USE_LINK_PATH FALSE)
        file(GLOB bloom_ffmpeg_shared "${BLOOM_DEPENDENCY_PREFIX}/lib/libav*.dylib"
             "${BLOOM_DEPENDENCY_PREFIX}/lib/libswresample*.dylib" "${BLOOM_DEPENDENCY_PREFIX}/lib/libswscale*.dylib")
        if(BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
            foreach(bloom_ffmpeg_library IN LISTS bloom_ffmpeg_shared)
                if(NOT IS_SYMLINK "${bloom_ffmpeg_library}")
                    execute_process(COMMAND otool -D "${bloom_ffmpeg_library}"
                                    OUTPUT_VARIABLE bloom_ffmpeg_install_name COMMAND_ERROR_IS_FATAL ANY)
                    if(NOT bloom_ffmpeg_install_name MATCHES "\n@rpath/")
                        message(FATAL_ERROR "qualified mode: ${bloom_ffmpeg_library} has no @rpath "
                                            "install name; rebuild the FFmpeg prefix")
                    endif()
                endif()
            endforeach()
        endif()
        file(COPY ${bloom_ffmpeg_shared} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/lib")
        file(GLOB bloom_ffmpeg_private "${CMAKE_CURRENT_BINARY_DIR}/lib/lib*.dylib")
        install(FILES ${bloom_ffmpeg_private} DESTINATION "${bloom_ffmpeg_install_dir}/lib")
    elseif(WIN32)
        # Windows resolves DLLs from the executable's own directory first, so the FFmpeg DLLs sit
        # beside the worker in its private generation directory and never beside Bloom.exe.
        file(GLOB bloom_ffmpeg_private "${BLOOM_DEPENDENCY_PREFIX}/bin/av*.dll"
             "${BLOOM_DEPENDENCY_PREFIX}/bin/swresample*.dll" "${BLOOM_DEPENDENCY_PREFIX}/bin/swscale*.dll")
        add_custom_command(TARGET bloom-media-worker POST_BUILD
                           COMMAND ${CMAKE_COMMAND} -E copy_if_different ${bloom_ffmpeg_private}
                                   "$<TARGET_FILE_DIR:bloom-media-worker>"
                           VERBATIM)
        install(FILES ${bloom_ffmpeg_private} DESTINATION "${bloom_ffmpeg_install_dir}")
    endif()
    install(TARGETS bloom-media-worker RUNTIME DESTINATION "${bloom_ffmpeg_install_dir}")
else()
    # Developer build without FFmpeg: keep the target name other directories order their tests
    # after, with nothing to build.
    add_custom_target(bloom-media-worker)
endif()

if(APPLE)
    # The platform provider generation: the Apple-framework VideoToolbox/AVFoundation provider, first
    # in the macOS chain ahead of the bundled FFmpeg worker. It links system frameworks only and
    # installs into its own generation directory inside the bundle.
    target_sources(bloom_media_provider
                   PRIVATE "${PROJECT_SOURCE_DIR}/src/media/provider/videotoolbox_manifest.cpp")
    target_compile_definitions(bloom_media_provider PRIVATE
        BLOOM_MEDIA_DEPENDENCY_LOCK="${bloom_media_lock_digest}"
        BLOOM_VIDEOTOOLBOX_BUILD_ID="videotoolbox-v1:${bloom_media_build_digest}"
        BLOOM_VIDEOTOOLBOX_FIXTURE_ID="${bloom_media_build_digest}"
        BLOOM_VIDEOTOOLBOX_ENCODE_FIXTURE_ID="${bloom_media_build_digest}"
        BLOOM_MEDIA_OS="${CMAKE_SYSTEM_NAME}" BLOOM_MEDIA_ARCH="${CMAKE_SYSTEM_PROCESSOR}")
    add_library(bloom_media_videotoolbox_worker_provider STATIC videotoolbox_provider.mm)
    target_include_directories(bloom_media_videotoolbox_worker_provider
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(bloom_media_videotoolbox_worker_provider
        PRIVATE bloom_media_provider
                "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
                "-framework VideoToolbox" "-framework CoreFoundation" "-framework Foundation")
    bloom_enable_warnings(bloom_media_videotoolbox_worker_provider)
    target_compile_options(bloom_media_videotoolbox_worker_provider
                           PRIVATE -Wno-deprecated-declarations -Wno-old-style-cast)
    add_executable(bloom-media-videotoolbox-worker main.cpp)
    target_compile_definitions(bloom-media-videotoolbox-worker PRIVATE BLOOM_VIDEOTOOLBOX_WORKER)
    target_link_libraries(bloom-media-videotoolbox-worker
        PRIVATE bloom_media_provider bloom_media_videotoolbox_worker_provider bloom_platform_process)
    bloom_enable_warnings(bloom-media-videotoolbox-worker)
    target_compile_definitions(bloom_media_worker_host PRIVATE
        BLOOM_NATIVE_WORKER_BUILD="$<TARGET_FILE:bloom-media-videotoolbox-worker>")
    install(TARGETS bloom-media-videotoolbox-worker
            RUNTIME DESTINATION "${bloom_media_install_root}/videotoolbox-v1")

    if(BUILD_TESTING)
        include(BloomTesting)
        add_executable(bloom_media_videotoolbox_test tests/videotoolbox_provider_tests.mm)
        target_include_directories(bloom_media_videotoolbox_test
                                   PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(bloom_media_videotoolbox_test
            PRIVATE bloom_media_provider bloom_media_videotoolbox_worker_provider
                    "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
                    "-framework VideoToolbox" "-framework CoreFoundation" "-framework Foundation")
        bloom_enable_warnings(bloom_media_videotoolbox_test)
        target_compile_options(bloom_media_videotoolbox_test
                               PRIVATE -Wno-deprecated-declarations -Wno-old-style-cast)
        bloom_add_test(NAME bloom.media.videotoolbox COMMAND bloom_media_videotoolbox_test
                       LABELS unit media)

        # End-to-end export through the real worker process (host session -> worker -> encode ->
        # reopen QC -> artifact). Single low-resolution frame, so a broken colour path, tolerance,
        # or protocol step fails here instead of in the running application.
        add_executable(bloom_media_videotoolbox_export_test
                       tests/videotoolbox_export_tests.cpp)
        target_link_libraries(bloom_media_videotoolbox_export_test PRIVATE bloom_media_worker_host)
        bloom_enable_warnings(bloom_media_videotoolbox_export_test)
        bloom_add_test(NAME bloom.media.videotoolbox-export
                       COMMAND bloom_media_videotoolbox_export_test LABELS unit media)

        # End-to-end decode through the real worker process (generated fixture -> host session ->
        # worker -> probe/decode). Nothing binary is checked in.
        add_executable(bloom_media_videotoolbox_decode_test
                       tests/videotoolbox_decode_tests.mm)
        target_include_directories(bloom_media_videotoolbox_decode_test
                                   PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(bloom_media_videotoolbox_decode_test
            PRIVATE bloom_media_video
                    "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
                    "-framework VideoToolbox" "-framework CoreFoundation" "-framework Foundation")
        bloom_enable_warnings(bloom_media_videotoolbox_decode_test)
        target_compile_options(bloom_media_videotoolbox_decode_test
                               PRIVATE -Wno-deprecated-declarations -Wno-old-style-cast)
        bloom_add_test(NAME bloom.media.videotoolbox-decode
                       COMMAND bloom_media_videotoolbox_decode_test LABELS unit media)
    endif()
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Tests may exercise the newly-intaken FFmpeg wrapper only with the exact Cisco binary. This
    # staging directory is build-only test data; it is never copied into the dependency prefix,
    # installed, or used as a product runtime location.
    set(bloom_openh264_test_archive
        "${BLOOM_DEPENDENCY_PREFIX}/../superbuild/downloads/libopenh264-2.6.0-linux64.8.so.bz2")
    set(bloom_openh264_test_license
        "${BLOOM_DEPENDENCY_PREFIX}/../superbuild/downloads/BINARY_LICENSE.txt")
    set(bloom_openh264_test_runtime "${CMAKE_CURRENT_BINARY_DIR}/openh264-test")
    set(bloom_openh264_test_library "${bloom_openh264_test_runtime}/libopenh264.so.8")
    if(EXISTS "${bloom_openh264_test_archive}" AND EXISTS "${bloom_openh264_test_license}")
        add_custom_command(
            OUTPUT "${bloom_openh264_test_library}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${bloom_openh264_test_runtime}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_openh264_test_archive}"
                    "${bloom_openh264_test_runtime}/libopenh264-2.6.0-linux64.8.so.bz2"
            COMMAND /usr/bin/bunzip2 --force
                    "${bloom_openh264_test_runtime}/libopenh264-2.6.0-linux64.8.so.bz2"
            COMMAND ${CMAKE_COMMAND} -E rename
                    "${bloom_openh264_test_runtime}/libopenh264-2.6.0-linux64.8.so"
                    "${bloom_openh264_test_library}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_openh264_test_license}"
                    "${bloom_openh264_test_runtime}/BINARY_LICENSE.txt"
            DEPENDS "${bloom_openh264_test_archive}" "${bloom_openh264_test_license}"
            VERBATIM)
        add_custom_target(bloom_media_openh264_test_runtime
                          DEPENDS "${bloom_openh264_test_library}")
        set(bloom_openh264_test_runtime_available TRUE)
    else()
        set(bloom_openh264_test_runtime_available FALSE)
        message(STATUS "Cisco OpenH264 test binary is absent; FFmpeg encode/decode tests will skip")
    endif()
    add_library(bloom_media_ffmpeg_fixture_worker_provider STATIC ffmpeg_provider.cpp)
    target_compile_definitions(bloom_media_ffmpeg_fixture_worker_provider PRIVATE BLOOM_MEDIA_FIXTURE_GENERATOR)
    target_link_libraries(bloom_media_ffmpeg_fixture_worker_provider PRIVATE bloom_media_provider bloom_platform_process
                          bloom_media_ffmpeg_reopen_compare
                          FFmpeg::avformat FFmpeg::avcodec FFmpeg::avfilter FFmpeg::swscale FFmpeg::swresample
                          "${BLOOM_DEPENDENCY_PREFIX}/lib/libopenh264.so")
    bloom_enable_warnings(bloom_media_ffmpeg_fixture_worker_provider)
    add_executable(bloom_media_generate_fixtures tests/generate_fixtures.cpp)
    target_include_directories(bloom_media_generate_fixtures PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(bloom_media_generate_fixtures PRIVATE bloom_media_ffmpeg_fixture_worker_provider bloom_media_provider)
    add_dependencies(bloom_media_generate_fixtures bloom_openh264_shim)
    set_target_properties(
        bloom_media_generate_fixtures
        PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE
                   BUILD_WITH_INSTALL_RPATH TRUE
                   BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR}/lib"
                   INSTALL_RPATH "$ORIGIN/lib"
                   INSTALL_RPATH_USE_LINK_PATH FALSE)
    if(bloom_openh264_test_runtime_available)
        add_dependencies(bloom_media_generate_fixtures bloom_media_openh264_test_runtime)
    endif()
    bloom_enable_warnings(bloom_media_generate_fixtures)
    add_executable(bloom_media_reopen_compare_test tests/reopen_compare_tests.cpp)
    target_link_libraries(bloom_media_reopen_compare_test PRIVATE bloom_media_ffmpeg_reopen_compare)
    set_target_properties(bloom_media_reopen_compare_test PROPERTIES
                          BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR}/lib")
    bloom_enable_warnings(bloom_media_reopen_compare_test)
    bloom_add_test(NAME bloom.media.reopen-compare COMMAND bloom_media_reopen_compare_test
                   LABELS unit media TIMEOUT 60)
    add_executable(bloom_media_encode_test "${PROJECT_SOURCE_DIR}/src/media/provider/tests/encode_tests.cpp")
    if(bloom_openh264_test_runtime_available)
        target_compile_definitions(bloom_media_encode_test PRIVATE
            BLOOM_OPENH264_TEST_RUNTIME="${bloom_openh264_test_runtime}")
    endif()
    target_link_libraries(bloom_media_encode_test PRIVATE bloom_media_worker_host)
    bloom_enable_warnings(bloom_media_encode_test)
    add_dependencies(bloom_media_encode_test bloom-media-worker)
    if(bloom_openh264_test_runtime_available)
        add_dependencies(bloom_media_encode_test bloom_media_openh264_test_runtime)
    endif()
    bloom_add_test(NAME bloom.media.encode COMMAND bloom_media_encode_test
                   "${CMAKE_CURRENT_BINARY_DIR}/encode-fixtures" LABELS unit media TIMEOUT 120)
    add_executable(bloom_media_ffmpeg_test "${PROJECT_SOURCE_DIR}/src/media/provider/tests/ffmpeg_tests.cpp")
    target_link_libraries(bloom_media_ffmpeg_test PRIVATE bloom_media_worker_host)
    target_compile_definitions(bloom_media_ffmpeg_test PRIVATE BLOOM_FFMPEG_WORKER="$<TARGET_FILE:bloom-media-worker>")
    bloom_enable_warnings(bloom_media_ffmpeg_test)
    add_dependencies(bloom_media_ffmpeg_test bloom-media-worker bloom_media_generate_fixtures)
    include(BloomTesting)
    bloom_add_test(NAME bloom.media.generate-video-fixtures COMMAND bloom_media_generate_fixtures
                   "${CMAKE_CURRENT_BINARY_DIR}/fixtures" LABELS unit media TIMEOUT 30)
    set_tests_properties(bloom.media.generate-video-fixtures PROPERTIES FIXTURES_SETUP media3)
    bloom_add_test(NAME bloom.media.ffmpeg COMMAND bloom_media_ffmpeg_test
                   "${CMAKE_CURRENT_BINARY_DIR}/fixtures" LABELS unit media security TIMEOUT 120)
    set_tests_properties(bloom.media.ffmpeg PROPERTIES FIXTURES_REQUIRED media3)
    add_executable(bloom_media_video_session_test "${PROJECT_SOURCE_DIR}/src/media/video/tests/session_tests.cpp")
    target_link_libraries(bloom_media_video_session_test PRIVATE bloom_media_video bloom_color_video)
    bloom_enable_warnings(bloom_media_video_session_test)
    add_dependencies(bloom_media_video_session_test bloom-media-worker bloom_media_generate_fixtures)
    bloom_add_test(NAME bloom.media.video-session COMMAND bloom_media_video_session_test
                   "${CMAKE_CURRENT_BINARY_DIR}/fixtures" LABELS unit media TIMEOUT 60)
    set_tests_properties(bloom.media.video-session PROPERTIES FIXTURES_REQUIRED media3)
    add_executable(bloom_media_video_source_revision_test "${PROJECT_SOURCE_DIR}/src/media/video/tests/source_revision_tests.cpp")
    target_include_directories(bloom_media_video_source_revision_test PRIVATE "${PROJECT_SOURCE_DIR}/src/render")
    target_link_libraries(bloom_media_video_source_revision_test PRIVATE bloom_media_video)
    bloom_enable_warnings(bloom_media_video_source_revision_test)
    add_dependencies(bloom_media_video_source_revision_test bloom-media-worker bloom_media_generate_fixtures)
    bloom_add_test(NAME bloom.media.video-source-revision COMMAND bloom_media_video_source_revision_test
                   "${CMAKE_CURRENT_BINARY_DIR}/fixtures" LABELS unit media security TIMEOUT 60)
    set_tests_properties(bloom.media.video-source-revision PROPERTIES FIXTURES_REQUIRED media3)
    add_executable(bloom_media_video_integration_test "tests/video_integration_tests.cpp")
    target_link_libraries(bloom_media_video_integration_test PRIVATE bloom_commands bloom_runtime_evaluator bloom_project bloom_media_video)
    target_include_directories(bloom_media_video_integration_test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    bloom_enable_warnings(bloom_media_video_integration_test)
    add_dependencies(bloom_media_video_integration_test bloom-media-worker bloom_media_generate_fixtures)
    bloom_add_test(NAME bloom.media.video-integration COMMAND bloom_media_video_integration_test
                   "${CMAKE_CURRENT_BINARY_DIR}/fixtures" LABELS unit media integration TIMEOUT 60)
    set_tests_properties(bloom.media.video-integration PROPERTIES FIXTURES_REQUIRED media3)
    # Local latency probe for generated media; intentionally not a CTest case.
    add_executable(bloom_media_video_decode_benchmark tests/video_decode_benchmark.cpp)
    target_link_libraries(bloom_media_video_decode_benchmark PRIVATE bloom_media_video)
    bloom_enable_warnings(bloom_media_video_decode_benchmark)
    add_dependencies(bloom_media_video_decode_benchmark bloom-media-worker)
    add_executable(bloom_media_hostile_worker tests/hostile_worker.cpp)
    target_include_directories(bloom_media_hostile_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(bloom_media_hostile_worker PRIVATE bloom_media_provider bloom_media_fake_worker_provider bloom_platform_process)
    bloom_enable_warnings(bloom_media_hostile_worker)
    add_executable(bloom_media_worker_test "${PROJECT_SOURCE_DIR}/src/media/provider/tests/worker_tests.cpp")
    target_link_libraries(bloom_media_worker_test PRIVATE bloom_media_worker_host)
    target_compile_definitions(bloom_media_worker_test PRIVATE
        BLOOM_MEDIA_WORKER="$<TARGET_FILE:bloom-media-fake-worker>"
        BLOOM_MEDIA_HOSTILE_WORKER="$<TARGET_FILE:bloom_media_hostile_worker>"
        BLOOM_MEDIA_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/media/worker-cases.txt"
        BLOOM_MEDIA_DEPENDENCY_LOCK="${bloom_media_lock_digest}"
        BLOOM_MEDIA_BUILD_ID="${bloom_media_build_id}"
        BLOOM_MEDIA_OS="${CMAKE_SYSTEM_NAME}" BLOOM_MEDIA_ARCH="${CMAKE_SYSTEM_PROCESSOR}")
    add_dependencies(bloom_media_worker_test bloom-media-fake-worker bloom_media_hostile_worker)
    bloom_enable_warnings(bloom_media_worker_test)
    include(BloomTesting)
    bloom_add_test(NAME bloom.media.worker COMMAND bloom_media_worker_test LABELS unit media security TIMEOUT 30)
endif()

# Evaluate after the optional desktop composition root exists. Inspect transitive target links,
# including LINK_ONLY expressions, so a future intermediate library cannot smuggle a provider
# into the desktop. Provider-private targets use the bloom_media_*_worker_provider naming rule.
function(bloom_media_check_target target visited)
    if(NOT TARGET "${target}" OR "${target}" IN_LIST visited)
        return()
    endif()
    if(target MATCHES "^bloom_media_.*_worker_provider$")
        message(FATAL_ERROR "Desktop links worker-private provider target: ${target}")
    endif()
    list(APPEND visited "${target}")
    get_target_property(bloom_media_direct "${target}" LINK_LIBRARIES)
    get_target_property(bloom_media_interface "${target}" INTERFACE_LINK_LIBRARIES)
    string(REGEX MATCHALL "bloom_[A-Za-z0-9_]+" bloom_media_links
           "${bloom_media_direct};${bloom_media_interface}")
    foreach(bloom_media_link IN LISTS bloom_media_links)
        bloom_media_check_target("${bloom_media_link}" "${visited}")
    endforeach()
endfunction()
function(bloom_media_check_desktop)
    bloom_media_check_target(bloom "")
    bloom_media_check_target(bloom_media_provider "")
    bloom_media_check_target(bloom_media_worker_host "")
endfunction()
cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL bloom_media_check_desktop)
