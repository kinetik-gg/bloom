#include <bloom/host/font_catalogue.hpp>

#include <utility>

namespace bloom::host {

FontCatalogueCache::FontCatalogueCache()
    : catalogue_(std::make_shared<const platform::FontCatalogue>(
          platform::FontCatalogueProvider::embeddedCatalogue())) {}

FontCatalogueCache::~FontCatalogueCache() {
    if (future_.valid())
        future_.wait();
}

void FontCatalogueCache::refresh() {
    if (future_.valid() &&
        future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        return;
    future_ = std::async(std::launch::async,
                         [] { return platform::FontCatalogueProvider{}.enumerate(); });
}

bool FontCatalogueCache::poll() {
    if (!future_.valid() ||
        future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        return false;
    catalogue_ = std::make_shared<const platform::FontCatalogue>(future_.get());
    return true;
}

} // namespace bloom::host
