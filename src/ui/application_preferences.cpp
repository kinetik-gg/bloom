#include "application_preferences.hpp"

#include <QSettings>
#include <QString>

#include <cstdint>
#include <filesystem>

namespace bloom::ui {
namespace {

constexpr auto kAudioEnabledKey = "playback/audio-enabled";
constexpr auto kLoopPlaybackKey = "playback/loop";
constexpr auto kOperationCacheBytesKey = "playback/operation-cache-bytes";
constexpr auto kRamPreviewBytesKey = "playback/ram-preview-memory-bytes";
constexpr auto kMediaDiskCacheEnabledKey = "media/disk-cache-enabled";
constexpr auto kMediaDiskCacheDirectoryKey = "media/disk-cache-directory";
constexpr auto kMediaDiskCacheBudgetKey = "media/disk-cache-budget-bytes";
constexpr auto kTimelineTimeFormatKey = "timeline/time-format";
constexpr auto kTimelineSnappingKey = "timeline/snapping";
constexpr auto kTimelineKeyframesVisibleKey = "timeline/keyframes-visible";
constexpr auto kTimelineGraphEditorKey = "timeline/graph-editor";
constexpr auto kTimelineLayerColumnWidthKey = "timeline/layer-column-width";
constexpr auto kNodeLinkStyleKey = "nodes/link-style";
constexpr auto kNodeSnapKey = "nodes/snap";
constexpr auto kNodeGridSizeKey = "nodes/grid-size";
constexpr auto kViewerResolutionKey = "viewer/resolution";
constexpr auto kViewerBackgroundKey = "viewer/background";
constexpr auto kViewerSafeAreasKey = "viewer/overlay/safe-areas";
constexpr auto kViewerCentreCrossKey = "viewer/overlay/centre-cross";
constexpr auto kViewerThirdsKey = "viewer/overlay/thirds";
constexpr auto kViewerRulersKey = "viewer/overlay/rulers";
constexpr auto kViewerPixelGridKey = "viewer/overlay/pixel-grid";

// The byte-budget readers (preview_frame_cache.cpp, media_disk_cache_settings.cpp) parse the value
// through toString().toULongLong() and treat a missing, zero, or unparseable value as "default".
// Reading and writing through the same spelling keeps a saved override valid on the next launch.
[[nodiscard]] std::uint64_t byteBudgetFromSettings(const QSettings& settings,
                                                   const char* const key) {
    bool parsed = false;
    const auto bytes = settings.value(QLatin1String(key)).toString().toULongLong(&parsed);
    if (!parsed || bytes == 0)
        return 0;
    return bytes;
}

void writeByteBudget(QSettings& settings, const char* const key, const std::uint64_t bytes) {
    if (bytes == 0) {
        // 0 is "use the machine-derived default"; clearing the override keeps QSettings from
        // carrying an explicit zero that means the same thing.
        settings.remove(QLatin1String(key));
        return;
    }
    settings.setValue(QLatin1String(key), QString::number(static_cast<qulonglong>(bytes)));
}

// QVariant::toDouble() silently yields 0 for a malformed string rather than the caller's default,
// so a stored "not-a-number" would otherwise become a zero grid pitch. Parse explicitly and fall
// back on anything that is not a positive finite value.
[[nodiscard]] double positiveDoubleFromSettings(const QSettings& settings, const char* const key,
                                                const double fallback) {
    bool parsed = false;
    const auto value = settings.value(QLatin1String(key)).toDouble(&parsed);
    if (!parsed || value <= 0.0)
        return fallback;
    return value;
}

[[nodiscard]] int nonNegativeIntFromSettings(const QSettings& settings, const char* const key) {
    bool parsed = false;
    const auto value = settings.value(QLatin1String(key)).toInt(&parsed);
    if (!parsed || value < 0)
        return 0;
    return value;
}

[[nodiscard]] std::filesystem::path nativePathFromSettingsString(const QString& text) {
#if defined(_WIN32)
    return std::filesystem::path(text.toStdWString());
#else
    return std::filesystem::path(text.toStdString());
#endif
}

[[nodiscard]] std::string absoluteDirectoryFromSettings(const QSettings& settings) {
    const auto text = settings.value(QLatin1String(kMediaDiskCacheDirectoryKey)).toString();
    if (text.isEmpty())
        return {};
    const auto path = nativePathFromSettingsString(text);
    // Matches mediaDiskCacheDirectoryFromSettings(): only an absolute override is honored.
    return path.is_absolute() ? path.string() : std::string{};
}

[[nodiscard]] QString directorySettingText(const std::string& directory) {
    if (directory.empty())
        return {};
    const auto path = std::filesystem::path(directory);
    if (!path.is_absolute())
        return {};
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

[[nodiscard]] TimelineTimeFormat timelineTimeFormatFromString(const QString& value) {
    return value == QLatin1StringView("timecode") ? TimelineTimeFormat::Timecode
                                                  : TimelineTimeFormat::Frames;
}

[[nodiscard]] NodeLinkStyle nodeLinkStyleFromString(const QString& value) {
    if (value == QLatin1StringView("straight"))
        return NodeLinkStyle::Straight;
    if (value == QLatin1StringView("angled"))
        return NodeLinkStyle::Angled;
    return NodeLinkStyle::Spline;
}

[[nodiscard]] ViewerResolutionPreference viewerResolutionFromString(const QString& value) {
    if (value == QLatin1StringView("Full"))
        return ViewerResolutionPreference::Full;
    if (value == QLatin1StringView("Half"))
        return ViewerResolutionPreference::Half;
    if (value == QLatin1StringView("Quarter"))
        return ViewerResolutionPreference::Quarter;
    return ViewerResolutionPreference::Auto;
}

[[nodiscard]] ViewerBackgroundPreference viewerBackgroundFromString(const QString& value) {
    if (value == QLatin1StringView("Checkerboard"))
        return ViewerBackgroundPreference::Checkerboard;
    if (value == QLatin1StringView("Black"))
        return ViewerBackgroundPreference::Black;
    if (value == QLatin1StringView("White"))
        return ViewerBackgroundPreference::White;
    return ViewerBackgroundPreference::Solid;
}

} // namespace

ApplicationPreferences defaultApplicationPreferences() noexcept { return ApplicationPreferences{}; }

ApplicationPreferences loadApplicationPreferences(const QSettings& settings) {
    ApplicationPreferences preferences;
    preferences.audioEnabled = settings.value(QLatin1String(kAudioEnabledKey), true).toBool();
    preferences.loopPlayback = settings.value(QLatin1String(kLoopPlaybackKey), true).toBool();
    preferences.operationCacheBytes = byteBudgetFromSettings(settings, kOperationCacheBytesKey);
    preferences.ramPreviewBytes = byteBudgetFromSettings(settings, kRamPreviewBytesKey);
    preferences.mediaDiskCacheEnabled =
        settings.value(QLatin1String(kMediaDiskCacheEnabledKey), true).toBool();
    preferences.mediaDiskCacheDirectory = absoluteDirectoryFromSettings(settings);
    preferences.mediaDiskCacheBudgetBytes =
        byteBudgetFromSettings(settings, kMediaDiskCacheBudgetKey);
    preferences.timelineTimeFormat = timelineTimeFormatFromString(
        settings.value(QLatin1String(kTimelineTimeFormatKey), QStringLiteral("frames")).toString());
    preferences.timelineSnapping =
        settings.value(QLatin1String(kTimelineSnappingKey), true).toBool();
    preferences.timelineKeyframesVisible =
        settings.value(QLatin1String(kTimelineKeyframesVisibleKey), true).toBool();
    preferences.timelineGraphEditor =
        settings.value(QLatin1String(kTimelineGraphEditorKey), false).toBool();
    preferences.timelineLayerColumnWidth =
        nonNegativeIntFromSettings(settings, kTimelineLayerColumnWidthKey);
    preferences.nodeLinkStyle =
        nodeLinkStyleFromString(settings.value(QLatin1String(kNodeLinkStyleKey)).toString());
    preferences.nodeSnap = settings.value(QLatin1String(kNodeSnapKey), false).toBool();
    preferences.nodeGridSize = positiveDoubleFromSettings(settings, kNodeGridSizeKey, 16.0);
    preferences.viewerResolution = viewerResolutionFromString(
        settings.value(QLatin1String(kViewerResolutionKey), QStringLiteral("Auto")).toString());
    preferences.viewerBackground = viewerBackgroundFromString(
        settings.value(QLatin1String(kViewerBackgroundKey), QStringLiteral("Solid")).toString());
    preferences.viewerSafeAreas =
        settings.value(QLatin1String(kViewerSafeAreasKey), false).toBool();
    preferences.viewerCentreCross =
        settings.value(QLatin1String(kViewerCentreCrossKey), false).toBool();
    preferences.viewerThirds = settings.value(QLatin1String(kViewerThirdsKey), false).toBool();
    preferences.viewerRulers = settings.value(QLatin1String(kViewerRulersKey), false).toBool();
    preferences.viewerPixelGrid =
        settings.value(QLatin1String(kViewerPixelGridKey), false).toBool();
    return preferences;
}

void saveApplicationPreferences(QSettings& settings, const ApplicationPreferences& preferences) {
    settings.setValue(QLatin1String(kAudioEnabledKey), preferences.audioEnabled);
    settings.setValue(QLatin1String(kLoopPlaybackKey), preferences.loopPlayback);
    writeByteBudget(settings, kOperationCacheBytesKey, preferences.operationCacheBytes);
    writeByteBudget(settings, kRamPreviewBytesKey, preferences.ramPreviewBytes);
    settings.setValue(QLatin1String(kMediaDiskCacheEnabledKey), preferences.mediaDiskCacheEnabled);
    const auto directoryText = directorySettingText(preferences.mediaDiskCacheDirectory);
    if (directoryText.isEmpty())
        settings.remove(QLatin1String(kMediaDiskCacheDirectoryKey));
    else
        settings.setValue(QLatin1String(kMediaDiskCacheDirectoryKey), directoryText);
    writeByteBudget(settings, kMediaDiskCacheBudgetKey, preferences.mediaDiskCacheBudgetBytes);
    settings.setValue(QLatin1String(kTimelineTimeFormatKey),
                      QLatin1String(timelineTimeFormatValue(preferences.timelineTimeFormat)));
    settings.setValue(QLatin1String(kTimelineSnappingKey), preferences.timelineSnapping);
    settings.setValue(QLatin1String(kTimelineKeyframesVisibleKey),
                      preferences.timelineKeyframesVisible);
    settings.setValue(QLatin1String(kTimelineGraphEditorKey), preferences.timelineGraphEditor);
    if (preferences.timelineLayerColumnWidth <= 0)
        settings.remove(QLatin1String(kTimelineLayerColumnWidthKey));
    else
        settings.setValue(QLatin1String(kTimelineLayerColumnWidthKey),
                          preferences.timelineLayerColumnWidth);
    settings.setValue(QLatin1String(kNodeLinkStyleKey),
                      QLatin1String(nodeLinkStyleValue(preferences.nodeLinkStyle)));
    settings.setValue(QLatin1String(kNodeSnapKey), preferences.nodeSnap);
    settings.setValue(QLatin1String(kNodeGridSizeKey), preferences.nodeGridSize);
    settings.setValue(QLatin1String(kViewerResolutionKey),
                      QLatin1String(viewerResolutionValue(preferences.viewerResolution)));
    settings.setValue(QLatin1String(kViewerBackgroundKey),
                      QLatin1String(viewerBackgroundValue(preferences.viewerBackground)));
    settings.setValue(QLatin1String(kViewerSafeAreasKey), preferences.viewerSafeAreas);
    settings.setValue(QLatin1String(kViewerCentreCrossKey), preferences.viewerCentreCross);
    settings.setValue(QLatin1String(kViewerThirdsKey), preferences.viewerThirds);
    settings.setValue(QLatin1String(kViewerRulersKey), preferences.viewerRulers);
    settings.setValue(QLatin1String(kViewerPixelGridKey), preferences.viewerPixelGrid);
}

const char* timelineTimeFormatValue(const TimelineTimeFormat format) noexcept {
    return format == TimelineTimeFormat::Timecode ? "timecode" : "frames";
}

const char* nodeLinkStyleValue(const NodeLinkStyle style) noexcept {
    switch (style) {
    case NodeLinkStyle::Straight:
        return "straight";
    case NodeLinkStyle::Angled:
        return "angled";
    case NodeLinkStyle::Spline:
        break;
    }
    return "spline";
}

const char* viewerResolutionValue(const ViewerResolutionPreference policy) noexcept {
    switch (policy) {
    case ViewerResolutionPreference::Full:
        return "Full";
    case ViewerResolutionPreference::Half:
        return "Half";
    case ViewerResolutionPreference::Quarter:
        return "Quarter";
    case ViewerResolutionPreference::Auto:
        break;
    }
    return "Auto";
}

const char* viewerBackgroundValue(const ViewerBackgroundPreference background) noexcept {
    switch (background) {
    case ViewerBackgroundPreference::Checkerboard:
        return "Checkerboard";
    case ViewerBackgroundPreference::Black:
        return "Black";
    case ViewerBackgroundPreference::White:
        return "White";
    case ViewerBackgroundPreference::Solid:
        break;
    }
    return "Solid";
}

} // namespace bloom::ui
