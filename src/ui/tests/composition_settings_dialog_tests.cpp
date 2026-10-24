// #376 Composition Settings: the shared New Composition / Composition Settings modal, its presets,
// the timecode labels it authors, and the one command each mode commits.
//
// With BLOOM_SCREENSHOT_DIR set, the test also writes review screenshots of create mode, settings
// mode, the open preset dropdown, the open Advanced section and an inline validation error.

#include "composition_settings_dialog.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_presets.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>

namespace {

using namespace bloom;
using ui::CompositionSettingsDialog;

int failures = 0;
void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.line() << ": " << message << '\n';
    }
}

[[nodiscard]] document::FrameRate rate(const std::uint32_t numerator,
                                       const std::uint32_t denominator = 1) {
    return document::FrameRate::create(numerator, denominator)
        .value_or(document::FrameRate::framesPerSecond24());
}

[[nodiscard]] commands::CompositionSettings baseSettings() {
    commands::CompositionSettings settings;
    settings.name = "Main";
    settings.format =
        document::CompositionFormat::create(1920, 1080, core::PixelAspectRatio::square(), rate(24))
            .value_or(document::CompositionFormat{});
    settings.duration = core::RationalTime::fromInteger(10);
    return settings;
}

[[nodiscard]] std::vector<ui::WorkingColorSpaceChoice> spaces() {
    return {{QStringLiteral("Inherit"), QString{}},
            {QStringLiteral("Linear Rec.709 (sRGB)"), QStringLiteral("Linear Rec.709 (sRGB)")}};
}

template <typename T> T* child(QWidget& parent, const char* name) {
    return parent.findChild<T*>(QString::fromLatin1(name));
}

[[nodiscard]] int findText(const ui::kit::KDropdown& dropdown, const QString& text) {
    for (int index = 0; index < dropdown.count(); ++index)
        if (dropdown.itemText(index) == text)
            return index;
    return -1;
}

void settle(const int milliseconds = 0) {
    QElapsedTimer timer;
    timer.start();
    do
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    while (timer.elapsed() < milliseconds);
}

void testTimecodeLabels() {
    const auto ntsc = rate(30000, 1001);
    const document::CompositionTimecode drop{core::RationalTime{}, true};
    expect(ui::formatTimelineFrameLabel(1800, ntsc, true, drop) == QStringLiteral("00:01:00;02"),
           "drop-frame skips ;00 and ;01 at the first minute");
    expect(ui::formatTimelineFrameLabel(17982, ntsc, true, drop) == QStringLiteral("00:10:00;00"),
           "drop-frame keeps ;00 at the tenth minute");
    expect(ui::formatTimelineFrameLabel(1800, ntsc, true) == QStringLiteral("00:01:00:00"),
           "non-drop labels at the nominal 30");
    expect(ui::parseTimelineFrameLabel(QStringLiteral("00:01:00;02"), ntsc, true, drop) == 1800U &&
               ui::parseTimelineFrameLabel(QStringLiteral("00:10:00;00"), ntsc, true, drop) ==
                   17982U,
           "drop-frame labels parse back to their frames");
    expect(!ui::parseTimelineFrameLabel(QStringLiteral("00:01:00;01"), ntsc, true, drop),
           "a dropped label names no frame");
    expect(ui::formatTimelineFrameLabel(3600, rate(60000, 1001), true, drop) ==
               QStringLiteral("00:01:00;04"),
           "59.94 drop-frame skips four labels");
    const document::CompositionTimecode hour{core::RationalTime::fromInteger(3600), false};
    expect(ui::formatTimelineFrameLabel(0, rate(25), true, hour) == QStringLiteral("01:00:00:00") &&
               ui::formatTimelineFrameLabel(0, rate(25), false, hour) == QStringLiteral("90000"),
           "the start timecode labels frame zero in both formats");
    expect(ui::parseTimelineFrameLabel(QStringLiteral("01:00:01:00"), rate(25), true, hour) ==
                   25U &&
               !ui::parseTimelineFrameLabel(QStringLiteral("00:59:59:00"), rate(25), true, hour),
           "parsing subtracts the start and refuses a label before it");
    const document::CompositionTimecode ignored{core::RationalTime{}, true};
    expect(ui::formatTimelineFrameLabel(1800, rate(30), true, ignored) ==
               QStringLiteral("00:01:00:00"),
           "drop-frame is ignored at a non-NTSC rate");
}

