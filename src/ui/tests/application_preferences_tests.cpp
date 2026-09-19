#include <bloom/ui/application_preferences.hpp>

#include <QApplication>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <iostream>
#include <source_location>
#include <string_view>

namespace {

// The small "Expectations" idiom every Bloom test file already uses (see
// src/ui/tests/media_disk_cache_settings_tests.cpp for the same shape).
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

[[nodiscard]] QSettings openSettings(QTemporaryDir& directory) {
    return QSettings(directory.filePath(QStringLiteral("bloom.ini")), QSettings::IniFormat);
}

void testDefaultsRoundTrip(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    const auto loaded = ui::loadApplicationPreferences(settings);
    check.expect(loaded == ui::defaultApplicationPreferences(),
                 "an empty QSettings loads exactly the named defaults");

    ui::saveApplicationPreferences(settings, ui::defaultApplicationPreferences());
    const auto reloaded = ui::loadApplicationPreferences(settings);
    check.expect(reloaded == ui::defaultApplicationPreferences(),
                 "saving the defaults and reloading them is stable");
}

void testFullRoundTrip(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);

    ui::ApplicationPreferences prefs;
    prefs.audioEnabled = false;
    prefs.loopPlayback = false;
    prefs.operationCacheBytes = 10ULL * 1024 * 1024 * 1024;
    prefs.ramPreviewBytes = 6ULL * 1024 * 1024 * 1024;
    prefs.mediaDiskCacheEnabled = false;
    prefs.mediaDiskCacheDirectory = directory.filePath(QStringLiteral("media-cache")).toStdString();
    prefs.mediaDiskCacheBudgetBytes = 2ULL * 1024 * 1024 * 1024;
    prefs.timelineTimeFormat = ui::TimelineTimeFormat::Timecode;
    prefs.timelineSnapping = false;
    prefs.timelineKeyframesVisible = false;
    prefs.timelineGraphEditor = true;
    prefs.timelineLayerColumnWidth = 360;
    prefs.nodeLinkStyle = ui::NodeLinkStyle::Angled;
    prefs.nodeSnap = true;
    prefs.nodeGridSize = 24.0;
    prefs.viewerResolution = ui::ViewerResolutionPreference::Half;
    prefs.viewerBackground = ui::ViewerBackgroundPreference::Checkerboard;
    prefs.viewerSafeAreas = true;
    prefs.viewerCentreCross = true;
    prefs.viewerThirds = true;
    prefs.viewerRulers = true;
    prefs.viewerPixelGrid = true;

    ui::saveApplicationPreferences(settings, prefs);
    const auto reloaded = ui::loadApplicationPreferences(settings);
    check.expect(reloaded == prefs, "every global preference round-trips through QSettings");
}

