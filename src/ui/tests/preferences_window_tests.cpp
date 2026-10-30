#include "preferences_window.hpp"

#include <bloom/ui/acceleration_status.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <source_location>
#include <string_view>

namespace {

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

// Every kit dialog needs an owner window; main() provides one.
QWidget* gOwner = nullptr;
[[nodiscard]] QWidget& owner() { return *gOwner; }

[[nodiscard]] ui::ApplicationPreferences samplePreferences() {
    ui::ApplicationPreferences prefs;
    prefs.audioEnabled = false;
    prefs.loopPlayback = false;
    prefs.ramHeadroomBytes = 6ULL * 1024 * 1024 * 1024;
    prefs.vramHeadroomBytes = 512ULL * 1024 * 1024;
    prefs.mediaDiskCacheEnabled = false;
    prefs.timelineTimeFormat = ui::TimelineTimeFormat::Timecode;
    prefs.timelineSnapping = false;
    prefs.timelineGraphEditor = true;
    prefs.timelineLayerColumnWidth = 320;
    prefs.nodeLinkStyle = ui::NodeLinkStyle::Angled;
    prefs.nodeSnap = true;
    prefs.nodeGridSize = 32.0;
    prefs.viewerResolution = ui::ViewerResolutionPreference::Half;
    prefs.viewerBackground = ui::ViewerBackgroundPreference::Grey;
    prefs.viewerTransparencyGrid = true;
    prefs.viewerThirds = true;
    return prefs;
}

void testControlsReflectPreferences(Expectations& check) {
    ui::PreferencesWindow window(owner(), samplePreferences());
    const auto prefs = samplePreferences();

    auto* audio =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesAudioEnabledSwitch"));
    check.expect(audio != nullptr && !audio->isChecked(),
                 "the audio switch reflects the stored value");
    auto* loop =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesLoopPlaybackSwitch"));
    check.expect(loop != nullptr && !loop->isChecked(),
                 "the loop switch reflects the stored value");

    auto* ramHeadroom =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesRamHeadroomField"));
    check.expect(ramHeadroom != nullptr && ramHeadroom->value() == 6.0,
                 "the RAM headroom is shown in GiB");
    auto* vramHeadroom =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesVramHeadroomField"));
    check.expect(vramHeadroom != nullptr && vramHeadroom->value() == 0.5,
                 "the VRAM headroom is shown in GiB");

    auto* format = window.findChild<ui::kit::KDropdown*>(
        QStringLiteral("preferencesTimelineTimeFormatDropdown"));
    check.expect(format != nullptr &&
                     format->currentData().toString() == QStringLiteral("timecode"),
                 "the time format dropdown reflects the stored value");
    auto* link =
        window.findChild<ui::kit::KDropdown*>(QStringLiteral("preferencesNodeLinkStyleDropdown"));
    check.expect(link != nullptr && link->currentData().toString() == QStringLiteral("angled"),
                 "the link style dropdown reflects the stored value");
    auto* resolution = window.findChild<ui::kit::KDropdown*>(
        QStringLiteral("preferencesViewerResolutionDropdown"));
    check.expect(resolution != nullptr &&
                     resolution->currentData().toString() == QStringLiteral("Half"),
                 "the resolution dropdown reflects the stored value");
    auto* background = window.findChild<ui::kit::KDropdown*>(
        QStringLiteral("preferencesViewerBackgroundDropdown"));
    check.expect(background != nullptr &&
                     background->currentData().toString() == QStringLiteral("Grey") &&
                     background->findData(QStringLiteral("Checkerboard")) < 0,
                 "the surround dropdown reflects the stored value and offers no checker");
    auto* transparencyGrid = window.findChild<QAbstractButton*>(
        QStringLiteral("preferencesViewerTransparencyGridSwitch"));
    check.expect(transparencyGrid != nullptr && transparencyGrid->isChecked(),
                 "the Transparency grid switch reflects the stored value");

    auto* grid =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesNodeGridSizeField"));
    check.expect(grid != nullptr && grid->value() == 32.0,
                 "the raw-valued grid size field reflects the stored value");

    check.expect(window.preferences() == prefs, "an unedited window reports exactly its input");
}

void testEditingUpdatesDraftAndApplyState(Expectations& check) {
    ui::ApplicationPreferences initial;
    ui::PreferencesWindow window(owner(), initial);
    auto* apply = window.findChild<QAbstractButton*>(QStringLiteral("preferencesApplyButton"));
    check.expect(apply != nullptr && !apply->isEnabled(), "Apply is disabled with no edits");

    auto* audio =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesAudioEnabledSwitch"));
    audio->setChecked(false);
    check.expect(!window.preferences().audioEnabled, "toggling a switch updates the draft");
    check.expect(apply->isEnabled(), "an edit enables Apply");

    auto* format = window.findChild<ui::kit::KDropdown*>(
        QStringLiteral("preferencesTimelineTimeFormatDropdown"));
    format->setCurrentIndex(format->findData(QStringLiteral("timecode")));
    check.expect(window.preferences().timelineTimeFormat == ui::TimelineTimeFormat::Timecode,
                 "choosing Timecode updates the draft");

    auto* grid =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesNodeGridSizeField"));
    grid->setValue(48.0);
    check.expect(window.preferences().nodeGridSize == 48.0, "editing a field updates the draft");

    auto* headroom =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesRamHeadroomField"));
    headroom->setValue(8.0);
    check.expect(window.preferences().ramHeadroomBytes == 8ULL * 1024 * 1024 * 1024,
                 "a GiB headroom edit round-trips to the exact byte count");
}

void testApplyEmitsAndClearsDirty(Expectations& check) {
    ui::ApplicationPreferences initial;
    ui::PreferencesWindow window(owner(), initial);
    auto* apply = window.findChild<QAbstractButton*>(QStringLiteral("preferencesApplyButton"));

    int emissions = 0;
    ui::ApplicationPreferences emitted;
    QObject::connect(&window, &ui::PreferencesWindow::preferencesApplied, &window,
                     [&](const ui::ApplicationPreferences& prefs) {
                         ++emissions;
                         emitted = prefs;
                     });

    auto* snap = window.findChild<QAbstractButton*>(QStringLiteral("preferencesNodeSnapSwitch"));
    snap->setChecked(true);
    check.expect(apply->isEnabled(), "the edit leaves Apply enabled before it is clicked");

    apply->click();
    check.expect(emissions == 1, "Apply emits preferencesApplied exactly once");
    check.expect(emitted.nodeSnap, "the emitted value carries the edit");
    check.expect(!apply->isEnabled(), "a successful Apply clears the dirty state");
}

void testSetPreferencesRoundTripsControls(Expectations& check) {
    ui::PreferencesWindow window(owner(), ui::ApplicationPreferences{});
    const auto prefs = samplePreferences();
    window.setPreferences(prefs);

    auto* audio =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesAudioEnabledSwitch"));
    check.expect(audio != nullptr && !audio->isChecked(),
                 "setPreferences pushes the value into the control");

    auto* apply = window.findChild<QAbstractButton*>(QStringLiteral("preferencesApplyButton"));
    check.expect(apply != nullptr && apply->isEnabled(),
                 "pushing a value makes the draft differ from the applied baseline");
    check.expect(window.preferences() == prefs, "setPreferences round-trips through preferences()");
}

// Restore Defaults asks first (Cancel is the default answer); only the confirmation resets the
// draft, and nothing is persisted until OK or Apply.
void testRestoreDefaults(Expectations& check) {
    ui::PreferencesWindow window(owner(), samplePreferences());
    // Restore Defaults lives in the sidebar's action menu, as Blender's Load Factory does.
    auto* restore = window.findChild<QAction*>(QStringLiteral("preferencesRestoreDefaultsAction"));
    check.expect(restore != nullptr && window.findChild<QAbstractButton*>(QStringLiteral(
                                           "preferencesRestoreDefaultsButton")) == nullptr,
                 "Restore Defaults is a sidebar action, not a button-row button");
    if (restore == nullptr)
        return;
    const auto confirmation = [&window] {
        return window.findChild<ui::kit::KDialog*>(
            QStringLiteral("preferencesRestoreDefaultsConfirmation"));
    };
    restore->trigger();
    auto* asked = confirmation();
    check.expect(asked != nullptr && asked->isVisible() &&
                     asked->defaultRole() == ui::kit::KDialog::Role::Reject,
                 "Restore Defaults asks first, with Cancel as the default");
    check.expect(window.preferences() == samplePreferences(), "asking changes nothing yet");
    if (asked == nullptr)
        return;
    QTest::keyClick(asked, Qt::Key_Return);
    QApplication::processEvents();
    check.expect(window.preferences() == samplePreferences(),
                 "Return answers the safe default and keeps the preferences");
    restore->trigger();
    asked = confirmation();
    if (auto* confirm =
            asked != nullptr ? asked->button(ui::kit::KDialog::Role::Destructive) : nullptr)
        confirm->click();
    QApplication::processEvents();
    check.expect(window.preferences() == ui::defaultApplicationPreferences(),
                 "confirming Restore Defaults resets the draft to the model defaults");
}

// A cache directory that is a file is refused under its field once it is chosen or left, and OK
// waits; a usable folder clears the reason.
void testCacheDirectoryIsValidated(Expectations& check) {
    QTemporaryDir directory;
    QFile file(directory.filePath(QStringLiteral("not-a-folder")));
    check.expect(file.open(QIODevice::WriteOnly), "a file to refuse");
    file.close();
    ui::PreferencesWindow window(owner(), samplePreferences());
    auto* edit =
        window.findChild<QLineEdit*>(QStringLiteral("preferencesMediaDiskCacheDirectoryEdit"));
    auto* reason =
        window.findChild<QLabel*>(QStringLiteral("preferencesMediaDiskCacheDirectoryReason"));
    auto* ok = window.findChild<QAbstractButton*>(QStringLiteral("preferencesOkButton"));
    check.expect(edit && reason && ok, "the cache directory row has its reason and OK exists");
    if (!edit || !reason || !ok)
        return;
    edit->setText(file.fileName());
    emit edit->editingFinished();
    check.expect(!reason->isHidden() && !reason->text().isEmpty() && !ok->isEnabled(),
                 "a file is refused as the cache folder and OK waits");
    edit->setText(directory.path());
    emit edit->editingFinished();
    check.expect(reason->isHidden() && ok->isEnabled(), "a writable folder is accepted");
}

void testTimelineKeyframesAndGraphAreMutuallyExclusive(Expectations& check) {
    ui::PreferencesWindow window(owner(), ui::defaultApplicationPreferences());
    auto* keyframes =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesTimelineKeyframesSwitch"));
    auto* graph =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesTimelineGraphEditorSwitch"));
    check.expect(keyframes != nullptr && graph != nullptr, "both timeline toggles exist");
    if (keyframes == nullptr || graph == nullptr) {
        return;
    }
    graph->setChecked(true);
    check.expect(!keyframes->isChecked(),
                 "enabling the graph editor clears Show keyframes in the window");
    check.expect(!window.preferences().timelineKeyframesVisible,
                 "the draft matches the editor's exclusive rule");
    keyframes->setChecked(true);
    check.expect(!graph->isChecked(), "enabling keyframes clears the graph editor");
}

void testRestartBadgesAndRail(Expectations& check) {
    ui::PreferencesWindow window(owner(), samplePreferences());
    // The disk cache section is read at startup: one Restart badge, in its header.
    const auto badges = window.findChildren<QLabel*>(QStringLiteral("preferencesRestartBadge"));
    const auto* disk = window.findChild<QWidget*>(QStringLiteral("preferencesMediaCacheSection"));
    check.expect(badges.size() == 1 && disk != nullptr && disk->isAncestorOf(badges.front()),
                 "the disk cache section carries the one Restart badge");
    check.expect(window.findChild<QLabel*>(QStringLiteral("preferencesRestartNotice")) == nullptr,
                 "no page intro paragraph");

    auto* rail = window.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail"));
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("preferencesPages"));
    if (rail == nullptr || pages == nullptr) {
        check.expect(false, "the sidebar and the pages exist");
        return;
    }
    QStringList categories;
    int gaps = 0;
    for (int row = 0; row < rail->count(); ++row) {
        if (rail->item(row)->flags() == Qt::NoItemFlags)
            ++gaps;
        else
            categories << rail->item(row)->text();
    }
    check.expect(categories == QStringList{"General", "Sounds", "Viewer", "Timeline",
                                           "Node Graph", "Memory & Caches", "Performance"} &&
                     gaps == 2,
                 "the sidebar lists the seven categories in three groups");
    check.expect(pages->count() == 7, "the stack carries the seven pages");
    const auto pageOf = [pages](const char* name) {
        return pages->indexOf(pages->findChild<QWidget*>(QLatin1StringView(name)));
    };
    rail->setCurrentRow(4);
    check.expect(pages->currentIndex() == pageOf("preferencesTimelinePage"),
                 "selecting a category switches the page");
    // A sidebar list, not radio dots: the keyboard moves through it like any list, over the gaps.
    window.show();
    rail->setFocus();
    QTest::keyClick(rail, Qt::Key_Down);
    check.expect(pages->currentIndex() == pageOf("preferencesNodeGraphPage"),
                 "Down moves to the next category");
    QTest::keyClick(rail, Qt::Key_Down);
    check.expect(pages->currentIndex() == pageOf("preferencesMemoryPage"),
                 "Down steps over a group gap");
    QTest::keyClick(rail, Qt::Key_Home);
    check.expect(pages->currentIndex() == pageOf("preferencesGeneralPage"),
                 "Home moves to the first category");
    QTest::keyClick(rail, Qt::Key_End);
    check.expect(pages->currentIndex() == pageOf("preferencesPerformancePage"),
                 "End moves to the last category");
    QTest::keyClicks(rail, QStringLiteral("Ti"));
    check.expect(pages->currentIndex() == pageOf("preferencesTimelinePage"),
                 "typing a name finds its category");
    check.expect(rail->item(0)->sizeHint().height() == ui::kit::px(ui::kit::Size::ListRow),
                 "rail rows are at the list pitch");
}

