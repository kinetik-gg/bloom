#pragma once

#include <bloom/core/sha256.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace bloom::platform {

enum class FontCatalogueStatus : std::uint8_t { Available, Unavailable };
enum class FontSource : std::uint8_t { Embedded, System };

struct FontFace final {
    std::string family;
    std::string style;
    std::int32_t weight = 400;
    std::int32_t width = 100;
    bool italic = false;
    std::filesystem::path path;
    core::Sha256Digest contentDigest;
    FontSource source = FontSource::System;
    std::uint32_t faceIndex = 0;

    friend bool operator==(const FontFace&, const FontFace&) = default;
};

struct FontCatalogue final {
    FontCatalogueStatus status = FontCatalogueStatus::Unavailable;
    std::vector<FontFace> faces;

    [[nodiscard]] bool available() const noexcept {
        return status == FontCatalogueStatus::Available;
    }
    [[nodiscard]] const FontFace* findByDigest(const core::Sha256Digest& digest) const noexcept;
};

class FontCatalogueProvider final {
  public:
    using Cancellation = std::function<bool()>;

    // Returns the bundled faces without consulting the operating system. UI catalogues use this
    // small immediate snapshot while the complete system enumeration runs on a worker.
    [[nodiscard]] static FontCatalogue embeddedCatalogue();

    // Enumeration is deliberately synchronous at this low-level boundary. Callers use it from a
    // worker (FontCatalogueCache is the host convenience wrapper); no fontconfig or filesystem
    // work is ever needed by the render evaluator.
    [[nodiscard]] FontCatalogue enumerate(const Cancellation& cancelled = {}) const;
};

} // namespace bloom::platform
