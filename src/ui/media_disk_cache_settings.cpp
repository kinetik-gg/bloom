#include <bloom/ui/media_disk_cache_settings.hpp>

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/platform/cache_directory.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <QSettings>
#include <QString>

namespace bloom::ui {

namespace {
constexpr auto kEnabledKey = "media/disk-cache-enabled";
constexpr auto kDirectoryKey = "media/disk-cache-directory";
constexpr auto kBudgetBytesKey = "media/disk-cache-budget-bytes";

[[nodiscard]] std::filesystem::path nativePathFromSettingsString(const QString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
}
} // namespace

bool mediaDiskCacheEnabledFromSettings(const QSettings& settings) {
    return settings.value(QLatin1String(kEnabledKey), true).toBool();
}

std::filesystem::path mediaDiskCacheDirectoryFromSettings(const QSettings& settings) {
    const auto text = settings.value(QLatin1String(kDirectoryKey)).toString();
    if (text.isEmpty())
        return {};
    const auto path = nativePathFromSettingsString(text);
    return path.is_absolute() ? path : std::filesystem::path{};
}

std::uint64_t mediaDiskCacheByteBudgetFromSettings(const QSettings& settings,
                                                   const std::uint64_t fallback) {
    bool parsed = false;
    const auto value = settings.value(QLatin1String(kBudgetBytesKey)).toLongLong(&parsed);
    if (!parsed || value <= 0)
        return fallback;
    return static_cast<std::uint64_t>(value);
}

std::unique_ptr<media::cache::MediaDiskCache>
makeMediaDiskCacheFromSettings(const QSettings& settings) {
    if (!mediaDiskCacheEnabledFromSettings(settings))
        return nullptr;
    auto directory = mediaDiskCacheDirectoryFromSettings(settings);
    if (directory.empty()) {
        const auto platformDefault = platform::userCacheDirectory("bloom");
        if (!platformDefault.has_value())
            return nullptr;
        directory = *platformDefault / "media";
    }
    media::cache::MediaDiskCacheConfig config;
    config.rootDirectory = directory;
    config.byteBudget = mediaDiskCacheByteBudgetFromSettings(
        settings, media::cache::defaultMediaDiskCacheByteBudget(directory));
    config.enabled = true;
    return std::make_unique<media::cache::MediaDiskCache>(config);
}

bool purgeMediaCaches(media::cache::MediaDiskCache* const cache,
                      runtime::OperationCache* const operationCache) {
    bool cleared = false;
    if (cache != nullptr) {
        // Removes every on-disk entry and resets the disk cache statistics. No source file is
        // consulted: the store is content-addressed and holds decoded pixels only.
        cache->clear();
        cleared = true;
    }
    if (operationCache != nullptr) {
        // The evaluator's in-memory decoded-still-image entries. Derived operation results are
        // deliberately outside "decoded media" here.
        operationCache->clearDecodedMedia();
        cleared = true;
    }
    return cleared;
}

} // namespace bloom::ui
