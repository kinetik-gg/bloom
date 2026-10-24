#pragma once

// A thumbnail for a data-only image (no colour layer): the Cryptomatte ID colours of its first
// Cryptomatte type, or its first data channel as grey. Worker-only; decodes two channels at most.

#include <QImage>
#include <bloom/core/sha256.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace bloom::ui::detail {

[[nodiscard]] QImage dataChannelThumbnail(const std::filesystem::path& path,
                                          const core::Sha256Digest& digest, const std::string& part,
                                          const std::function<bool()>& cancel);

} // namespace bloom::ui::detail
