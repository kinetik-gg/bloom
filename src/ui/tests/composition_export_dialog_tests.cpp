// Lane C: Export Composition asks the export provider which H.264 encoder this platform uses, and
// shows install / consent / Locate only for OpenH264. Verifying the installed binary and Locate's
// hash-and-copy run as tasks on a worker, never on the UI thread; the provider answer is injected
// here so every platform's branch runs on any machine.
#include "composition_export_dialog.hpp"

#include <bloom/output/output_analysis.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/h264_encoder_service.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>

namespace {

using namespace bloom;
using ui::H264EncoderService;

int failures = 0;
void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

struct Machine final {
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge{scheduler, nullptr, std::chrono::milliseconds{1}};
    ~Machine() {
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};

// What the dialog shows for H.264 on a machine described by `support` and `probe`.
struct H264View final {
    bool installVisible = false;
    bool hardwareVisible = false;
    bool hardwareChecked = false;
    bool hardwareEnabled = false;
    bool continueEnabled = false;
    QString note;
    QString refusal;
};

[[nodiscard]] H264View viewFor(QWidget& owner, H264EncoderService& service) {
    H264View view;
    const bool available =
        output::outputPresetAvailabilityV1(output::OutputPresetV1::H264MovV1).available;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<ui::kit::KDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr)
            return;
        (void)QTest::qWaitFor([&] { return service.state().probed; }, 5000);
        auto* preset = dialog->findChild<ui::kit::KDropdown*>("compositionExportPreset");
        const auto index =
            preset ? preset->findData(static_cast<int>(output::OutputPresetV1::H264MovV1)) : -1;
        if (preset && index >= 0 && available)
            preset->chooseIndex(index);
        if (auto* destination = dialog->findChild<QLineEdit*>("compositionExportDestination"))
            destination->setText(QStringLiteral("review.mov"));
        QApplication::processEvents();
        auto* install = dialog->findChild<QWidget*>("installOpenH264");
        auto* hardware = dialog->findChild<ui::kit::KCheckBox*>("useVaapi");
        auto* proceed = dialog->findChild<QAbstractButton*>("compositionExportContinue");
        auto* note = dialog->findChild<QLabel*>("compositionExportNote");
        view.installVisible = install && install->isVisibleTo(dialog);
        view.hardwareVisible = hardware && hardware->isVisibleTo(dialog);
        view.hardwareChecked = hardware && hardware->isChecked();
        view.hardwareEnabled = hardware && hardware->isEnabled();
        view.continueEnabled = proceed && proceed->isEnabled();
        view.note = note ? note->text() : QString{};
        view.refusal = dialog->refusal() ? dialog->refusal()->reason : QString{};
        if (const auto shots = qEnvironmentVariable("BLOOM_SCREENSHOT_DIR"); !shots.isEmpty()) {
            QTest::qWait(100);
            (void)dialog->grab().save(shots + QStringLiteral("/export-h264-") +
                                      QString::number(static_cast<int>(
                                          service.state().support.software)) +
                                      QStringLiteral(".png"));
        }
        dialog->reject();
    });
    (void)ui::compositionExportDialog(owner, {.maximumFrame = 47,
                                              .sampleRate = 48000,
                                              .projectKey = {},
                                              .colorSettings = nullptr,
                                              .encoder = &service});
    return view;
}

void testVideoToolboxNeedsNoConsent(QWidget& owner) {
    Machine machine;
    H264EncoderService service(
        machine.scheduler, machine.bridge,
        {.software = output::H264EncoderV1::VideoToolbox,
         .hardwareVaapi = false,
         .softwareReason = {}},
        [](const output::H264EncoderSupportV1&) { return ui::H264EncoderProbe{}; });
    const auto view = viewFor(owner, service);
    if (!output::outputPresetAvailabilityV1(output::OutputPresetV1::H264MovV1).available)
        return;
    expect(!view.installVisible && !view.hardwareVisible,
           "VideoToolbox: no install, consent, Locate or VA-API controls");
    expect(view.continueEnabled && view.note.contains(QStringLiteral("VideoToolbox")),
           "VideoToolbox: Continue is enabled and the note names the encoder");
}

void testOpenH264AsksFirst(QWidget& owner) {
    Machine machine;
    H264EncoderService service(
        machine.scheduler, machine.bridge,
        {.software = output::H264EncoderV1::OpenH264, .hardwareVaapi = true, .softwareReason = {}},
        [](const output::H264EncoderSupportV1&) {
            return ui::H264EncoderProbe{.vaapiDevice = false,
                                        .openh264Installed = false,
                                        .detail = "H.264 encoder not installed"};
        });
    const auto view = viewFor(owner, service);
    if (!output::outputPresetAvailabilityV1(output::OutputPresetV1::H264MovV1).available)
        return;
    expect(view.installVisible && !view.hardwareVisible,
           "OpenH264 not installed: install and Locate show; no render node, no VA-API switch");
    expect(!view.continueEnabled && view.refusal.contains(QStringLiteral("Install OpenH264")),
           "OpenH264 not installed: Continue waits, saying what to do");
}

