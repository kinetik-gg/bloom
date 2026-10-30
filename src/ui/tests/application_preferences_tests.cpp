#include <bloom/ui/application_preferences.hpp>

#include <QApplication>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cmath>
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
namespace runtime = bloom::runtime;

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
    prefs.ramHeadroomBytes = 6ULL * 1024 * 1024 * 1024;
    prefs.vramHeadroomBytes = 512ULL * 1024 * 1024;
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
    prefs.viewerBackground = ui::ViewerBackgroundPreference::Grey;
    prefs.viewerTransparencyGrid = true;
    prefs.viewerSafeAreas = true;
    prefs.viewerCentreCross = true;
    prefs.viewerThirds = true;
    prefs.viewerRulers = true;
    prefs.viewerPixelGrid = true;
    prefs.soundsEnabled = false;
    prefs.soundVolumePercent = 73;
    prefs.eventSoundsEnabled = false;
    prefs.interactionSoundsEnabled = true;
    prefs.renderNotificationsEnabled = false;

    ui::saveApplicationPreferences(settings, prefs);
    const auto reloaded = ui::loadApplicationPreferences(settings);
    check.expect(reloaded == prefs, "every global preference round-trips through QSettings");
}

void testHeadroomPreferences(Expectations& check) {
    constexpr auto gib = std::uint64_t{1024} * 1024 * 1024;
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    const auto defaults = ui::loadApplicationPreferences(settings);
    check.expect(defaults.ramHeadroomBytes == runtime::defaultRamHeadroomBytes() &&
                     defaults.vramHeadroomBytes == runtime::defaultVramHeadroomBytes() &&
                     ui::defaultApplicationPreferences().ramHeadroomBytes ==
                         runtime::defaultRamHeadroomBytes(),
                 "RAM and VRAM headroom default to the computed 10% of capacity");

    ui::ApplicationPreferences prefs;
    prefs.ramHeadroomBytes = 0;
    prefs.vramHeadroomBytes = 256 * gib;
    ui::saveApplicationPreferences(settings, prefs);
    check.expect(settings.value(QStringLiteral("memory/ramHeadroomBytes")).toString() ==
                         QStringLiteral("0") &&
                     settings.value(QStringLiteral("memory/vramHeadroomBytes")).toString() ==
                         QString::number(256 * gib),
                 "headroom is stored as decimal byte strings under the memory/ keys");
    check.expect(ui::loadApplicationPreferences(settings) == prefs,
                 "zero and the 256 GiB maximum round-trip");

    for (const auto* bad : {"garbage", "-5", "1.5", "", " 7", "0x10", "99999999999999999999"}) {
        settings.setValue(QStringLiteral("memory/ramHeadroomBytes"), QString::fromLatin1(bad));
        check.expect(ui::loadApplicationPreferences(settings).ramHeadroomBytes ==
                         runtime::defaultRamHeadroomBytes(),
                     "garbage or overflowing headroom reads as the computed default");
    }
    settings.setValue(QStringLiteral("memory/ramHeadroomBytes"), QString::number(900 * gib));
    check.expect(ui::loadApplicationPreferences(settings).ramHeadroomBytes ==
                     runtime::defaultRamHeadroomBytes(),
                 "headroom above the 256 GiB maximum reads as the default");

    settings.clear();
    settings.setValue(QStringLiteral("playback/operation-cache-bytes"), QStringLiteral("1024"));
    settings.setValue(QStringLiteral("playback/ram-preview-memory-bytes"), QStringLiteral("2048"));
    ui::saveApplicationPreferences(settings, ui::loadApplicationPreferences(settings));
    check.expect(ui::loadApplicationPreferences(settings) == ui::defaultApplicationPreferences() &&
                     settings.value(QStringLiteral("playback/operation-cache-bytes")).toString() ==
                         QStringLiteral("1024"),
                 "the old cache-budget keys are ignored (neither read nor rewritten)");
}

void testSoundDefaultsAndBounds(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    const auto defaults = ui::loadApplicationPreferences(settings);
    check.expect(defaults.soundsEnabled && defaults.eventSoundsEnabled &&
                     defaults.renderNotificationsEnabled,
                 "sounds, event sounds and render notifications default on");
    check.expect(!defaults.interactionSoundsEnabled, "interaction sounds default off");
    check.expect(defaults.soundVolumePercent == 40, "the default volume is 40 percent");
    settings.setValue(QStringLiteral("sound/volume-percent"), 400);
    check.expect(ui::loadApplicationPreferences(settings).soundVolumePercent == 40,
                 "an out-of-range stored volume falls back to the default");
    auto prefs = defaults;
    prefs.soundVolumePercent = 250;
    ui::saveApplicationPreferences(settings, prefs);
    check.expect(ui::loadApplicationPreferences(settings).soundVolumePercent == 100,
                 "saving clamps the volume");
}

