#include <bloom/platform/font_catalog.hpp>

namespace bloom::platform::detail {

FontCatalogue enumerateSystemFonts(const FontCatalogueProvider::Cancellation&) {
    FontCatalogue result;
    result.status = FontCatalogueStatus::Unavailable;
    return result;
}

} // namespace bloom::platform::detail
