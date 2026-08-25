# Bloom dependency superbuild — the dependency-only composition root defined by
# docs/architecture/dependency-intake.md. It acquires locked archives, builds them with
# networking disabled, and installs them into a staging prefix that the normal Bloom build
# consumes. The application build tree never runs ExternalProject or FetchContent.
#
# Intake staging note: this superbuild currently pins each component's URL and SHA-256 directly
# in its recipe. The reviewed production lock at dependencies/dependencies.lock.json becomes the
# single authority for these values when the production lock validator lands; recipes then read
# the lock instead of declaring literals. Until a qualified prefix exists, prefixes produced here
# are development artifacts and must not be treated as qualified.
cmake_minimum_required(VERSION 3.25)

project(BloomDependencySuperbuild LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Build the dependency superbuild out of source.")
endif()

set(BLOOM_DEPENDENCY_PREFIX "${CMAKE_BINARY_DIR}/prefix" CACHE PATH
    "Staging installation prefix that receives every dependency.")
set(BLOOM_DEPENDENCY_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads" CACHE PATH
    "Content-addressed archive cache. Populate it offline to build without network access.")
option(BLOOM_DEPENDENCY_ALLOW_DOWNLOADS
    "Permit the acquire phase to download missing locked archives. OFF builds strictly offline
from BLOOM_DEPENDENCY_DOWNLOAD_DIR."
    ON)

# The build phase is offline by construction: every recipe verifies its archive digest before
# extraction, disables in-recipe network features, and never uses FetchContent. Downloads happen
# only through ExternalProject's download step against the exact locked URL and hash.
include(ExternalProject)

# Reproducibility baseline shared by every recipe. SOURCE_DATE_EPOCH and locale pinning keep
# component builds independent of the invoking user's environment.
set(BLOOM_DEPENDENCY_COMMON_ENV
    "SOURCE_DATE_EPOCH=0"
    "LC_ALL=C"
    "TZ=UTC")

set(BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX:PATH=${BLOOM_DEPENDENCY_PREFIX}
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
    -DBUILD_SHARED_LIBS:BOOL=OFF
    -DCMAKE_POLICY_DEFAULT_CMP0074:STRING=NEW
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG:BOOL=ON
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY:BOOL=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY:BOOL=OFF)

if(NOT BLOOM_DEPENDENCY_ALLOW_DOWNLOADS)
    set(BLOOM_DEPENDENCY_OFFLINE_ARGS DOWNLOAD_NO_PROGRESS ON)
endif()

add_subdirectory(projects)