void testSoundsPage(Expectations& check) {
    ui::PreferencesWindow window(owner(), ui::ApplicationPreferences{});
    const auto find = [&](const char* name) {
        return window.findChild<QWidget*>(QLatin1StringView(name));
    };
    auto* master = qobject_cast<ui::kit::KSwitch*>(find("preferencesSoundsEnabledSwitch"));
    auto* events = qobject_cast<ui::kit::KSwitch*>(find("preferencesEventSoundsSwitch"));
    auto* interaction = qobject_cast<ui::kit::KSwitch*>(find("preferencesInteractionSoundsSwitch"));
    auto* notify = qobject_cast<ui::kit::KSwitch*>(find("preferencesRenderNotificationsSwitch"));
    auto* volume = qobject_cast<ui::kit::KSlider*>(find("preferencesSoundVolumeSlider"));
    auto* test = qobject_cast<ui::kit::KButton*>(find("preferencesPlayTestSoundButton"));
    check.expect(master && events && interaction && notify && volume && test,
                 "the Sounds page has its master, category, notification, volume and test controls");
    if (!(master && events && interaction && notify && volume && test))
        return;
    check.expect(master->isChecked() && events->isChecked() && !interaction->isChecked() &&
                     notify->isChecked() && std::lround(volume->value()) == 40,
                 "the page shows the defaults: events on, interaction off, volume 40");

    interaction->setChecked(true);
    volume->setValue(65.0);
    check.expect(window.preferences().interactionSoundsEnabled &&
                     window.preferences().soundVolumePercent == 65,
                 "editing the page updates the draft");

    int testRequests = 0;
    int requestedVolume = 0;
    QObject::connect(&window, &ui::PreferencesWindow::soundTestRequested, &window,
                     [&](const ui::ApplicationPreferences& draft) {
                         ++testRequests;
                         requestedVolume = draft.soundVolumePercent;
                     });
    test->click();
    check.expect(testRequests == 1 && requestedVolume == 65,
                 "Play test sound hands the unsaved draft to the caller");

    master->setChecked(false);
    check.expect(!volume->isEnabled() && !events->isEnabled() && !interaction->isEnabled(),
                 "turning sounds off disables the volume and category rows");
    check.expect(!window.preferences().soundsEnabled, "the master switch edits the draft");
}