void testInvalidValuesFallBack(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    settings.setValue(QStringLiteral("timeline/time-format"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("nodes/link-style"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("viewer/resolution"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("viewer/background"), QStringLiteral("nonsense"));
    settings.setValue(QStringLiteral("nodes/grid-size"), QStringLiteral("not-a-number"));
    settings.setValue(QStringLiteral("media/disk-cache-budget-bytes"), QStringLiteral("0"));

    const auto loaded = ui::loadApplicationPreferences(settings);
    check.expect(loaded.timelineTimeFormat == ui::TimelineTimeFormat::Frames,
                 "an unrecognized time format reads as Frames");
    check.expect(loaded.nodeLinkStyle == ui::NodeLinkStyle::Spline,
                 "an unrecognized link style reads as Spline");
    check.expect(loaded.viewerResolution == ui::ViewerResolutionPreference::Auto,
                 "an unrecognized resolution reads as Auto");
    check.expect(loaded.viewerBackground == ui::ViewerBackgroundPreference::Solid &&
                     !loaded.viewerTransparencyGrid,
                 "an unrecognized background reads as Solid with the grid off");
    check.expect(loaded.nodeGridSize == 16.0, "an unparseable grid size reads as 16");
    check.expect(loaded.mediaDiskCacheBudgetBytes == 0,
                 "an explicit zero disk budget reads as the default");
}

// The pre-split Checkerboard background (which drew the checker in the frame) keeps showing a
// checker: it reads as the Solid surround with the Transparency Grid on, and a grid value stored
// since always wins.
void testLegacyCheckerboardMigrates(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);
    settings.setValue(QStringLiteral("viewer/background"), QStringLiteral("Checkerboard"));
    auto loaded = ui::loadApplicationPreferences(settings);
    check.expect(loaded.viewerBackground == ui::ViewerBackgroundPreference::Solid &&
                     loaded.viewerTransparencyGrid,
                 "a stored Checkerboard reads as Solid with the Transparency Grid on");
    settings.setValue(QStringLiteral("viewer/transparency-grid"), false);
    loaded = ui::loadApplicationPreferences(settings);
    check.expect(loaded.viewerBackground == ui::ViewerBackgroundPreference::Solid &&
                     !loaded.viewerTransparencyGrid,
                 "a grid value stored since the migration is never overridden");
    ui::saveApplicationPreferences(settings, loaded);
    check.expect(settings.value(QStringLiteral("viewer/background")).toString() ==
                         QStringLiteral("Solid") &&
                     !settings.value(QStringLiteral("viewer/transparency-grid")).toBool(),
                 "saving writes the migrated pair");
    QTemporaryDir fresh;
    auto empty = openSettings(fresh);
    const auto defaults = ui::loadApplicationPreferences(empty);
    check.expect(defaults.viewerBackground == ui::ViewerBackgroundPreference::Solid &&
                     !defaults.viewerTransparencyGrid,
                 "the defaults are the Solid surround with the grid off (After Effects)");
}

void testNonFiniteGridSizeFallsBack(Expectations& check) {
    QTemporaryDir directory;
    auto settings = openSettings(directory);

    // QVariant::toDouble() parses "nan"/"inf"/"infinity" successfully, so a persisted non-finite
    // value would otherwise survive as a NaN or infinite grid pitch and reach node layout/painting.
    constexpr std::array<std::string_view, 5> nonFinite{"nan", "inf", "+inf", "infinity",
                                                        "negativeinf"};
    for (const auto value : nonFinite) {
        settings.setValue(QStringLiteral("nodes/grid-size"),
                          QString::fromLatin1(value.data(), static_cast<int>(value.size())));
        const auto loaded = ui::loadApplicationPreferences(settings);
        check.expect(loaded.nodeGridSize == 16.0,
                     "a persisted non-finite grid size reads as the 16.0 default");
        check.expect(std::isfinite(loaded.nodeGridSize), "the loaded grid size is always finite");
    }
    settings.setValue(QStringLiteral("nodes/grid-size"), QStringLiteral("-inf"));
    check.expect(ui::loadApplicationPreferences(settings).nodeGridSize == 16.0,
                 "a persisted -inf grid size reads as the 16.0 default");

    // A genuine positive finite value still round-trips untouched.
    settings.setValue(QStringLiteral("nodes/grid-size"), 24.0);
    check.expect(ui::loadApplicationPreferences(settings).nodeGridSize == 24.0,
                 "a positive finite grid size is honored");

    // The model owns only its global keys: loading and saving must not touch a project key.
    settings.setValue(QStringLiteral("project/canvas"), QStringLiteral("untouched"));
    ui::saveApplicationPreferences(settings, ui::loadApplicationPreferences(settings));
    check.expect(settings.value(QStringLiteral("project/canvas")).toString() ==
                     QStringLiteral("untouched"),
                 "loading and saving global preferences never writes a project key");
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
    prefs.mediaDiskCacheBudgetBytes = 1024;
    ui::saveApplicationPreferences(settings, prefs);
    check.expect(settings.contains(QStringLiteral("media/disk-cache-budget-bytes")),
                 "a non-zero budget is written as an override");

    prefs.mediaDiskCacheBudgetBytes = 0;
    ui::saveApplicationPreferences(settings, prefs);
    check.expect(!settings.contains(QStringLiteral("media/disk-cache-budget-bytes")),
                 "a zero budget clears the override so the machine default applies");
    check.expect(ui::loadApplicationPreferences(settings).mediaDiskCacheBudgetBytes == 0,
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
                         ui::ViewerBackgroundPreference::Grey)) == "Gray",
                 "the viewer background vocabulary matches the existing reader");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations check;
    testDefaultsRoundTrip(check);
    testFullRoundTrip(check);
    testHeadroomPreferences(check);
    testSoundDefaultsAndBounds(check);
    testInvalidValuesFallBack(check);
    testLegacyCheckerboardMigrates(check);
    testNonFiniteGridSizeFallsBack(check);
    testDirectoryOverrideMustBeAbsolute(check);
    testZeroBudgetClearsOverride(check);
    testStoredSpellingsMatchReaders(check);
    return check.failures() == 0 ? 0 : 1;
}
