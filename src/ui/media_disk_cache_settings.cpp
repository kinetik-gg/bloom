#include <bloom/ui/media_disk_cache_settings.hpp>

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/platform/cache_directory.hpp>

#include <QMessageBox>
#include <QObject>
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

bool confirmAndClearMediaDiskCache(QWidget* const parent,
                                   media::cache::MediaDiskCache* const cache) {
    if (cache == nullptr) {
        QMessageBox::information(parent, QObject::tr("Clear Media Cache"),
                                 QObject::tr("The media disk cache is not enabled for this "
                                             "session."));
        return false;
    }
    const auto stats = cache->statistics();
    const auto question =
        stats.entryCount == 0
            ? QObject::tr("The media disk cache is already empty. Clear it anyway?")
            : QObject::tr("Clear %1 cached frames from disk? Scrubbing will decode again on next "
                          "use.")
                  .arg(stats.entryCount);
    const auto choice =
        QMessageBox::question(parent, QObject::tr("Clear Media Cache"), question,
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice != QMessageBox::Yes)
        return false;
    cache->clear();
    return true;
}

} // namespace bloom::ui