void testHeadroomFields(Expectations& check) {
    constexpr auto gib = std::uint64_t{1024} * 1024 * 1024;
    ui::PreferencesWindow window(owner(), ui::ApplicationPreferences{});
    auto* ram =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesRamHeadroomField"));
    auto* vram =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesVramHeadroomField"));
    const auto* hint = window.findChild<QLabel*>(QStringLiteral("preferencesMemoryHint"));
    check.expect(ram && vram && ram->value() == 2.0 && vram->value() == 2.0 &&
                     ram->minimum() == 0.0 && ram->maximum() == 256.0,
                 "the Memory page shows RAM and VRAM headroom, 0 to 256 GiB, defaulting to 2");
    check.expect(hint && hint->text() == QStringLiteral(
                             "Headroom is memory Bloom leaves free for the rest of your computer. "
                             "Bloom uses everything else for rendering and caching."),
                 "the Memory page explains headroom in one helper line");
    check.expect(window.findChild<ui::kit::KValueField*>(
                     QStringLiteral("preferencesOperationCacheBytesField")) == nullptr &&
                     window.findChild<ui::kit::KValueField*>(
                         QStringLiteral("preferencesRamPreviewBytesField")) == nullptr,
                 "the old cache budget fields are gone");
    ram->setValue(0.0);
    vram->setValue(1000.0);
    check.expect(window.preferences().ramHeadroomBytes == 0 &&
                     window.preferences().vramHeadroomBytes == 256 * gib,
                 "zero is a real headroom and the field clamps to 256 GiB");
    auto* memory =
        window.findChild<ui::kit::KSection*>(QStringLiteral("preferencesMemorySection"));
    if (memory != nullptr)
        emit memory->resetRequested();
    check.expect(window.preferences().ramHeadroomBytes == 2 * gib &&
                     window.preferences().vramHeadroomBytes == 2 * gib && ram->value() == 2.0 &&
                     vram->value() == 2.0,
                 "Restore Defaults on the Memory section restores 2 GiB");
}

