# The official sdist contains the separately locked robin-map 1.4.0 subtree.
set(BLOOM_NANOBIND_VERSION 3.0.1)
set(BLOOM_NANOBIND_URL
    "https://files.pythonhosted.org/packages/07/11/69cf231c4f3ac4fa724a0b1933c3650fb65aff6b9d7a4c2573b9e4e40077/nanobind-3.0.1.tar.gz")
set(BLOOM_NANOBIND_SHA256 f7f0a889c8fb80deaacb95e918d88e05148850a32d2b8eda28446291b2bf7c35)
ExternalProject_Add(bloom_dependency_nanobind
    URL "${BLOOM_NANOBIND_URL}"
    URL_HASH SHA256=${BLOOM_NANOBIND_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME nanobind-${BLOOM_NANOBIND_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CONFIGURE_COMMAND ${CMAKE_COMMAND}
        -S "${CMAKE_CURRENT_LIST_DIR}/nanobind" -B <BINARY_DIR>
        -G "${CMAKE_GENERATOR}"
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
        -DBLOOM_NANOBIND_SOURCE:PATH=<SOURCE_DIR>
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target bloom-nanobind-static --parallel 3
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>)
ExternalProject_Add_Step(bloom_dependency_nanobind bloom_install_records
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/../../licenses/nanobind"
        "${BLOOM_DEPENDENCY_PREFIX}/share/licenses/nanobind"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/../../licenses/robin_map"
        "${BLOOM_DEPENDENCY_PREFIX}/share/licenses/robin_map"
    DEPENDEES install)
