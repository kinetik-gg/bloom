#pragma once

#include <span>
#include <string_view>

namespace bloom::ui {

// One shipped third-party component's name and complete license text, as embedded at CMake
// configure time by src/ui/generate_third_party_license_catalog.cmake (task U2, issue #118,
// decision 3) from the real records: every `dependencies/licenses/*/LICENSE` and
// `src/ui/kit/third_party/*/LICENSE` file, plus the Qt/ADR-0014 dynamic-LGPL notice. Neither field
// is ever hand-typed here or in the generated source -- both are byte-for-byte copies of the
// license roots' own files (or, for the Qt entry, ADR 0014's own decision text).
struct ThirdPartyLicenseEntry {
    std::string_view component;
    std::string_view licenseText;
};

// The complete catalog, in a stable order (license root traversal order). Defined by the
// generated translation unit (see the CMake script above); never hand-maintained.
[[nodiscard]] std::span<const ThirdPartyLicenseEntry> thirdPartyLicenseCatalog();

} // namespace bloom::ui