void testLargeBudgetPreferencesRemainExactOnApply(Expectations& check) {
    constexpr auto gib = std::uint64_t{1024} * 1024 * 1024;
    auto preferences = samplePreferences();
    preferences.mediaDiskCacheBudgetBytes = 16384 * gib;
    ui::PreferencesWindow window(owner(), preferences);
    const auto* disk = window.findChild<ui::kit::KValueField*>(
        QStringLiteral("preferencesMediaDiskCacheBudgetField"));
    check.expect(disk && disk->value() == 16384, "the disk budget has no arbitrary GiB ceiling");
    ui::ApplicationPreferences applied;
    QObject::connect(&window, &ui::PreferencesWindow::preferencesApplied, &window,
                     [&](const auto& value) { applied = value; });
    window.findChild<QAbstractButton*>(QStringLiteral("preferencesAudioEnabledSwitch"))
        ->setChecked(true);
    window.findChild<QAbstractButton*>(QStringLiteral("preferencesApplyButton"))->click();
    check.expect(applied.mediaDiskCacheBudgetBytes == preferences.mediaDiskCacheBudgetBytes &&
                     applied.ramHeadroomBytes == preferences.ramHeadroomBytes,
                 "an unrelated Apply preserves large saved byte counts exactly");
}

void testPreferenceLabelsRemainReadable(Expectations& check) {
    ui::PreferencesWindow window(owner(), samplePreferences());
    window.show();
    // Every page: each row label is the Properties row label, right-aligned in the one label
    // column all pages share, and shows its whole text.
    auto* rail = window.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail"));
    int columnX = -1;
    for (int row = 0; rail != nullptr && row < rail->count(); ++row) {
        if (rail->item(row)->flags() == Qt::NoItemFlags)
            continue;
        rail->setCurrentRow(row);
        QCoreApplication::processEvents();
        for (const auto* propertyRow : window.findChildren<ui::kit::KPropertyRow*>()) {
            if (!propertyRow->isVisibleTo(&window))
                continue;
            if (const auto* label = propertyRow->findChild<QLabel*>(
                    QStringLiteral("propertiesRowLabel"), Qt::FindDirectChildrenOnly)) {
                const int x = label->mapTo(&window, QPoint()).x();
                columnX = columnX < 0 ? x : columnX;
                check.expect(x == columnX,
                             "every page's labels start at one x: " + label->text().toStdString());
            }
        }
        for (const auto* propertyRow : window.findChildren<ui::kit::KPropertyRow*>()) {
            if (!propertyRow->isVisibleTo(&window))
                continue;
            const auto* label = propertyRow->findChild<QLabel*>(
                QStringLiteral("propertiesRowLabel"), Qt::FindDirectChildrenOnly);
            check.expect(label != nullptr &&
                             label->width() == ui::kit::px(ui::kit::Size::PreferencesLabelWidth) &&
                             label->alignment().testFlag(Qt::AlignRight) &&
                             label->width() >=
                                 QFontMetrics(label->font()).horizontalAdvance(label->text()),
                         "preference labels share one readable, right-aligned column: " +
                             (label != nullptr ? label->text().toStdString() : std::string{}));
        }
    }
    auto* label = ui::kit::makePropertyRowLabel(QStringLiteral("Default property"), &window);
    ui::kit::KPropertyRow defaultRow(label, nullptr, {}, &window);
    defaultRow.resize(600, defaultRow.height());
    defaultRow.show();
    QCoreApplication::processEvents();
    check.expect(label->width() == ui::kit::px(ui::kit::Size::PropertiesLabelWidth),
                 "ordinary Properties rows retain their existing label width");
}

