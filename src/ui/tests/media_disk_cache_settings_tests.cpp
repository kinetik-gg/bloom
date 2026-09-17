#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/ui/media_disk_cache_settings.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <iostream>
#include <source_location>
#include <string_view>

namespace {

// The small "Expectations" idiom every Bloom test file already uses (see
// src/ui/tests/main_window_chrome_tests.cpp and src/ui/tests/ram_preview_tests.cpp), duplicated
// here rather than shared, matching the same precedent those two files themselves follow.
class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

namespace ui = bloom::ui;
namespace cache = bloom::media::cache;

void testEnabledDefaultsTrue(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    check.expect(ui::mediaDiskCacheEnabledFromSettings(settings),
                 "an unset enabled setting defaults to true");
    settings.setValue(QStringLiteral("media/disk-cache-enabled"), false);
    check.expect(!ui::mediaDiskCacheEnabledFromSettings(settings), "an explicit false is honored");
}

void testDirectoryOverrideMustBeAbsolute(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    check.expect(ui::mediaDiskCacheDirectoryFromSettings(settings).empty(),
                 "an unset directory reads as empty (use the platform default)");
    settings.setValue(QStringLiteral("media/disk-cache-directory"),
                      QStringLiteral("relative/path"));
    check.expect(ui::mediaDiskCacheDirectoryFromSettings(settings).empty(),
                 "a relative directory override is refused, not silently resolved against cwd");
    const auto absolute = directory.filePath(QStringLiteral("override-cache"));
    settings.setValue(QStringLiteral("media/disk-cache-directory"), absolute);
    check.expect(ui::mediaDiskCacheDirectoryFromSettings(settings).string() ==
                     absolute.toStdString(),
                 "an absolute directory override is honored verbatim");
}

void testByteBudgetFallsBackOnInvalidValues(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    constexpr std::uint64_t fallback = 123'456'789ULL;
    check.expect(ui::mediaDiskCacheByteBudgetFromSettings(settings, fallback) == fallback,
                 "an unset budget uses the caller's fallback");
    for (const auto* value : {"0", "-1", "not-a-number"}) {
        settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"),
                          QString::fromLatin1(value));
        check.expect(ui::mediaDiskCacheByteBudgetFromSettings(settings, fallback) == fallback,
                     "an invalid budget falls back rather than being silently accepted");
    }
    settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"), QStringLiteral("4096"));
    check.expect(ui::mediaDiskCacheByteBudgetFromSettings(settings, fallback) == 4096,
                 "a valid positive budget is honored");
}

void testMakeMediaDiskCacheFromSettings(Expectations& check) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
    const auto root = directory.filePath(QStringLiteral("explicit-cache"));
    settings.setValue(QStringLiteral("media/disk-cache-directory"), root);
    settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"), QStringLiteral("4194304"));
    auto built = ui::makeMediaDiskCacheFromSettings(settings);
    check.expect(built != nullptr, "an explicit directory and budget build a usable cache");
    if (built != nullptr) {
        check.expect(built->rootDirectory().string() == root.toStdString(),
                     "the built cache's root is exactly the configured directory");
        check.expect(built->byteBudget() == 4194304,
                     "the built cache's budget is what settings said");
        check.expect(built->enabled(), "a cache built from default-enabled settings is enabled");
    }

    settings.setValue(QStringLiteral("media/disk-cache-enabled"), false);
    check.expect(ui::makeMediaDiskCacheFromSettings(settings) == nullptr,
                 "disabled-in-settings builds no cache at all");
}

// mediaDiskCacheStatusText() (window_status_bar.hpp) is the free function the status bar's cell
// calls; tested directly here since it needs only a statistics value, not a live cache or widget.
void testMediaDiskCacheStatusText(Expectations& check) {
    cache::MediaDiskCacheStatistics empty;
    check.expect(ui::mediaDiskCacheStatusText(empty, /*enabled=*/false) ==
                     QStringLiteral("Disk cache off"),
                 "a disabled cache always reports 'Disk cache off', regardless of statistics");
    check.expect(ui::mediaDiskCacheStatusText(empty, /*enabled=*/true).isEmpty(),
                 "an enabled cache with no lookups yet and no resident entries reports nothing");

    cache::MediaDiskCacheStatistics warm;
    warm.hits = 3;
    warm.misses = 1;
    warm.storedBytes = 2ULL * 1024 * 1024;
    warm.entryCount = 4;
    const auto text = ui::mediaDiskCacheStatusText(warm, /*enabled=*/true);
    check.expect(text.contains(QStringLiteral("75")),
                 "3 hits of 4 lookups is reported as a 75% hit rate");
    check.expect(text.contains(QStringLiteral("MB")),
                 "resident bytes are reported with a readable unit");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations check;
    testEnabledDefaultsTrue(check);
    testDirectoryOverrideMustBeAbsolute(check);
    testByteBudgetFallsBackOnInvalidValues(check);
    testMakeMediaDiskCacheFromSettings(check);
    testMediaDiskCacheStatusText(check);
    return check.failures() == 0 ? 0 : 1;
}
