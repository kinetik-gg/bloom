# yyjson 0.12.0 — strict JSON reader/writer named as the qualified implementation for
# .bloom project I/O by docs/architecture/project-format.md (MIT; see
# dependencies/licenses/yyjson/). Feature minimization per the intake contract: library only —
# no tests, fuzzers, docs, or miscellaneous tools enter the prefix.
#
# The archive digest below was computed from the exact downloaded release archive on 2026-08-26;
# provenance and review notes live in dependencies/licenses/yyjson/provenance.md.
set(BLOOM_YYJSON_VERSION 0.12.0)
set(BLOOM_YYJSON_URL
    "https://github.com/ibireme/yyjson/archive/refs/tags/${BLOOM_YYJSON_VERSION}.tar.gz")
set(BLOOM_YYJSON_SHA256 b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d)

ExternalProject_Add(bloom_dependency_yyjson
    URL "${BLOOM_YYJSON_URL}"
    URL_HASH SHA256=${BLOOM_YYJSON_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME yyjson-${BLOOM_YYJSON_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DYYJSON_BUILD_TESTS:BOOL=OFF
        -DYYJSON_BUILD_FUZZER:BOOL=OFF
        -DYYJSON_BUILD_MISC:BOOL=OFF
        -DYYJSON_BUILD_DOC:BOOL=OFF
        -DYYJSON_ENABLE_COVERAGE:BOOL=OFF
        -DYYJSON_ENABLE_VALGRIND:BOOL=OFF
        -DYYJSON_ENABLE_SANITIZE:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