void testOpenH264Installed(QWidget& owner) {
    Machine machine;
    H264EncoderService service(
        machine.scheduler, machine.bridge,
        {.software = output::H264EncoderV1::OpenH264, .hardwareVaapi = true, .softwareReason = {}},
        [](const output::H264EncoderSupportV1&) {
            return ui::H264EncoderProbe{.vaapiDevice = true,
                                        .openh264Installed = true,
                                        .detail = "OpenH264 2.6.0 verified"};
        });
    const auto view = viewFor(owner, service);
    if (!output::outputPresetAvailabilityV1(output::OutputPresetV1::H264MovV1).available)
        return;
    expect(!view.installVisible && view.hardwareVisible && view.hardwareEnabled &&
               view.continueEnabled,
           "OpenH264 verified: no install UI, VA-API offered, Continue enabled");
}

void testHardwareOnly(QWidget& owner) {
    Machine machine;
    H264EncoderService service(machine.scheduler, machine.bridge,
                               {.software = output::H264EncoderV1::None,
                                .hardwareVaapi = true,
                                .softwareReason = "no OpenH264 here"},
                               [](const output::H264EncoderSupportV1&) {
                                   return ui::H264EncoderProbe{.vaapiDevice = true,
                                                               .openh264Installed = false,
                                                               .detail = {}};
                               });
    const auto view = viewFor(owner, service);
    if (!output::outputPresetAvailabilityV1(output::OutputPresetV1::H264MovV1).available)
        return;
    expect(!view.installVisible && view.hardwareVisible && view.hardwareChecked &&
               !view.hardwareEnabled && view.continueEnabled,
           "VA-API only: hardware is on and fixed, no OpenH264 UI, Continue enabled");
}

void testWorkStaysOffTheUiThread() {
    Machine machine;
    const auto uiThread = std::this_thread::get_id();
    std::mutex lock;
    std::thread::id probedOn;
    std::thread::id locatedOn;
    H264EncoderService service(
        machine.scheduler, machine.bridge,
        {.software = output::H264EncoderV1::OpenH264, .hardwareVaapi = true, .softwareReason = {}},
        [&](const output::H264EncoderSupportV1&) {
            const std::scoped_lock guard(lock);
            probedOn = std::this_thread::get_id();
            return ui::H264EncoderProbe{};
        },
        [&](const std::filesystem::path&, const std::function<bool()>&) {
            const std::scoped_lock guard(lock);
            locatedOn = std::this_thread::get_id();
            return output::H264RuntimeAvailabilityV1{.installed = true,
                                                     .version = "2.6.0",
                                                     .digest = {},
                                                     .directory = {},
                                                     .detail = "verified"};
        });
    expect(service.state().runtime == H264EncoderService::Runtime::Checking,
           "the encoder starts as being checked, never hashed in place");
    service.refresh();
    expect(QTest::qWaitFor([&] { return service.state().probed; }, 5000), "the probe answers");
    {
        const std::scoped_lock guard(lock);
        expect(probedOn != std::thread::id{} && probedOn != uiThread,
               "verify runs on a worker, not the UI thread");
    }
    bool located = false;
    QObject::connect(&service, &H264EncoderService::locateFinished,
                     [&located](const bool installed, const QString&) { located = installed; });
    expect(service.locate("/tmp/libopenh264.so.8") && service.locating(),
           "Locate starts as a task");
    expect(QTest::qWaitFor([&] { return located; }, 5000), "Locate finishes");
    {
        const std::scoped_lock guard(lock);
        expect(locatedOn != std::thread::id{} && locatedOn != uiThread,
               "Locate's hash and copy run on a worker");
    }
    expect(service.state().runtime == H264EncoderService::Runtime::Installed,
           "a located binary makes OpenH264 installed");
}

void testLocateCancels() {
    Machine machine;
    std::atomic<bool> started = false;
    H264EncoderService service(
        machine.scheduler, machine.bridge,
        {.software = output::H264EncoderV1::OpenH264, .hardwareVaapi = false, .softwareReason = {}},
        [](const output::H264EncoderSupportV1&) { return ui::H264EncoderProbe{}; },
        [&started](const std::filesystem::path&, const std::function<bool()>& cancelled) {
            started = true;
            while (!cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return output::H264RuntimeAvailabilityV1{};
        });
    bool finished = false;
    bool installed = true;
    QObject::connect(&service, &H264EncoderService::locateFinished,
                     [&](const bool result, const QString&) {
                         finished = true;
                         installed = result;
                     });
    (void)service.locate("/tmp/libopenh264.so.8");
    expect(QTest::qWaitFor([&] { return started.load(); }, 5000), "Locate is running");
    service.cancelLocate();
    expect(QTest::qWaitFor([&] { return finished; }, 5000) && !installed && !service.locating(),
           "Cancel stops Locate without installing");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    ui::kit::installKinetikTheme(application);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QApplication::setOrganizationName(QStringLiteral("BloomCompositionExportDialogTests"));
    QWidget owner;
    testVideoToolboxNeedsNoConsent(owner);
    testOpenH264AsksFirst(owner);
    testOpenH264Installed(owner);
    testHardwareOnly(owner);
    testWorkStaysOffTheUiThread();
    testLocateCancels();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