void testInvalidValuesFallBack(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    settings.setValue(QStringLiteral("timeline/time-format"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("nodes/link-style"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("viewer/resolution"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("viewer/background"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("nodes/grid-size"), QStringLiteral("not-a-number"));
    settings.setValue(QStringLiteral("playback/operation-cache-bytes"),
                      QStringLiteral("not-a-number"));
    settings.setValue(QStringLiteral("playback/ram-preview-memory-bytes"), QStringLiteral("-5"));
    settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"), QStringLiteral("0"));

    const auto loaded = ui::loadApplicationPreferences(settings);
    check.expect(loaded.timelineTimeFormat == ui::TimelineTimeFormat::Frames,
                 "an unrecognized time format reads as Frames");
    check.expect(loaded.nodeLinkStyle == ui::NodeLinkStyle::Spline,
                 "an unrecognized link style reads as Spline");
    check.expect(loaded.viewerResolution == ui::ViewerResolutionPreference::Auto,
                 "an unrecognized resolution reads as Auto");
    check.expect(loaded.viewerBackground == ui::ViewerBackgroundPreference::Solid,
                 "an unrecognized background reads as Solid");
    check.expect(loaded.nodeGridSize == 16.0, "an unparseable grid size reads as 16");
    check.expect(loaded.operationCacheBytes == 0,
                 "an unparseable operation-cache budget reads as the default");
    check.expect(loaded.ramPreviewBytes == 0, "a negative RAM-preview budget reads as the default");
    check.expect(loaded.mediaDiskCacheBudgetBytes == 0,
                 "an explicit zero disk budget reads as the default");
}

void testDirectoryOverrideMustBeAbsolute(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    settings.setValue(QStringLiteral("media/disk-cache-directory"),
                      QStringLiteral("relative/path"));
    check.expect(ui::loadApplicationPreferences(settings).mediaDiskCacheDirectory.empty(),
                 "a relative disk-cache directory is refused, not resolved against cwd");

    const auto absolute = directory.filePath(QStringLiteral("override-cache"));
    settings.setValue(QStringLiteral("media/disk-cache-directory"), absolute);
    check.expect(ui::loadApplicationPreferences(settings).mediaDiskCacheDirectory ==
                     absolute.toStdString(),
                 "an absolute disk-cache directory is honored verbatim");

    settings.setValue(QStringLiteral("media/disk-cache-directory"), absolute);
    ui::ApplicationPreferences relative;
    relative.mediaDiskCacheDirectory = "relative/path";
    ui::saveApplicationPreferences(settings, relative);
    check.expect(
        !settings.contains(QStringLiteral("media/disk-cache-directory")),
        "saving a relative disk-cache directory refuses it and clears the stored override");
}

void testZeroBudgetClearsOverride(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    ui::ApplicationPreferences prefs;
    prefs.operationCacheBytes = 1024;
    ui::saveApplicationPreferences(settings, prefs);
    check.expect(settings.contains(QStringLiteral("playback/operation-cache-bytes")),
                 "a non-zero budget is written as an override");

    prefs.operationCacheBytes = 0;
    ui::saveApplicationPreferences(settings, prefs);
    check.expect(!settings.contains(QStringLiteral("playback/operation-cache-bytes")),
                 "a zero budget clears the override so the machine default applies");
    check.expect(ui::loadApplicationPreferences(settings).operationCacheBytes == 0,
                 "a cleared budget reads back as the default");
}

void testStoredSpellingsMatchReaders(Expectations& check) {
    check.expect(
        std::string_view(ui::timelineTimeFormatValue(ui::TimelineTimeFormat::Frames)) == "frames" &&
            std::string_view(ui::timelineTimeFormatValue(ui::TimelineTimeFormat::Timecode)) ==
                "timecode",
        "the timeline time-format vocabulary matches the existing reader");
    check.expect(
        std::string_view(ui::nodeLinkStyleValue(ui::NodeLinkStyle::Spline)) == "spline" &&
            std::string_view(ui::nodeLinkStyleValue(ui::NodeLinkStyle::Straight)) == "straight" &&
            std::string_view(ui::nodeLinkStyleValue(ui::NodeLinkStyle::Angled)) == "angled",
        "the node link-style vocabulary matches the existing reader");
    check.expect(
        std::string_view(ui::viewerResolutionValue(ui::ViewerResolutionPreference::Auto)) ==
                "Auto" &&
            std::string_view(ui::viewerResolutionValue(ui::ViewerResolutionPreference::Quarter)) ==
                "Quarter",
        "the viewer resolution vocabulary matches the existing reader");
    check.expect(std::string_view(
                     ui::viewerBackgroundValue(ui::ViewerBackgroundPreference::Solid)) == "Solid" &&
                     std::string_view(ui::viewerBackgroundValue(
                         ui::ViewerBackgroundPreference::Checkerboard)) == "Checkerboard",
                 "the viewer background vocabulary matches the existing reader");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations check;
    testDefaultsRoundTrip(check);
    testFullRoundTrip(check);
    testInvalidValuesFallBack(check);
    testDirectoryOverrideMustBeAbsolute(check);
    testZeroBudgetClearsOverride(check);
    testStoredSpellingsMatchReaders(check);
    return check.failures() == 0 ? 0 : 1;
}
