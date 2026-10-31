# Shared build plumbing for the permissively licensed codec libraries that FFmpeg's LGPL wrappers
# link (OpenH264 first). Each codec is built from its pinned source archive as a static,
# position-independent library and linked into the worker's shared libavcodec, so the worker ships
# no extra runtime files and nothing is fetched or installed by the user. The per-codec configure
# vectors live in adjacent `<name>.configure-arguments` files that the dependency lock copies and
# the production lock check compares byte for byte.

# Meson builds the Meson-based codecs (OpenH264) on Linux, macOS and Windows (MSVC from a developer shell).
find_program(BLOOM_MESON_EXECUTABLE meson REQUIRED)
find_program(BLOOM_NINJA_EXECUTABLE ninja REQUIRED)

# x86 assembly in the Meson-built codecs uses the superbuild-private NASM (nasm.cmake). Meson finds it
# through a generated native file, never through the build host's PATH.
set(BLOOM_CODEC_MESON_ARGUMENTS)
set(BLOOM_CODEC_DEPENDS)
if(BLOOM_NASM_REQUIRED)
    set(BLOOM_CODEC_MESON_NATIVE_FILE "${CMAKE_BINARY_DIR}/bloom-codec-meson-native.ini")
    file(WRITE "${BLOOM_CODEC_MESON_NATIVE_FILE}"
        "[binaries]\nnasm = '${BLOOM_NASM_EXECUTABLE}'\nninja = '${BLOOM_NINJA_EXECUTABLE}'\n")
    list(APPEND BLOOM_CODEC_MESON_ARGUMENTS "--native-file=${BLOOM_CODEC_MESON_NATIVE_FILE}")
    list(APPEND BLOOM_CODEC_DEPENDS bloom_dependency_nasm)
endif()

# FFmpeg's MSVC toolchain compiles with cl's default static runtime; the static codec archives
# use the same runtime so libavcodec links one C runtime.
if(MSVC)
    list(APPEND BLOOM_CODEC_MESON_ARGUMENTS -Db_vscrt=mt)
    set(BLOOM_CODEC_CMAKE_PLATFORM_ARGUMENTS
        -DCMAKE_MSVC_RUNTIME_LIBRARY:STRING=MultiThreaded)
else()
    set(BLOOM_CODEC_CMAKE_PLATFORM_ARGUMENTS)
endif()

# The C++ runtime a static C++ codec archive needs when FFmpeg's C link resolves it.
if(APPLE)
    set(BLOOM_CODEC_CXX_RUNTIME -lc++)
elseif(WIN32)
    set(BLOOM_CODEC_CXX_RUNTIME)
else()
    set(BLOOM_CODEC_CXX_RUNTIME -lstdc++)
endif()

set(BLOOM_CODEC_STATIC_INSTALL_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/codec-static-install.cmake")
