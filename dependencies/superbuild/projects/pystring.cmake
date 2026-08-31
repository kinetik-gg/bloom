# pystring 1.2.0 — the Python-string-semantics C++ helper library OpenColorIO's core library
# requires unconditionally for path/string manipulation throughout config parsing and transform
# lookup; see dependencies/licenses/pystring/ and OpenColorIO's FindExtPackages.cmake, whose
# "Required dependencies" section lists pystring as REQUIRED ALLOW_INSTALL with no build-time
# opt-out. BSD-3-Clause license (see dependencies/licenses/pystring/). Static build: only the
# compiled `pystring` target is consumed (OCIO's own Findpystring.cmake locates it by
# find_path/find_library, not a CMake package config — see review.md); the header-only
# `pystring_header_only` interface target is not installed by this recipe's option set beyond
# what upstream always installs. pystring's CMakeLists.txt has no test/example toggle (its
# pystring_test* executables build unconditionally); they are not installed and enter no shipped
# artifact, so this recipe cannot reduce that further without patching upstream, which is out of
# this task's scope (see review.md).
set(BLOOM_PYSTRING_VERSION 1.2.0)
set(BLOOM_PYSTRING_URL
    "https://github.com/imageworks/pystring/archive/refs/tags/v${BLOOM_PYSTRING_VERSION}.tar.gz")
set(BLOOM_PYSTRING_SHA256 020a603a757ba1e429f4b1ea6feb3afbe0fb34bcafa355032e1f1b8a0019d198)

ExternalProject_Add(bloom_dependency_pystring
    URL "${BLOOM_PYSTRING_URL}"
    URL_HASH SHA256=${BLOOM_PYSTRING_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME pystring-${BLOOM_PYSTRING_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DBUILD_SHARED_LIBS:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
