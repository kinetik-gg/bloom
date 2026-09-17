#pragma once

#include <bloom/platform/font_catalog.hpp>

#include <chrono>
#include <future>
#include <memory>

namespace bloom::host {

class FontCatalogueCache final {
  public:
    FontCatalogueCache();
    ~FontCatalogueCache();
    FontCatalogueCache(const FontCatalogueCache&) = delete;
    FontCatalogueCache& operator=(const FontCatalogueCache&) = delete;

    // Starts enumeration on a worker. The previous snapshot remains usable until poll() installs
    // the new one, which keeps a Properties view stable while a large system font directory is
    // read.
    void refresh();
    [[nodiscard]] bool poll();
    [[nodiscard]] std::shared_ptr<const platform::FontCatalogue> snapshot() const noexcept {
        return catalogue_;
    }
    [[nodiscard]] bool pending() const noexcept { return future_.valid(); }

  private:
    std::shared_ptr<const platform::FontCatalogue> catalogue_;
    std::future<platform::FontCatalogue> future_;
};

} // namespace bloom::host