void testSectionResetRestoresItsDefaults(Expectations& check) {
    auto preferences = samplePreferences();
    preferences.nodeSnap = true;
    preferences.nodeGridSize = 32.0;
    preferences.timelineSnapping = false;
    ui::PreferencesWindow window(owner(), preferences);
    auto* nodes =
        window.findChild<ui::kit::KSection*>(QStringLiteral("preferencesNodeGraphSection"));
    check.expect(nodes != nullptr && nodes->isResetEnabled(), "the Node Graph section has a Reset");
    if (nodes != nullptr)
        emit nodes->resetRequested();
    const auto defaults = ui::defaultApplicationPreferences();
    check.expect(window.preferences().nodeSnap == defaults.nodeSnap &&
                     window.preferences().nodeGridSize == defaults.nodeGridSize &&
                     window.preferences().timelineSnapping == false,
                 "a section's Reset restores only that section's defaults");
    auto* performance =
        window.findChild<ui::kit::KSection*>(QStringLiteral("preferencesPerformanceSection"));
    check.expect(performance != nullptr && !performance->isResetEnabled(),
                 "the read-only Performance section offers no Reset");
}

void testWindowFitsItsContent(Expectations& check) {
    ui::PreferencesWindow window(owner(), samplePreferences());
    window.show();
    QCoreApplication::processEvents();
    // Opened, no page scrolls and no page is squeezed: the window is as tall as its tallest page.
    auto* rail = window.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail"));
    const auto size = window.size();
    for (int row = 0; rail != nullptr && row < rail->count(); ++row) {
        if (rail->item(row)->flags() == Qt::NoItemFlags)
            continue;
        rail->setCurrentRow(row);
        QCoreApplication::processEvents();
        for (auto* scroll : window.findChildren<QScrollArea*>()) {
            if (!scroll->isVisibleTo(&window) || scroll->widget() == nullptr)
                continue;
            check.expect(scroll->verticalScrollBar()->maximum() == 0 &&
                             scroll->horizontalScrollBar()->maximum() == 0,
                         "the opened window shows the whole page: " +
                             rail->item(row)->text().toStdString());
        }
        check.expect(window.size() == size, "switching pages never resizes the window");
    }
    check.expect(window.minimumWidth() < window.width(),
                 "the window can be made narrower than it opens");
}