void testPresets() {
    const auto builtIns = ui::builtInCompositionPresets();
    expect(builtIns.size() == 7 && builtIns.front().name == QStringLiteral("HD 1080p"),
           "seven built-in presets, HD 1080p first");
    auto settings = baseSettings();
    expect(ui::matchingCompositionPreset(builtIns, settings) == 0U,
           "1920 x 1080 matches HD 1080p at any rate");
    settings.format =
        document::CompositionFormat::create(1080, 1920, core::PixelAspectRatio::square(), rate(30))
            .value_or(settings.format);
    expect(ui::matchingCompositionPreset(builtIns, settings) == 5U, "vertical 1080 x 1920 matches");
    settings.format = document::CompositionFormat::create(1000, 1000).value_or(settings.format);
    expect(!ui::matchingCompositionPreset(builtIns, settings), "an odd size is Custom");

    QTemporaryDir directory;
    QSettings store(directory.filePath(QStringLiteral("presets.ini")), QSettings::IniFormat);
    auto custom = baseSettings();
    custom.timecode = {core::RationalTime::fromInteger(10), true};
    custom.format = document::CompositionFormat::create(
                        1920, 1080, core::PixelAspectRatio::square(), rate(30000, 1001))
                        .value_or(custom.format);
    std::vector<ui::CompositionPreset> user{
        ui::userPresetFrom(QStringLiteral(" Broadcast "), custom)};
    ui::saveUserCompositionPresets(store, user);
    store.sync();
    const auto loaded = ui::loadUserCompositionPresets(store);
    expect(loaded.size() == 1 && loaded.front() == user.front() &&
               loaded.front().name == QStringLiteral("Broadcast"),
           "a user preset saves and loads exactly, name trimmed");
    expect(ui::compositionPresetMatches(loaded.front(), custom), "it matches its own settings");
    auto other = custom;
    other.duration = core::RationalTime::fromInteger(11);
    expect(!ui::compositionPresetMatches(loaded.front(), other) &&
               ui::compositionPresetMatches(builtIns.front(), other),
           "a user preset matches only all its values; the built-in still matches the size");
    expect(ui::userPresetNameIssue(QStringLiteral("broadcast"), loaded).has_value() &&
               ui::userPresetNameIssue(QStringLiteral("  "), loaded).has_value() &&
               !ui::userPresetNameIssue(QStringLiteral("broadcast"), loaded, 0U).has_value(),
           "duplicate and empty names are refused; renaming to itself is not");
    store.setValue(QStringLiteral("compositionPresets/2/name"), QStringLiteral("Broken"));
    store.setValue(QStringLiteral("compositionPresets/size"), 2);
    expect(ui::loadUserCompositionPresets(store).size() == 1, "an unreadable entry is skipped");
}

