#include "preferences_window.hpp"

#include <bloom/ui/acceleration_status.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>

#include <iostream>
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

[[nodiscard]] ui::ApplicationPreferences samplePreferences() {
    ui::ApplicationPreferences prefs;
    prefs.audioEnabled = false;
    prefs.loopPlayback = false;
    prefs.operationCacheBytes = 10ULL * 1024 * 1024 * 1024;
    prefs.ramPreviewBytes = 6ULL * 1024 * 1024 * 1024;
    prefs.mediaDiskCacheEnabled = false;
    prefs.timelineTimeFormat = ui::TimelineTimeFormat::Timecode;
    prefs.timelineSnapping = false;
    prefs.timelineGraphEditor = true;
    prefs.timelineLayerColumnWidth = 320;
    prefs.nodeLinkStyle = ui::NodeLinkStyle::Angled;
    prefs.nodeSnap = true;
    prefs.nodeGridSize = 32.0;
    prefs.viewerResolution = ui::ViewerResolutionPreference::Half;
    prefs.viewerBackground = ui::ViewerBackgroundPreference::Checkerboard;
    prefs.viewerThirds = true;
    return prefs;
}

void testControlsReflectPreferences(Expectations& check) {
    ui::PreferencesWindow window(samplePreferences());
    const auto prefs = samplePreferences();

    auto* audio =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesAudioEnabledSwitch"));
    check.expect(audio != nullptr && !audio->isChecked(),
                 "the audio switch reflects the stored value");
    auto* loop =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesLoopPlaybackSwitch"));
    check.expect(loop != nullptr && !loop->isChecked(),
                 "the loop switch reflects the stored value");

    auto* operation = window.findChild<ui::kit::KValueField*>(
        QStringLiteral("preferencesOperationCacheBytesField"));
    check.expect(operation != nullptr && operation->value() == 10.0,
                 "the operation budget is shown in GiB");
    auto* ram =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesRamPreviewBytesField"));
    check.expect(ram != nullptr && ram->value() == 6.0, "the RAM preview budget is shown in GiB");

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
                     background->currentData().toString() == QStringLiteral("Checkerboard"),
                 "the background dropdown reflects the stored value");

    auto* grid =
        window.findChild<ui::kit::KValueField*>(QStringLiteral("preferencesNodeGridSizeField"));
    check.expect(grid != nullptr && grid->value() == 32.0,
                 "the raw-valued grid size field reflects the stored value");

    check.expect(window.preferences() == prefs, "an unedited window reports exactly its input");
}

void testEditingUpdatesDraftAndApplyState(Expectations& check) {
    ui::ApplicationPreferences initial;
    ui::PreferencesWindow window(initial);
    auto* apply = window.findChild<QPushButton*>(QStringLiteral("preferencesApplyButton"));
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

    auto* operation = window.findChild<ui::kit::KValueField*>(
        QStringLiteral("preferencesOperationCacheBytesField"));
    operation->setValue(8.0);
    check.expect(window.preferences().operationCacheBytes == 8ULL * 1024 * 1024 * 1024,
                 "a GiB budget edit round-trips to the exact byte count");
}

void testApplyEmitsAndClearsDirty(Expectations& check) {
    ui::ApplicationPreferences initial;
    ui::PreferencesWindow window(initial);
    auto* apply = window.findChild<QPushButton*>(QStringLiteral("preferencesApplyButton"));

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
    ui::PreferencesWindow window(ui::ApplicationPreferences{});
    const auto prefs = samplePreferences();
    window.setPreferences(prefs);

    auto* audio =
        window.findChild<QAbstractButton*>(QStringLiteral("preferencesAudioEnabledSwitch"));
    check.expect(audio != nullptr && !audio->isChecked(),
                 "setPreferences pushes the value into the control");

    auto* apply = window.findChild<QPushButton*>(QStringLiteral("preferencesApplyButton"));
    check.expect(apply != nullptr && apply->isEnabled(),
                 "pushing a value makes the draft differ from the applied baseline");
    check.expect(window.preferences() == prefs, "setPreferences round-trips through preferences()");
}