void testGridAndAutomaticWidthMatchRuntime(Expectations& check) {
    auto preferences = samplePreferences();
    preferences.nodeGridSize = 1;
    preferences.timelineLayerColumnWidth = 0;
    ui::PreferencesWindow window(owner(), preferences);
    auto* grid =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesNodeGridSizeField"));
    auto* width = window.findChild<ui::kit::KValueField*>(
        QStringLiteral("preferencesTimelineLayerColumnWidthField"));
    check.expect(grid && grid->value() == 1,
                 "the minimum supported grid size is displayed exactly");
    auto* widthMode = window.findChild<ui::kit::KDropdown*>(
        QStringLiteral("preferencesTimelineLayerColumnWidthFieldMode"));
    check.expect(width && width->value() == 0 && widthMode != nullptr &&
                     widthMode->currentText() == QStringLiteral("Automatic") && width->isHidden(),
                 "automatic timeline width reads Automatic, not an authored 80 pixels");
    grid->setValue(4096);
    check.expect(window.preferences().nodeGridSize == 4096,
                 "grid size accepts values above the old arbitrary 200-pixel ceiling");
    width->setValue(4096);
    check.expect(window.preferences().timelineLayerColumnWidth == 4096 &&
                     widthMode->currentText() == QStringLiteral("Custom") && !width->isHidden(),
                 "timeline width preserves the supported integer value, as Custom");
    widthMode->chooseIndex(0);
    check.expect(window.preferences().timelineLayerColumnWidth == 0,
                 "choosing Automatic returns the width to zero");
    widthMode->chooseIndex(1);
    check.expect(window.preferences().timelineLayerColumnWidth == 4096,
                 "choosing Custom again restores the last custom value");
}