void testDialogPresetsAndValidation() {
    CompositionSettingsDialog dialog(CompositionSettingsDialog::Mode::Create, baseSettings(),
                                     spaces());
    auto* preset = child<ui::kit::KDropdown>(dialog, "compositionSettingsPreset");
    auto* width = child<ui::kit::KValueField>(dialog, "assetsWidthField");
    auto* height = child<ui::kit::KValueField>(dialog, "assetsHeightField");
    auto* ok = child<QPushButton>(dialog, "assetsDialogOk");
    auto* error = child<QLabel>(dialog, "newCompositionErrorLabel");
    expect(preset && width && height && ok && error, "the dialog controls exist");
    if (!preset || !width || !height || !ok || !error)
        return;
    expect(preset->currentText() == QStringLiteral("HD 1080p"), "the preset shows HD 1080p");
    preset->setCurrentIndex(findText(*preset, QStringLiteral("UHD 4K")));
    expect(width->value() == 3840 && height->value() == 2160 && dialog.settings() &&
               dialog.settings()->format.frameRate() == rate(24),
           "choosing UHD 4K sets the size and keeps the rate");
    width->setValue(3000);
    expect(preset->currentText() == QStringLiteral("Custom"), "an edited size shows Custom");
    auto* lock = child<QAbstractButton>(dialog, "compositionSettingsLockAspect");
    width->setValue(1920);
    height->setValue(1080);
    lock->setChecked(true);
    width->setValue(1280);
    expect(height->value() == 720, "the aspect lock keeps 16:9");
    lock->setChecked(false);

    auto* name = child<QLineEdit>(dialog, "assetsNameField");
    name->setText(QString{});
    expect(!ok->isEnabled() && !error->isHidden() && error->text().contains(QStringLiteral("name")),
           "an empty name is refused inline");
    name->setText(QStringLiteral("Main"));
    auto* rateField = child<QLineEdit>(dialog, "assetsFrameRateField");
    rateField->setText(QStringLiteral("abc"));
    expect(!ok->isEnabled() && error->text().contains(QStringLiteral("frame rate")),
           "an invalid frame rate is refused inline");
    rateField->setText(QStringLiteral("30000/1001"));
    auto* drop = child<QAbstractButton>(dialog, "compositionSettingsDropFrame");
    expect(drop && drop->isEnabled(), "drop-frame is offered at 29.97");
    drop->setChecked(true);
    auto* unit = child<ui::kit::KDropdown>(dialog, "assetsDurationUnitDropdown");
    unit->setCurrentIndex(2);
    auto* duration = child<QLineEdit>(dialog, "assetsDurationField");
    duration->setText(QStringLiteral("00:01:00;02"));
    auto* start = child<QLineEdit>(dialog, "compositionSettingsStart");
    start->setText(QStringLiteral("01:00:00;00"));
    const auto authored = dialog.settings();
    expect(authored && authored->timecode.dropFrame &&
               authored->duration == *core::RationalTime::create(1800 * 1001, 30000),
           "a drop-frame timecode duration is exactly 1800 frames");
    expect(authored &&
               authored->timecode.start == *core::RationalTime::create(107892 * 1001, 30000),
           "a drop-frame start of 01:00:00;00 is frame 107892");
    rateField->setText(QStringLiteral("25"));
    expect(drop->isEnabled() == false && dialog.settings() &&
               !dialog.settings()->timecode.dropFrame,
           "at 25 fps drop-frame is unavailable and not authored");
    auto* titleSafe = child<ui::kit::KValueField>(dialog, "compositionSettingsTitleSafe");
    titleSafe->setValue(95);
    expect(!ok->isEnabled() && error->text().contains(QStringLiteral("Title safe")),
           "title safe outside action safe is refused inline");
    titleSafe->setValue(80);
    expect(ok->isEnabled(), "valid settings enable Add");

    // Custom presets: save, match, rename, delete.
    expect(dialog.savePreset(QStringLiteral("My 25")) == std::nullopt &&
               preset->currentText() == QStringLiteral("My 25"),
           "a saved preset appears and matches the current values");
    expect(dialog.savePreset(QStringLiteral("my 25")).has_value(), "a duplicate name is refused");
    QSettings store;
    expect(ui::loadUserCompositionPresets(store).size() == 1, "the preset is a user setting");
    expect(dialog.renameSelectedPreset(QStringLiteral("PAL")) == std::nullopt &&
               preset->currentText() == QStringLiteral("PAL") &&
               ui::loadUserCompositionPresets(store).front().name == QStringLiteral("PAL"),
           "the preset renames and persists");
    expect(dialog.deleteSelectedPreset() == std::nullopt && dialog.userPresets().empty() &&
               ui::loadUserCompositionPresets(store).empty(),
           "the preset deletes and persists");
}

void testCreateAndSettingsCommands() {
    document::Document document(
        document::makeNewProject(
            "Project", "Main", core::RationalTime::fromInteger(10),
            document::CompositionFormat::create(1000, 500).value_or(document::CompositionFormat{}))
            .project);
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack,
                                   document.snapshot().project().compositions().front().id());
    const auto composition = session.compositionId();

    QTimer::singleShot(0, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = dynamic_cast<CompositionSettingsDialog*>(widget);
                dialog && dialog->isVisible()) {
                child<QLineEdit>(*dialog, "assetsNameField")->setText(QStringLiteral("Square"));
                auto* preset = child<ui::kit::KDropdown>(*dialog, "compositionSettingsPreset");
                preset->setCurrentIndex(findText(*preset, QStringLiteral("Square 1080")));
                child<QPushButton>(*dialog, "assetsDialogOk")->click();
            }
    });
    const auto added = ui::showNewCompositionDialog(session, nullptr);
    const auto* square = added ? document.snapshot().project().findComposition(*added) : nullptr;
    expect(square && square->name() == "Square" && square->format().width() == 1080 &&
               square->format().height() == 1080,
           "New Composition adds one composition from a preset");

    const auto layer = [&] {
        commands::Transaction transaction("Solid", document.snapshot().revision());
        transaction.emplace<commands::AddSolidLayer>(composition, "Solid",
                                                     core::Color4d{1, 1, 1, 1});
        return stack.execute(std::move(transaction))
            .outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput);
    }();
    const auto positionOf = [&] {
        const auto* record =
            document.snapshot().project().findComposition(composition)->parameters().find(*layer);
        return std::get<document::Vec2d>(
            std::get<document::ConstantValueSource>(record->source).value);
    };
    const auto before = positionOf();
    const auto history = stack.size();
    QTimer::singleShot(0, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = dynamic_cast<CompositionSettingsDialog*>(widget);
                dialog && dialog->isVisible()) {
                child<ui::kit::KValueField>(*dialog, "assetsWidthField")->setValue(1200);
                child<QAbstractButton>(*dialog, "compositionSettingsAnchor0")->setChecked(true);
                child<QLineEdit>(*dialog, "assetsFrameRateField")->setText(QStringLiteral("30"));
                child<QPushButton>(*dialog, "assetsDialogOk")->click();
            }
    });
    expect(ui::showCompositionSettingsDialog(session, composition, nullptr),
           "Composition Settings commits");
    const auto* edited = document.snapshot().project().findComposition(composition);
    expect(edited->format().width() == 1200 && edited->format().frameRate() == rate(30) &&
               stack.size() == history + 1 && positionOf().x == before.x,
           "one command: width, rate, and a top-left anchor keeps the layer in place");
    expect(session.undo() &&
               document.snapshot().project().findComposition(composition)->format().width() == 1000,
           "one undo restores the settings");
}

