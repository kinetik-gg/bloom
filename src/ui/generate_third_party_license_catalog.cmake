# Generates bloom::ui::thirdPartyLicenseCatalog() (task U2, issue #118, decision 3) from the real,
# checked-in license records -- never hand-maintained -- so the "Open Source Licenses..." window
# always lists exactly what this build actually ships:
#
#   * every dependencies/licenses/<component>/LICENSE  (the qualified-prefix superbuild libraries)
#   * every src/ui/kit/third_party/<component>/LICENSE (the vendored icon/font assets)
#   * a Qt/ADR-0014 dynamic-LGPL distribution notice, authored here (there is no vendored Qt
#     LICENSE file in this repository to parse: Bloom links Qt dynamically rather than vendoring
#     its source -- see docs/decisions/0014-apache-license-and-qt-distribution.md)
#
# Technique: mirrors src/color/ocio_builtin_payload.inc.in's file(READ ... HEX) -> "0x..," byte
# array embed, generalized from one frozen asset to N discovered files, because license text has
# no fixed byte count to assert against. Runs at CMake configure time; included (not
# add_subdirectory'd) from src/ui/CMakeLists.txt so relative paths resolve against that file's own
# CMAKE_CURRENT_SOURCE_DIR/BINARY_DIR. Every source LICENSE file is registered with
# CMAKE_CONFIGURE_DEPENDS so editing one alone triggers a reconfigure and regeneration.

set(_bloom_license_component_names "")
set(_bloom_license_component_files "")

file(GLOB _bloom_dependency_license_dirs LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/../../dependencies/licenses/*")
file(GLOB _bloom_kit_license_dirs LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/kit/third_party/*")

foreach(_bloom_license_dir IN LISTS _bloom_dependency_license_dirs _bloom_kit_license_dirs)
    if(IS_DIRECTORY "${_bloom_license_dir}" AND EXISTS "${_bloom_license_dir}/LICENSE")
        get_filename_component(_bloom_component_name "${_bloom_license_dir}" NAME)
        list(APPEND _bloom_license_component_names "${_bloom_component_name}")
        list(APPEND _bloom_license_component_files "${_bloom_license_dir}/LICENSE")
    endif()
endforeach()

# The Qt notice: authored text, not parsed from a vendored file (see the file comment above),
# routed through the same hex-embed path as every other entry by writing it to a generated text
# file first.
set(_bloom_qt_notice_text [[Qt 6

Bloom links dynamically against the Qt 6 libraries under the GNU Lesser General Public License,
version 3 (LGPLv3). Bloom does not modify Qt's source. Community release packages bundle the
qualified, unmodified Qt runtime libraries and plugins alongside Bloom; artists never install a
separate Qt SDK or compile Bloom themselves.

Under the LGPLv3, you may relink Bloom against a modified or different compatible version of the
Qt libraries. Bloom preserves this right by linking dynamically and not imposing technical
restrictions (such as static linking, or locked or signed libraries that would reject a relinked
Qt) that would remove it.

Qt is Copyright (C) The Qt Company Ltd. and other contributors, and is licensed under the GNU
Lesser General Public License, version 3, as published by the Free Software Foundation. The
complete LGPLv3 text is available at https://www.gnu.org/licenses/lgpl-3.0.html. Qt's own
licensing terms are documented at https://doc.qt.io/qt-6/licensing.html.

See this repository's docs/decisions/0014-apache-license-and-qt-distribution.md for Bloom's
complete Qt distribution decision.
]])
set(_bloom_qt_notice_path "${CMAKE_CURRENT_BINARY_DIR}/generated/qt6-notice.txt")
file(WRITE "${_bloom_qt_notice_path}" "${_bloom_qt_notice_text}")
list(APPEND _bloom_license_component_names "Qt 6")
list(APPEND _bloom_license_component_files "${_bloom_qt_notice_path}")

list(LENGTH _bloom_license_component_names _bloom_license_component_count)
if(_bloom_license_component_count EQUAL 0)
    message(FATAL_ERROR
        "No third-party license files discovered under dependencies/licenses/*/LICENSE or "
        "src/ui/kit/third_party/*/LICENSE -- the Open Source Licenses window would ship empty. "
        "This means the license roots moved or are missing; fix the roots or this script.")
endif()

set(_bloom_catalog_body "")
string(APPEND _bloom_catalog_body
    "// GENERATED FILE -- produced by src/ui/generate_third_party_license_catalog.cmake at CMake\n"
    "// configure time. Do not edit by hand; edit the source LICENSE files or this script instead.\n"
    "#include <bloom/ui/third_party_license_catalog.hpp>\n"
    "\n"
    "namespace bloom::ui {\n"
    "namespace {\n"
    "\n")

set(_bloom_catalog_entries "")
math(EXPR _bloom_license_last_index "${_bloom_license_component_count} - 1")
foreach(_bloom_i RANGE 0 ${_bloom_license_last_index})
    list(GET _bloom_license_component_names ${_bloom_i} _bloom_component_name)
    list(GET _bloom_license_component_files ${_bloom_i} _bloom_component_file)

    file(READ "${_bloom_component_file}" _bloom_hex HEX)
    string(LENGTH "${_bloom_hex}" _bloom_hex_length)
    math(EXPR _bloom_byte_count "${_bloom_hex_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bloom_bytes "${_bloom_hex}")

    string(REPLACE "\\" "\\\\" _bloom_name_escaped "${_bloom_component_name}")
    string(REPLACE "\"" "\\\"" _bloom_name_escaped "${_bloom_name_escaped}")

    string(APPEND _bloom_catalog_body
        "constexpr unsigned char kLicenseBytes${_bloom_i}[] = { ${_bloom_bytes} };\n")
    string(APPEND _bloom_catalog_entries
        "    { std::string_view(\"${_bloom_name_escaped}\"), "
        "std::string_view(reinterpret_cast<const char*>(kLicenseBytes${_bloom_i}), "
        "${_bloom_byte_count}) },\n")
endforeach()

string(APPEND _bloom_catalog_body
    "\n"
    "const ThirdPartyLicenseEntry kEntries[] = {\n"
    "${_bloom_catalog_entries}"
    "};\n"
    "\n"
    "} // namespace\n"
    "\n"
    "std::span<const ThirdPartyLicenseEntry> thirdPartyLicenseCatalog() {\n"
    "    return kEntries;\n"
    "}\n"
    "\n"
    "} // namespace bloom::ui\n")

set(BLOOM_THIRD_PARTY_LICENSE_CATALOG_CPP
    "${CMAKE_CURRENT_BINARY_DIR}/generated/third_party_license_catalog.generated.cpp")
file(WRITE "${BLOOM_THIRD_PARTY_LICENSE_CATALOG_CPP}" "${_bloom_catalog_body}")

# Reconfigure (and so regenerate) whenever a source LICENSE file -- or this script -- changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_bloom_license_component_files}
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_third_party_license_catalog.cmake")
