add_library(bloom_media_dr_wav OBJECT wav_backend.cpp wav_backend.hpp)
target_compile_features(bloom_media_dr_wav PRIVATE cxx_std_20)
target_link_libraries(bloom_media_dr_wav PRIVATE bloom_core)
target_include_directories(bloom_media_dr_wav PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(bloom_media_dr_wav PRIVATE /w)
else()
    target_compile_options(bloom_media_dr_wav PRIVATE -w)
endif()
bloom_enable_strict_floating_point(bloom_media_dr_wav)

add_library(bloom_media_minimp3 OBJECT mp3_backend.cpp mp3_backend.hpp)
target_compile_features(bloom_media_minimp3 PRIVATE cxx_std_20)
target_link_libraries(bloom_media_minimp3 PRIVATE bloom_core)
target_include_directories(bloom_media_minimp3 PRIVATE include "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(bloom_media_minimp3 PRIVATE /w)
else()
    target_compile_options(bloom_media_minimp3 PRIVATE -w)
endif()
bloom_enable_strict_floating_point(bloom_media_minimp3)

add_library(bloom_media_audio STATIC audio.cpp)
target_sources(
    bloom_media_audio
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/bloom/media/audio/audio.hpp
)
target_compile_features(bloom_media_audio PUBLIC cxx_std_20)
target_include_directories(bloom_media_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    bloom_media_audio
    PUBLIC bloom_core
    PRIVATE bloom_media_dr_wav bloom_media_minimp3
)
bloom_enable_warnings(bloom_media_audio)
bloom_enable_strict_floating_point(bloom_media_audio)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_media_audio_test tests/audio_tests.cpp)
    target_link_libraries(bloom_media_audio_test PRIVATE bloom_media_audio)
    target_include_directories(bloom_media_audio_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(
        bloom_media_audio_test
        PRIVATE BLOOM_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    bloom_enable_warnings(bloom_media_audio_test)
    bloom_enable_strict_floating_point(bloom_media_audio_test)
    bloom_add_test(
        NAME bloom.media.audio
        COMMAND bloom_media_audio_test
        LABELS unit media audio
        TIMEOUT 60
    )
endif()
