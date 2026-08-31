# OpenEXR 3.4.15 — the qualified flat-scanline EXR reader/writer for
# docs/architecture/frame-output.md's FlatExrRgba32fLinRec709SceneV1 preset (BSD-3-Clause; see
# dependencies/licenses/openexr/). Static, core-libraries-only build: OPENEXR_BUILD_TOOLS,
# examples, Python bindings, oss-fuzz harnesses, tests, manpages, and the website all stay off.
#
# Compression minimization: the frame-output contract writes exactly ZIP_COMPRESSION and rejects
# every other compression attribute on reopen verification (see
# docs/architecture/frame-output.md, "Flat OpenEXR Preset Version 1"). OpenEXR 3.4's upstream
# CMakeLists exposes no per-codec switch — RLE/PIZ/PXR24/B44/B44A/DWAA/DWAB/HTJ2K encode and
# decode paths are unconditionally compiled into libOpenEXRCore; there is no
# OPENEXR_ENABLE_<CODEC> option to strip them. That extra codec surface is therefore dead code
# from Bloom's write path but remains reachable if a future consumer decodes an untrusted EXR
# file — noted in security.md as that future consumer's concern, not resolved here.
#
# zlib-vs-libdeflate: OpenEXR removed zlib as a DEFLATE backend option entirely in the 3.2 line;
# 3.4.15's CMake and source tree have no ZLIB reference at all (grep-verified against the
# downloaded archive). ZIP/ZIPS compression now requires libdeflate, either an external package
# or a vendored copy the archive bundles at external/deflate. Per the frozen decision, that makes
# libdeflate a third recipe with full rigor (bloom_dependency_libdeflate) rather than a silent
# vendored/auto-fetched dependency: OPENEXR_FORCE_INTERNAL_DEFLATE stays OFF and CMAKE_PREFIX_PATH
# points find_package(libdeflate CONFIG) at the shared prefix, which ranks ahead of default system
# locations in CMake's config-mode search order. Confirmed in the build log
# ("Using externally provided libdeflate"); the vendored external/deflate/*.c sources are included
# only inside compression.c's `#if OPENEXR_USE_INTERNAL_DEFLATE` branch and are therefore never
# compiled when the external package is found, so no separate vendored-libdeflate component
# exists in the prefix.
#
# OpenJPH (HTJ2K) is a different story: OpenEXRCore/CMakeLists.txt unconditionally
# add_subdirectory()s external/OpenJPH as an EXCLUDE_FROM_ALL OBJECT library and links its object
# files directly into OpenEXRCore whenever no external openjph package is used — there is no build
# switch to omit it. OPENEXR_FORCE_INTERNAL_OPENJPH is forced ON here so that outcome is
# deterministic and hermetic (no ambient find_package/pkg-config probe of the host), rather than
# relying on no host openjph package happening to be absent. This makes OpenJPH 0.31.0
# (BSD-2-Clause; see dependencies/licenses/openjph/) a vendored component of this build: its
# source is embedded in the OpenEXR archive already covered by BLOOM_OPENEXR_SHA256 above, not a
# separate download, and it is compiled as an object library with OJPH_INSTALL=OFF — it is never
# installed or exported as its own package.
#
# DEPENDS on bloom_dependency_imath (find_package(Imath 3.1 CONFIG), same CMAKE_PREFIX_PATH
# mechanism) and bloom_dependency_libdeflate.
set(BLOOM_OPENEXR_VERSION 3.4.15)
set(BLOOM_OPENEXR_URL
    "https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v${BLOOM_OPENEXR_VERSION}.tar.gz")
set(BLOOM_OPENEXR_SHA256 445ed5b0ea4d9cf98be3a4f219e419628b123b61dec65ccb743ab9b07fbebdaa)

ExternalProject_Add(bloom_dependency_openexr
    URL "${BLOOM_OPENEXR_URL}"
    URL_HASH SHA256=${BLOOM_OPENEXR_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME openexr-${BLOOM_OPENEXR_VERSION}.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS bloom_dependency_imath bloom_dependency_libdeflate
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_PREFIX_PATH:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DOPENEXR_BUILD_TOOLS:BOOL=OFF
        -DOPENEXR_INSTALL_TOOLS:BOOL=OFF
        -DOPENEXR_INSTALL_DEVELOPER_TOOLS:BOOL=OFF
        -DOPENEXR_BUILD_EXAMPLES:BOOL=OFF
        -DOPENEXR_BUILD_PYTHON:BOOL=OFF
        -DOPENEXR_BUILD_OSS_FUZZ:BOOL=OFF
        -DBUILD_TESTING:BOOL=OFF
        -DOPENEXR_INSTALL_DOCS:BOOL=OFF
        -DBUILD_WEBSITE:BOOL=OFF
        -DOPENEXR_USE_TBB:BOOL=OFF
        -DOPENEXR_FORCE_INTERNAL_DEFLATE:BOOL=OFF
        -DOPENEXR_FORCE_INTERNAL_OPENJPH:BOOL=ON
        -DOPENEXR_FORCE_INTERNAL_IMATH:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