void testPerformancePageReportsCpuOnly(Expectations& check) {
    ui::PreferencesWindow window(owner(), ui::ApplicationPreferences{});
    auto* backend = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceBackendValue"));
    auto* state = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceStateValue"));
    auto* deviceRow = window.findChild<QWidget*>(QStringLiteral("preferencesPerformanceDeviceRow"));
    check.expect(backend != nullptr && backend->text() == QStringLiteral("CPU reference"),
                 "with no provider the Performance page reports the CPU reference");
    check.expect(state != nullptr && state->text() == QStringLiteral("Unavailable"),
                 "with no provider the device state reads Unavailable");
    check.expect(deviceRow != nullptr && deviceRow->isHidden(),
                 "the device row is hidden without a GPU device");
}

// A provider stand-in for the future capability-report-backed one.
class StubAccelerationStatus final : public ui::AccelerationStatusProvider {
  public:
    [[nodiscard]] ui::AccelerationStatus accelerationStatus() const override {
        ui::AccelerationStatus status;
        status.backend = QStringLiteral("Vulkan");
        status.deviceState = QStringLiteral("Ready");
        status.deviceName = QStringLiteral("Test GPU");
        status.driver = QStringLiteral("test-driver 1.0");
        status.previewRoute = QStringLiteral("Available for eligible previews");
        return status;
    }
};

void testPerformancePageUsesInjectedProvider(Expectations& check) {
    const StubAccelerationStatus provider;
    ui::PreferencesWindow window(owner(), ui::ApplicationPreferences{}, &provider);
    auto* backend = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceBackendValue"));
    auto* device = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceDeviceValue"));
    auto* deviceRow = window.findChild<QWidget*>(QStringLiteral("preferencesPerformanceDeviceRow"));
    auto* removedRoute =
        window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceRouteValue"));
    check.expect(backend != nullptr && backend->text() == QStringLiteral("Vulkan"),
                 "an injected provider supplies the backend name");
    check.expect(device != nullptr && device->text() == QStringLiteral("Test GPU"),
                 "an injected provider supplies the device row");
    check.expect(deviceRow != nullptr && !deviceRow->isHidden(),
                 "a reported device makes the device row visible");
    check.expect(removedRoute == nullptr, "the removed Available preview route row is gone");
}

// A mutable cached provider: the Performance page must follow it while the dialog is visible.
class MutableAccelerationStatus final : public ui::AccelerationStatusProvider {
  public:
    [[nodiscard]] ui::AccelerationStatus accelerationStatus() const override {
        ++queries;
        return status;
    }
    mutable int queries = 0;
    ui::AccelerationStatus status;
};

void waitForPerformanceRefresh() {
    QEventLoop loop;
    QTimer::singleShot(400, &loop, &QEventLoop::quit);
    loop.exec();
}

