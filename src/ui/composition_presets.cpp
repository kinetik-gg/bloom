#include <bloom/ui/composition_presets.hpp>

#include <QSettings>

#include <algorithm>
#include <cstdint>

namespace bloom::ui {
namespace {

constexpr auto kGroup = "compositionPresets";
constexpr qsizetype kMaximumNameBytes = 128;

[[nodiscard]] CompositionPreset builtIn(const char* name, const std::uint32_t width,
                                        const std::uint32_t height) {
    CompositionPreset preset;
    preset.name = QString::fromUtf8(name);
    preset.builtIn = true;
    preset.width = width;
    preset.height = height;
    return preset;
}

[[nodiscard]] std::optional<std::uint32_t> unsignedValue(const QSettings& settings,
                                                         const QString& key) {
    bool ok = false;
    const auto value = settings.value(key).toULongLong(&ok);
    if (!ok || value == 0 || value > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::optional<core::RationalTime> timeValue(const QSettings& settings,
                                                          const QString& key) {
    bool numeratorOk = false;
    bool denominatorOk = false;
    const auto numerator =
        settings.value(key + QStringLiteral("Numerator")).toLongLong(&numeratorOk);
    const auto denominator =
        settings.value(key + QStringLiteral("Denominator")).toLongLong(&denominatorOk);
    if (!numeratorOk || !denominatorOk || denominator <= 0)
        return std::nullopt;
    return core::RationalTime::create(numerator, denominator);
}

void writeTime(QSettings& settings, const QString& key, const core::RationalTime time) {
    settings.setValue(key + QStringLiteral("Numerator"), static_cast<qlonglong>(time.numerator()));
    settings.setValue(key + QStringLiteral("Denominator"),
                      static_cast<qlonglong>(time.denominator()));
}

} // namespace

std::vector<CompositionPreset> builtInCompositionPresets() {
    return {builtIn("HD 1080p", 1920, 1080),   builtIn("HD 720p", 1280, 720),
            builtIn("UHD 4K", 3840, 2160),     builtIn("DCI 2K", 2048, 1080),
            builtIn("DCI 4K", 4096, 2160),     builtIn("Vertical 1080 × 1920", 1080, 1920),
            builtIn("Square 1080", 1080, 1080)};
}

std::vector<CompositionPreset> loadUserCompositionPresets(QSettings& settings) {
    std::vector<CompositionPreset> presets;
    const auto count = settings.beginReadArray(QString::fromLatin1(kGroup));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        CompositionPreset preset;
        preset.name = settings.value(QStringLiteral("name")).toString().trimmed();
        const auto width = unsignedValue(settings, QStringLiteral("width"));
        const auto height = unsignedValue(settings, QStringLiteral("height"));
        const auto parNumerator = unsignedValue(settings, QStringLiteral("pixelAspectNumerator"));
        const auto parDenominator =
            unsignedValue(settings, QStringLiteral("pixelAspectDenominator"));
        const auto rateNumerator = unsignedValue(settings, QStringLiteral("frameRateNumerator"));
        const auto rateDenominator =
            unsignedValue(settings, QStringLiteral("frameRateDenominator"));
        const auto duration = timeValue(settings, QStringLiteral("duration"));
        const auto start = timeValue(settings, QStringLiteral("start"));
        const auto aspect = parNumerator && parDenominator
                                ? core::PixelAspectRatio::create(*parNumerator, *parDenominator)
                                : std::nullopt;
        const auto rate = rateNumerator && rateDenominator
                              ? document::FrameRate::create(*rateNumerator, *rateDenominator)
                              : std::nullopt;
        // An unreadable entry is skipped rather than guessed: presets are a convenience, and a
        // half-read one would silently author the wrong composition.
        if (preset.name.isEmpty() || !width || !height || !aspect || !rate || !duration ||
            *duration <= core::RationalTime{} || !start || *start < core::RationalTime{} ||
            !document::CompositionFormat::create(*width, *height, *aspect, *rate))
            continue;
        preset.width = *width;
        preset.height = *height;
        preset.pixelAspect = *aspect;
        preset.frameRate = *rate;
        preset.duration = *duration;
        preset.timecode = document::CompositionTimecode{
            *start, settings.value(QStringLiteral("dropFrame")).toBool()};
        const auto colour = settings.value(QStringLiteral("background")).toList();
        core::Color4d background{0.0, 0.0, 0.0, 1.0};
        if (colour.size() == 4)
            background = {colour[0].toDouble(), colour[1].toDouble(), colour[2].toDouble(),
                          colour[3].toDouble()};
        preset.background = background.isValid() ? background : core::Color4d{0.0, 0.0, 0.0, 1.0};
        presets.push_back(std::move(preset));
    }
    settings.endArray();
    return presets;
}

void saveUserCompositionPresets(QSettings& settings,
                                const std::vector<CompositionPreset>& presets) {
    settings.remove(QString::fromLatin1(kGroup));
    settings.beginWriteArray(QString::fromLatin1(kGroup), static_cast<int>(presets.size()));
    int index = 0;
    for (const auto& preset : presets) {
        if (preset.builtIn)
            continue;
        settings.setArrayIndex(index++);
        settings.setValue(QStringLiteral("name"), preset.name);
        settings.setValue(QStringLiteral("width"), preset.width);
        settings.setValue(QStringLiteral("height"), preset.height);
        settings.setValue(QStringLiteral("pixelAspectNumerator"), preset.pixelAspect.numerator());
        settings.setValue(QStringLiteral("pixelAspectDenominator"),
                          preset.pixelAspect.denominator());
        const auto rate = preset.frameRate.value_or(document::FrameRate::framesPerSecond24());
        settings.setValue(QStringLiteral("frameRateNumerator"), rate.numerator());
        settings.setValue(QStringLiteral("frameRateDenominator"), rate.denominator());
        writeTime(settings, QStringLiteral("duration"),
                  preset.duration.value_or(core::RationalTime::fromInteger(10)));
        const auto timecode = preset.timecode.value_or(document::CompositionTimecode{});
        writeTime(settings, QStringLiteral("start"), timecode.start);
        settings.setValue(QStringLiteral("dropFrame"), timecode.dropFrame);
        const auto background = preset.background.value_or(core::Color4d{0.0, 0.0, 0.0, 1.0});
        settings.setValue(
            QStringLiteral("background"),
            QVariantList{background.red, background.green, background.blue, background.alpha});
    }
    settings.endArray();
}

CompositionPreset userPresetFrom(const QString& name,
                                 const commands::CompositionSettings& settings) {
    CompositionPreset preset;
    preset.name = name.trimmed();
    preset.width = settings.format.width();
    preset.height = settings.format.height();
    preset.pixelAspect = settings.format.pixelAspect();
    preset.frameRate = settings.format.frameRate();
    preset.duration = settings.duration;
    preset.timecode = settings.timecode;
    preset.background = settings.background;
    return preset;
}

commands::CompositionSettings applyCompositionPreset(const CompositionPreset& preset,
                                                     commands::CompositionSettings settings) {
    const auto format =
        document::CompositionFormat::create(preset.width, preset.height, preset.pixelAspect,
                                            preset.frameRate.value_or(settings.format.frameRate()));
    if (format)
        settings.format = *format;
    if (preset.duration)
        settings.duration = *preset.duration;
    if (preset.timecode)
        settings.timecode = *preset.timecode;
    if (preset.background)
        settings.background = *preset.background;
    return settings;
}

bool compositionPresetMatches(const CompositionPreset& preset,
                              const commands::CompositionSettings& settings) {
    if (preset.width != settings.format.width() || preset.height != settings.format.height() ||
        preset.pixelAspect != settings.format.pixelAspect())
        return false;
    if (preset.builtIn)
        return true;
    return preset.frameRate == settings.format.frameRate() &&
           preset.duration == settings.duration && preset.timecode == settings.timecode &&
           preset.background == settings.background;
}

std::optional<std::size_t>
matchingCompositionPreset(const std::vector<CompositionPreset>& presets,
                          const commands::CompositionSettings& settings) {
    for (std::size_t index = 0; index < presets.size(); ++index)
        if (compositionPresetMatches(presets[index], settings))
            return index;
    return std::nullopt;
}

std::optional<QString> userPresetNameIssue(const QString& name,
                                           const std::vector<CompositionPreset>& presets,
                                           const std::optional<std::size_t> ignoring) {
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("Enter a preset name.");
    if (trimmed.toUtf8().size() > kMaximumNameBytes)
        return QStringLiteral("A preset name is at most 128 UTF-8 bytes.");
    for (std::size_t index = 0; index < presets.size(); ++index)
        if (index != ignoring && presets[index].name.compare(trimmed, Qt::CaseInsensitive) == 0)
            return QStringLiteral("A preset with this name already exists.");
    return std::nullopt;
}

} // namespace bloom::ui