void testRestoreDefaults(Expectations& check) {
    ui::PreferencesWindow window(samplePreferences());
    auto* restore =
        window.findChild<QPushButton*>(QStringLiteral("preferencesRestoreDefaultsButton"));
    check.expect(restore != nullptr, "the Restore Defaults button exists");
    restore->click();
    check.expect(window.preferences() == ui::defaultApplicationPreferences(),
                 "Restore Defaults resets the draft to the model defaults");
}

void testTimelineKeyframesAndGraphAreMutuallyExclusive(Expectations& check) {
    ui::PreferencesWindow window(ui::defaultApplicationPreferences());
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
    ui::PreferencesWindow window(samplePreferences());
    const auto badges = window.findChildren<QLabel*>(QStringLiteral("preferencesRestartBadge"));
    check.expect(badges.size() >= 4, "the restart-only preferences each carry a Restart badge");
    check.expect(window.findChild<QLabel*>(QStringLiteral("preferencesRestartNotice")) != nullptr,
                 "the memory page explains the restart requirement");

    auto* rail = window.findChild<ui::kit::KRadioGroup*>(QStringLiteral("preferencesCategoryRail"));
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("preferencesPages"));
    check.expect(rail != nullptr && rail->count() == 6, "the rail carries the six categories");
    check.expect(pages != nullptr && pages->count() == 6, "the stack carries the six pages");

    rail->setCurrentIndex(2);
    check.expect(pages != nullptr && pages->currentIndex() == 2,
                 "selecting a category switches the page");
}

void testPerformancePageReportsCpuOnly(Expectations& check) {
    ui::PreferencesWindow window(ui::ApplicationPreferences{});
    auto* backend = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceBackendValue"));
    auto* state = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceStateValue"));
    check.expect(backend != nullptr && backend->text() == QStringLiteral("CPU reference"),
                 "with no provider the Performance page reports the CPU reference");
    check.expect(state != nullptr && state->text() == QStringLiteral("Unavailable"),
                 "with no provider the device state reads Unavailable");
    check.expect(window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceDeviceValue")) ==
                     nullptr,
                 "no device row is shown without a GPU device");
}

// A provider stand-in for the future capability-report-backed one.
class StubAccelerationStatus final : public ui::AccelerationStatusProvider {
  public:
    [[nodiscard]] ui::AccelerationStatus accelerationStatus() const override {
        ui::AccelerationStatus status;
        status.gpuBuildEnabled = true;
        status.backend = QStringLiteral("Vulkan");
        status.deviceState = QStringLiteral("Ready");
        status.deviceName = QStringLiteral("Test GPU");
        status.driver = QStringLiteral("test-driver 1.0");
        return status;
    }
};

void testPerformancePageUsesInjectedProvider(Expectations& check) {
    const StubAccelerationStatus provider;
    ui::PreferencesWindow window(ui::ApplicationPreferences{}, &provider);
    auto* backend = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceBackendValue"));
    auto* device = window.findChild<QLabel*>(QStringLiteral("preferencesPerformanceDeviceValue"));
    check.expect(backend != nullptr && backend->text() == QStringLiteral("Vulkan"),
                 "an injected provider supplies the backend name");
    check.expect(device != nullptr && device->text() == QStringLiteral("Test GPU"),
                 "an injected provider supplies the device row");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations check;
    testControlsReflectPreferences(check);
    testEditingUpdatesDraftAndApplyState(check);
    testApplyEmitsAndClearsDirty(check);
    testSetPreferencesRoundTripsControls(check);
    testRestoreDefaults(check);
    testTimelineKeyframesAndGraphAreMutuallyExclusive(check);
    testRestartBadgesAndRail(check);
    testPerformancePageReportsCpuOnly(check);
    testPerformancePageUsesInjectedProvider(check);
    return check.failures() == 0 ? 0 : 1;
}
