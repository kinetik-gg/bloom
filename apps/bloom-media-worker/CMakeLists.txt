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
     "${PROJECT_SOURCE_DIR}/src/platform/include/bloom/platform/process_*.hpp"
     "${PROJECT_SOURCE_DIR}/src/core/sha256.cpp"
     "${PROJECT_SOURCE_DIR}/src/core/include/bloom/core/sha256.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
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
# FFmpeg remains private to the Linux worker. The host sees only codec-free manifests.
set(bloom_ffmpeg_build_id "ffmpeg-v1:${bloom_media_build_digest}")
target_sources(bloom_media_provider PRIVATE "${PROJECT_SOURCE_DIR}/src/media/provider/ffmpeg_manifest.cpp")
target_compile_definitions(bloom_media_provider PRIVATE
    BLOOM_MEDIA_DEPENDENCY_LOCK="${bloom_media_lock_digest}"
    BLOOM_FFMPEG_BUILD_ID="${bloom_ffmpeg_build_id}"
    BLOOM_FFMPEG_FIXTURE_ID="${bloom_ffmpeg_fixture_digest}"
    BLOOM_MEDIA_OS="${CMAKE_SYSTEM_NAME}" BLOOM_MEDIA_ARCH="${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    bloom_find_dependency(FFmpeg)
    add_library(bloom_media_ffmpeg_worker_provider STATIC ffmpeg_provider.cpp)
    target_link_libraries(bloom_media_ffmpeg_worker_provider PRIVATE bloom_media_provider
                          FFmpeg::avformat FFmpeg::avcodec FFmpeg::swscale FFmpeg::swresample)
    bloom_enable_warnings(bloom_media_ffmpeg_worker_provider)
    add_executable(bloom-media-worker main.cpp)
    target_link_libraries(bloom-media-worker PRIVATE bloom_media_provider bloom_media_ffmpeg_worker_provider bloom_platform_process)
    bloom_enable_warnings(bloom-media-worker)
    target_compile_definitions(bloom_media_video PRIVATE BLOOM_VIDEO_WORKER="$<TARGET_FILE:bloom-media-worker>")
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
    install(TARGETS bloom-media-worker RUNTIME DESTINATION libexec/bloom/media/ffmpeg-v1)
    install(FILES ${bloom_ffmpeg_private} DESTINATION libexec/bloom/media/ffmpeg-v1/lib)
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(bloom_media_ffmpeg_fixture_worker_provider STATIC ffmpeg_provider.cpp)
    target_compile_definitions(bloom_media_ffmpeg_fixture_worker_provider PRIVATE BLOOM_MEDIA_FIXTURE_GENERATOR)
    target_link_libraries(bloom_media_ffmpeg_fixture_worker_provider PRIVATE bloom_media_provider
                          FFmpeg::avformat FFmpeg::avcodec FFmpeg::swscale FFmpeg::swresample)
    bloom_enable_warnings(bloom_media_ffmpeg_fixture_worker_provider)
    add_executable(bloom_media_generate_fixtures tests/generate_fixtures.cpp)
    target_include_directories(bloom_media_generate_fixtures PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(bloom_media_generate_fixtures PRIVATE bloom_media_ffmpeg_fixture_worker_provider bloom_media_provider)
    set_target_properties(bloom_media_generate_fixtures PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/lib")
    bloom_enable_warnings(bloom_media_generate_fixtures)
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
    add_executable(bloom_media_video_integration_test "tests/video_integration_tests.cpp")
    target_link_libraries(bloom_media_video_integration_test PRIVATE bloom_commands bloom_runtime_evaluator bloom_project bloom_media_video)
    bloom_enable_warnings(bloom_media_video_integration_test)
    add_dependencies(bloom_media_video_integration_test bloom-media-worker bloom_media_generate_fixtures)
    bloom_add_test(NAME bloom.media.video-integration COMMAND bloom_media_video_integration_test
                   "${CMAKE_CURRENT_BINARY_DIR}/fixtures" LABELS unit media integration TIMEOUT 60)
    set_tests_properties(bloom.media.video-integration PROPERTIES FIXTURES_REQUIRED media3)
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