// Grabs the dialog and any open dropdown popup into one review image.
void screenshot(QDialog& dialog, const QString& path, ui::kit::KDropdown* open = nullptr) {
    settle(400);
    QImage image = dialog.grab().toImage();
    // A headless Weston has no input seat, so it dismisses an xdg_popup at once; the popup widget
    // is laid out by showPopup() all the same and is rendered here at its placed geometry.
    if (open != nullptr && open->popup() != nullptr) {
        const auto popup = open->popup()->grab().toImage();
        // The popup sits under the field, offset by its shadow gutter; keep that offset.
        const auto below = QPoint(0, open->height());
        const auto at =
            open->mapTo(&dialog, below) + (open->popup()->pos() - open->mapToGlobal(below));
        QImage combined(std::max(image.width(), at.x() + popup.width()),
                        std::max(image.height(), at.y() + popup.height()), QImage::Format_ARGB32);
        combined.fill(Qt::transparent);
        QPainter painter(&combined);
        painter.drawImage(0, 0, image);
        painter.drawImage(at, popup);
        painter.end();
        image = combined;
    }
    expect(image.save(path), "screenshot written: " + path.toStdString());
}

void writeScreenshots(const QString& directory) {
    QDir().mkpath(directory);
    auto settings = baseSettings();
    settings.name = "Main";
    {
        CompositionSettingsDialog dialog(CompositionSettingsDialog::Mode::Create, settings,
                                         spaces());
        dialog.show();
        screenshot(dialog, directory + QStringLiteral("/composition-create.png"));
        auto* preset = child<ui::kit::KDropdown>(dialog, "compositionSettingsPreset");
        preset->showPopup();
        settle(400);
        expect(preset->popupView()->model()->rowCount() == preset->count(),
               "the preset dropdown lists every preset");
        screenshot(dialog, directory + QStringLiteral("/composition-preset-dropdown.png"), preset);
        preset->hidePopup();
    }
    {
        auto edit = settings;
        edit.timecode = {core::RationalTime::fromInteger(3600), true};
        edit.format = document::CompositionFormat::create(
                          3840, 2160, core::PixelAspectRatio::square(), rate(30000, 1001))
                          .value_or(edit.format);
        CompositionSettingsDialog dialog(CompositionSettingsDialog::Mode::Edit, edit, spaces());
        dialog.show();
        child<ui::kit::KDropdown>(dialog, "assetsDurationUnitDropdown")->setCurrentIndex(2);
        screenshot(dialog, directory + QStringLiteral("/composition-settings.png"));
        child<ui::kit::KSection>(dialog, "compositionSettingsAdvanced")->setCollapsed(false);
        screenshot(dialog, directory + QStringLiteral("/composition-advanced.png"));
        child<ui::kit::KSection>(dialog, "compositionSettingsAdvanced")->setCollapsed(true);
        child<QLineEdit>(dialog, "assetsFrameRateField")->setText(QStringLiteral("0"));
        screenshot(dialog, directory + QStringLiteral("/composition-validation-error.png"));
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto screenshots = qEnvironmentVariable("BLOOM_SCREENSHOT_DIR");
    QApplication application(argc, argv);
    ui::kit::installKinetikTheme(application);
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QApplication::setOrganizationName(QStringLiteral("BloomCompositionSettingsTests"));
    QApplication::setApplicationName(QStringLiteral("BloomCompositionSettingsTests"));

    testTimecodeLabels();
    testPresets();
    testDialogPresetsAndValidation();
    testCreateAndSettingsCommands();
    if (!screenshots.isEmpty())
        writeScreenshots(screenshots);
    if (failures == 0)
        std::cout << "composition settings PASS\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
