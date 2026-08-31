# yaml-cpp 0.9.0 — the YAML parser/emitter OpenColorIO's core library requires unconditionally to
# read and write .ocio config files (src/OpenColorIO/OCIOYaml.cpp); see
# dependencies/licenses/yaml-cpp/ and OpenColorIO's FindExtPackages.cmake, whose "Required
# dependencies" section lists yaml-cpp as REQUIRED ALLOW_INSTALL with no build-time opt-out. MIT
# license (see dependencies/licenses/yaml-cpp/). Static, library-only build: the yaml-parse/
# yaml-merge command-line tools, the legacy GraphBuilder "contrib" sources (unused by OCIO — see
# dependencies/licenses/yaml-cpp/review.md), and the test suite all stay out of the prefix.
#
# Installs its CMake package config (yaml-cpp-config.cmake, target yaml-cpp::yaml-cpp on this
# version) into the shared prefix so bloom_dependency_opencolorio's find_package(yaml-cpp CONFIG)
# resolves it without touching the host.
set(BLOOM_YAML_CPP_VERSION 0.9.0)
set(BLOOM_YAML_CPP_URL
    "https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-${BLOOM_YAML_CPP_VERSION}.tar.gz")
set(BLOOM_YAML_CPP_SHA256 25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a)

ExternalProject_Add(bloom_dependency_yaml-cpp
    URL "${BLOOM_YAML_CPP_URL}"
    URL_HASH SHA256=${BLOOM_YAML_CPP_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME yaml-cpp-${BLOOM_YAML_CPP_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DYAML_BUILD_SHARED_LIBS:BOOL=OFF
        -DYAML_CPP_BUILD_CONTRIB:BOOL=OFF
        -DYAML_CPP_BUILD_TOOLS:BOOL=OFF
        -DBUILD_TESTING:BOOL=OFF
        -DYAML_CPP_BUILD_TESTS:BOOL=OFF
        -DYAML_CPP_INSTALL:BOOL=ON
        -DYAML_CPP_FORMAT_SOURCE:BOOL=OFF
        -DYAML_ENABLE_PIC:BOOL=ON
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