void testPerformancePageRefreshesWhileVisible(Expectations& check) {
    MutableAccelerationStatus provider;
    provider.status.backend = QStringLiteral("Vulkan (initializing)");
    provider.status.deviceState = QStringLiteral("Initializing");
    provider.status.previewRoute = QStringLiteral("CPU fallback");
    provider.status.presentationStatus = QStringLiteral("Not requested");
    provider.status.summary = QStringLiteral("Checking GPU acceleration.");

    ui::PreferencesWindow window(owner(), ui::ApplicationPreferences{}, &provider);
    auto* backend = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceBackendValue"));
    auto* state = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceStateValue"));
    auto* deviceRow = window.findChild<QWidget*>(QStringLiteral("preferencesPerformanceDeviceRow"));
    auto* driverRow = window.findChild<QWidget*>(QStringLiteral("preferencesPerformanceDriverRow"));
    // Fail before any dereference: a renamed/missing row must not crash the suite.
    if (backend == nullptr || state == nullptr || deviceRow == nullptr || driverRow == nullptr) {
        check.expect(false, "the Performance page exposes its named status rows and labels");
        return;
    }
    check.expect(backend->text() == QStringLiteral("Vulkan (initializing)"),
                 "the initial cached status is shown at construction");
    check.expect(deviceRow->isHidden(), "no device row while Initializing");

    window.show();
    provider.status.backend = QStringLiteral("Vulkan");
    provider.status.deviceState = QStringLiteral("Ready");
    provider.status.deviceName = QStringLiteral("Test GPU");
    provider.status.driver = QStringLiteral("test-driver 1.0");
    provider.status.previewRoute = QStringLiteral("Available for eligible previews");
    provider.status.summary =
        QStringLiteral("GPU acceleration is available for eligible previews.");
    waitForPerformanceRefresh();
    check.expect(backend->text() == QStringLiteral("Vulkan"),
                 "the visible page follows the cached status to Ready");
    check.expect(state->text() == QStringLiteral("Ready"),
                 "the device state updates while the dialog is visible");
    check.expect(deviceRow != nullptr && !deviceRow->isHidden(),
                 "the device row appears once a device is reported");
    check.expect(driverRow != nullptr && !driverRow->isHidden(),
                 "the driver row appears once a driver is reported");

    provider.status.backend = QStringLiteral("CPU reference");
    provider.status.deviceState = QStringLiteral("Unavailable");
    provider.status.deviceName.clear();
    provider.status.driver.clear();
    provider.status.summary = QStringLiteral("GPU acceleration is unavailable.");
    waitForPerformanceRefresh();
    check.expect(backend->text() == QStringLiteral("CPU reference"),
                 "the page follows a lost capability");
    check.expect(deviceRow->isHidden(), "the device row hides again after the loss");

    window.hide();
    const int queriesWhileHidden = provider.queries;
    waitForPerformanceRefresh();
    check.expect(provider.queries == queriesWhileHidden,
                 "a hidden dialog stops refreshing from the cached provider");
}

void testPerformancePageDestructionSafe(Expectations& check) {
    MutableAccelerationStatus provider;
    {
        ui::PreferencesWindow window(owner(), ui::ApplicationPreferences{}, &provider);
        window.show();
        // No explicit hide: destroying the dialog must destroy its Qt-owned timer.
    }
    const int queriesAfterDestruction = provider.queries;
    waitForPerformanceRefresh();
    check.expect(provider.queries == queriesAfterDestruction,
                 "no provider query after the dialog is destroyed");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QWidget ownerWindow;
    gOwner = &ownerWindow;
    Expectations check;
    testControlsReflectPreferences(check);
    testEditingUpdatesDraftAndApplyState(check);
    testApplyEmitsAndClearsDirty(check);
    testSetPreferencesRoundTripsControls(check);
    testRestoreDefaults(check);
    testCacheDirectoryIsValidated(check);
    testTimelineKeyframesAndGraphAreMutuallyExclusive(check);
    testRestartBadgesAndRail(check);
    testSoundsPage(check);
    testHeadroomFields(check);
    testLargeBudgetPreferencesRemainExactOnApply(check);
    testPreferenceLabelsRemainReadable(check);
    testWindowFitsItsContent(check);
    testSectionResetRestoresItsDefaults(check);
    testGridAndAutomaticWidthMatchRuntime(check);
    testPerformancePageReportsCpuOnly(check);
    testPerformancePageUsesInjectedProvider(check);
    testPerformancePageRefreshesWhileVisible(check);
    testPerformancePageDestructionSafe(check);
    return check.failures() == 0 ? 0 : 1;
}
