# Imath 3.2.3 — the vector/matrix/half-float math library OpenEXR requires, per
# docs/architecture/dependency-intake.md's Initial Qualification Candidates (BSD-3-Clause; see
# dependencies/licenses/imath/). Static, library-only build: boost/pybind11 Python bindings,
# the ImathTest suite, and the readthedocs website source stay out of the prefix. Installs its
# CMake package config into the shared prefix so bloom_dependency_openexr's
# find_package(Imath 3.1 CONFIG) resolves it without touching the host.
set(BLOOM_IMATH_VERSION 3.2.3)
set(BLOOM_IMATH_URL
    "https://github.com/AcademySoftwareFoundation/Imath/archive/refs/tags/v${BLOOM_IMATH_VERSION}.tar.gz")
set(BLOOM_IMATH_SHA256 e10c12b3f21f45bf08e09d4215d9c7691368d747beebd840de0b6fefed2df9f8)

ExternalProject_Add(bloom_dependency_imath
    URL "${BLOOM_IMATH_URL}"
    URL_HASH SHA256=${BLOOM_IMATH_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME imath-${BLOOM_IMATH_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DPYTHON:BOOL=OFF
        -DPYBIND11:BOOL=OFF
        -DBUILD_TESTING:BOOL=OFF
        -DBUILD_WEBSITE:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
