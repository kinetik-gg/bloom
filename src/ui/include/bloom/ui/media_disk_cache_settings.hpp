#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

class QSettings;
class QWidget;

namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache

// Settings and lifecycle for the media disk cache (docs/architecture/media-io.md "Disk cache").
// These keys are edited by the Settings window (Edit | Settings... -> Memory & Caches, docs/
// user-guide/settings.md), which reads and writes them through bloom::ui::ApplicationPreferences;
// this header remains the reader that owns their parsing and the clear command. They are read once
// at startup, so a change takes effect after restart, exactly like the RAM preview and
// operation-cache byte budgets.
namespace bloom::ui {

// "media/disk-cache-enabled", default true.
[[nodiscard]] bool mediaDiskCacheEnabledFromSettings(const QSettings& settings);

// "media/disk-cache-directory": an absolute path override. Empty, missing, or relative reads as
// "use the platform default" (bloom::platform::userCacheDirectory() plus a "media" leaf).
[[nodiscard]] std::filesystem::path mediaDiskCacheDirectoryFromSettings(const QSettings& settings);

// "media/disk-cache-budget-bytes": missing, zero, unparseable, or negative reads as `fallback`
// (the caller resolves that from physical disk free space via
// media::cache::defaultMediaDiskCacheByteBudget()).
[[nodiscard]] std::uint64_t mediaDiskCacheByteBudgetFromSettings(const QSettings& settings,
                                                                 std::uint64_t fallback);

// Builds a MediaDiskCache from the current settings and the platform cache directory. Returns
// nullptr when the cache is disabled in settings OR no directory can be resolved at all (no
// override and no platform default) -- both are "disk cache unavailable this session", never a
// startup failure (derived caches are optional runtime state per media-io.md).
[[nodiscard]] std::unique_ptr<media::cache::MediaDiskCache>
makeMediaDiskCacheFromSettings(const QSettings& settings);

// The "Clear media cache" command body (Composition menu): asks for confirmation, then clears.
// `cache` may be null (no disk cache configured for this session); the dialog then says so and
// clears nothing. Returns true when the cache was actually cleared, for a status-bar notice.
bool confirmAndClearMediaDiskCache(QWidget* parent, media::cache::MediaDiskCache* cache);

} // namespace bloom::ui
