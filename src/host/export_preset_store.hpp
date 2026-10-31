#pragma once

#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Private to src/host: the per-user preset store (ADR 0033, "Preferences") and the encoder probe.
// Every function here performs I/O and runs only on a blocking-I/O worker.
namespace bloom::host {

inline constexpr std::string_view kExportPresetStoreFileName = "export-presets.json";
inline constexpr std::string_view kExportPresetStoreFormat = "bloom.export-presets";
inline constexpr std::uint32_t kExportPresetStoreVersion = 1;

struct ExportUserPresetRecord final {
    std::string id;
    std::string displayName;
    document::ExportOutputSettings settings;
};

// A missing file is an empty store. A corrupt, foreign or newer-versioned file is left untouched
// and reported as nullopt (the catalogue then refuses writes with StoreUnavailable).
[[nodiscard]] std::optional<std::vector<ExportUserPresetRecord>>
readExportUserPresets(const std::filesystem::path& path);
// Atomic replace through a sibling temporary file.
[[nodiscard]] bool writeExportUserPresets(const std::filesystem::path& path,
                                          const std::vector<ExportUserPresetRecord>& records);

// Serialization shared with exportJobScript(): the 1.29 `settings` object as compact JSON.
[[nodiscard]] std::string exportSettingsJson(const document::ExportOutputSettings& settings);

// Probes this computer's encoders (VA-API render nodes and their encode entry
// points through the media worker, the worker itself). Worker thread only.
[[nodiscard]] ExportEncoderStatus probeExportEncoders(const ExportEncoderStatus& initial);

} // namespace bloom::host
