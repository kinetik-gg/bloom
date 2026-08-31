# expat 2.8.3 — the XML parser OpenColorIO's core library requires unconditionally (CDL/CCC/CTF
# transform-file parsing lives in libOpenColorIO itself, not behind an OCIO_BUILD_* switch); see
# dependencies/licenses/expat/ and OpenColorIO's share/cmake/modules/FindExtPackages.cmake, whose
# "Required dependencies" section lists expat as REQUIRED ALLOW_INSTALL with no build-time opt-out.
# MIT license (see dependencies/licenses/expat/). Static, library-only build: the xmlwf CLI tool,
# examples, tests, docs, fuzzers, and the pkg-config file are all off; the XML feature surface
# itself (DTD/general-entity/namespace parsing) stays at its upstream default because Bloom's own
# OCIO consumption needs correct, standard XML parsing of color-transform files, not a reduced
# parser dialect.
#
# The archive's top-level CMakeLists.txt lives at expat/CMakeLists.txt (the libexpat repository
# also ships an unrelated top-level Brewfile/testdata/expat sibling layout), hence SOURCE_SUBDIR.
#
# Archive symlink (see dependencies/licenses/expat/provenance.md for the full record): one entry,
# libexpat-R_2_8_3/README.md -> expat/README.md, relative, resolves inside the archive to a
# regular file, and lies outside the expat/ subtree this recipe actually configures and builds.
# Skipped per the acquisition rule's provenance-recorded symbolic-link tolerance; never
# materialized.
#
# Installs its CMake package config (expat-config.cmake, target expat::libexpat) into the shared
# prefix so bloom_dependency_opencolorio's find_package(expat CONFIG) resolves it without
# touching the host.
set(BLOOM_EXPAT_VERSION 2.8.3)
set(BLOOM_EXPAT_URL
    "https://github.com/libexpat/libexpat/archive/refs/tags/R_2_8_3.tar.gz")
set(BLOOM_EXPAT_SHA256 533659a16e0184035a99fd8e783f1ad61a887a7bf8586a8681740b9d7ed42389)

ExternalProject_Add(bloom_dependency_expat
    URL "${BLOOM_EXPAT_URL}"
    URL_HASH SHA256=${BLOOM_EXPAT_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME expat-${BLOOM_EXPAT_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR expat
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DEXPAT_SHARED_LIBS:BOOL=OFF
        -DEXPAT_BUILD_TOOLS:BOOL=OFF
        -DEXPAT_BUILD_EXAMPLES:BOOL=OFF
        -DEXPAT_BUILD_TESTS:BOOL=OFF
        -DEXPAT_BUILD_DOCS:BOOL=OFF
        -DEXPAT_BUILD_FUZZERS:BOOL=OFF
        -DEXPAT_BUILD_PKGCONFIG:BOOL=OFF
        -DEXPAT_ENABLE_INSTALL:BOOL=ON
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
