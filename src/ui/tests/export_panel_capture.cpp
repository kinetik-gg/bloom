// Real-widget captures of the Export panel (the After Effects Render Queue layout) from hand-built
// snapshots, at three panel widths: the empty list, three queued jobs with one open on two outputs,
// a rendering job, a failed job, and the Render Settings, Output Module (ProRes and OpenEXR) and
// replace dialogs over the panel. Writes PNGs named <state>-<width>-<dpr>.png to the directory given
// as the first argument (or BLOOM_GRAMMAR_ARTIFACT_DIR) and prints their names.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "export_job_lines.hpp"
#include "export_output_module_dialog.hpp"
#include "export_render_list.hpp"
#include "export_render_settings_dialog.hpp"
#include "export_test_support.hpp"

#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_editor.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <iostream>

namespace {
using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::test;
namespace ep = bloom::ui::exportpanel;

host::ExportOutputView proresOutput(const std::uint64_t id, const std::string& name) {
    auto output = makeOutput(id, "builtin:prores-422hq-mov", document::ExportFormat::ProResMov,
                             "Exports/{name}.mov");
    output.destination.firstPath = "/project/Exports/" + name + ".mov";
    output.destination.lastPath = output.destination.firstPath;
    output.estimatedBytes = 1'840'000'000;
    output.freeBytesAtDestination = 212'000'000'000;
    return output;
}

host::ExportOutputView exrOutput(const std::uint64_t id, const std::string& name) {
    auto output = makeOutput(id, "builtin:exr-sequence", document::ExportFormat::OpenExr,
                             "Exports/{name}.exr");
    output.destination.firstPath = "/project/Exports/" + name + ".0000.exr";
    output.destination.lastPath = "/project/Exports/" + name + ".2155.exr";
    output.destination.fileCount = 2156;
    return output;
}

host::ExportJobView shotJob(const std::uint64_t id, const std::string& name,
                            const document::CompositionId composition,
                            std::vector<host::ExportOutputView> outputs) {
    auto job = makeJob(id, name, composition, std::move(outputs));
    job.resolvedRange = host::ExportResolvedRange{0, 2155};
    return job;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomExportCapture");
    app.setApplicationName("ExportCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    kit::installKinetikTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    QDir().mkpath(directory);
    const QString dpr = app.devicePixelRatio() > 1.2 || qEnvironmentVariable("QT_SCALE_FACTOR") == "1.5"
                            ? QStringLiteral("dpr15")
                            : QStringLiteral("dpr1");

    auto seed = document::makeNewProject("Export Capture", "SC020", core::RationalTime::fromInteger(90));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    CompositionSession session(doc, stack, seed.initialCompositionId);
    FakeExportPort port;
    ExportQueueAdapter adapter(port);
    EditorRegistry registry;
    (void)registerExportEditor(registry, session, &adapter);
    EditorArea area(registry, "bloom.export", QString{});
    const std::vector<std::pair<QString, QSize>> widths = {
        {QStringLiteral("2000"), QSize(2000, 520)}, {QStringLiteral("900"), QSize(900, 520)},
        {QStringLiteral("420"), QSize(420, 620)}};
    area.resize(widths.front().second);
    area.show();
    QApplication::processEvents();
    auto* editor = area.findChild<ExportEditor*>();
    int failures = 0;
    const auto save = [&](const QImage& image, const QString& state, const QString& width) {
        const QString name = QStringLiteral("%1-%2-%3").arg(state, width, dpr);
        if (!image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
    };
    // The panel, with a dialog over it when one is open, as the artist sees them.
    const auto shot = [&](const QString& state, kit::KDialog* dialog = nullptr) {
        for (const auto& [width, size] : widths) {
            area.resize(size);
            QTest::qWait(80);
            auto image = area.grab().toImage();
            if (dialog != nullptr) {
                QTest::qWait(40);
                const auto over = dialog->grab().toImage();
                const auto ratio = image.devicePixelRatio();
                const QSize logical(std::max(qRound(image.width() / ratio), qRound(over.width() / ratio)),
                                    std::max(qRound(image.height() / ratio), qRound(over.height() / ratio)));
                QImage combined(logical * ratio, QImage::Format_ARGB32_Premultiplied);
                combined.setDevicePixelRatio(ratio);
                combined.fill(kit::color(kit::Color::Canvas));
                QPainter painter(&combined);
                painter.drawImage(QPoint(0, 0), image);
                const QSize overSize(qRound(over.width() / ratio), qRound(over.height() / ratio));
                painter.drawImage(QPoint((logical.width() - overSize.width()) / 2,
                                         (logical.height() - overSize.height()) / 2),
                                  over);
                painter.end();
                image = combined;
            }
            save(image, state, width);
        }
    };
    const auto publish = [&](const std::function<void(host::ExportQueueSnapshot&)>& change) {
        port.publish([&](host::ExportQueueSnapshot& s) {
            s.commands = allAvailable();
            change(s);
            if (!s.activeJob.has_value())
                refuse(s.commands, host::ExportCommand::StopAll, host::ExportUnavailableCode::NothingToStop,
                       "No job is running.");
        });
        adapter.drain();
        QTest::qWait(40);
    };
    const auto composition = session.compositionId();
    const auto open = [&](const std::uint64_t job, const bool expanded) {
        editor->setJobExpanded(document::ExportJobId::fromRaw(job), expanded);
    };

    publish([](host::ExportQueueSnapshot&) {});
    shot("empty");

    // Three queued jobs; the first open on two outputs.
    const auto queued = [&](host::ExportQueueSnapshot& s) {
        s.jobs = {shotJob(1, "SC020", composition, {proresOutput(11, "SC020"), exrOutput(12, "SC020")}),
                  shotJob(2, "SC030", composition, {proresOutput(21, "SC030")}),
                  shotJob(3, "SC040", composition, {proresOutput(31, "SC040")})};
        s.activeJob = std::nullopt;
    };
    publish(queued);
    open(1, true);
    open(2, false);
    open(3, false);
    shot("queued");

    // Rendering: the first job writes, the strip open on its bar.
    publish([&](host::ExportQueueSnapshot& s) {
        queued(s);
        auto& running = s.jobs[0];
        running.state = host::ExportJobState::Rendering;
        running.run = host::ExportRunLink{.run = 1};
        running.progress = {.framesCompleted = 403, .framesTotal = 2156, .outputIndex = 0, .outputCount = 2,
                            .currentFrame = 403, .fraction = 0.187,
                            .elapsed = std::chrono::milliseconds(28000),
                            .remaining = std::chrono::milliseconds(122000),
                            .route = host::ExportDeviceRoute::Gpu};
        running.outputs[0].state = host::ExportOutputState::Rendering;
        refuse(running.commands, host::ExportCommand::EditJob, host::ExportUnavailableCode::LockedWhileRunning,
               "Settings are locked while this job renders.");
        for (auto& output : running.outputs)
            refuse(output.commands, host::ExportCommand::EditOutput, host::ExportUnavailableCode::LockedWhileRunning,
                   "Settings are locked while this job renders.");
        s.jobs[1].state = host::ExportJobState::Waiting;
        s.activeJob = running.id;
    });
    editor->setCurrentRenderExpanded(true);
    shot("rendering");
    editor->setCurrentRenderExpanded(false);

    // Failed: the third job failed its check; the first finished.
    publish([&](host::ExportQueueSnapshot& s) {
        queued(s);
        s.jobs[0].state = host::ExportJobState::Done;
        s.jobs[0].run = host::ExportRunLink{.run = 1};
        s.jobs[0].progress.elapsed = std::chrono::milliseconds(151000);
        for (auto& output : s.jobs[0].outputs)
            output.state = host::ExportOutputState::Published;
        auto& failed = s.jobs[2];
        failed.state = host::ExportJobState::Failed;
        failed.run = host::ExportRunLink{.run = 2};
        failed.progress.elapsed = std::chrono::milliseconds(9000);
        failed.failure = host::ExportFailure{
            .code = host::ExportFailureCode::VerificationFailed,
            .reason = "The file has 239 frames but 240 were asked for. Render again, or choose another folder.",
            .summary = std::string(host::exportFailureSummary(host::ExportFailureCode::VerificationFailed))};
        failed.outputs[0].state = host::ExportOutputState::Failed;
    });
    open(1, false);
    shot("failed");

    // The dialogs, over the queued list.
    publish(queued);
    open(1, true);
    auto* settings = editor->openRenderSettings(document::ExportJobId::fromRaw(1));
    QTest::qWait(60);
    shot("render-settings", settings);
    settings->reject();
    QTest::qWait(40);
    auto* prores = editor->openOutputModule(document::ExportJobId::fromRaw(1), document::ExportOutputId::fromRaw(11));
    QTest::qWait(60);
    shot("output-module-prores", prores);
    prores->reject();
    QTest::qWait(40);
    auto* exr = editor->openOutputModule(document::ExportJobId::fromRaw(1), document::ExportOutputId::fromRaw(12));
    QTest::qWait(60);
    shot("output-module-exr", exr);
    exr->reject();
    QTest::qWait(40);

    // The replace question Start All asks.
    publish([&](host::ExportQueueSnapshot& s) {
        queued(s);
        auto& output = s.jobs[1].outputs[0];
        output.preflight.push_back({host::ExportPreflightCode::DestinationExists,
                                    host::ExportPreflightTarget::Destination,
                                    host::ExportPreflightSeverity::NeedsConfirmation, "SC030.mov already exists."});
        refuse(s.jobs[1].commands, host::ExportCommand::Start, host::ExportUnavailableCode::NeedsReplaceConfirmation,
               "Replace existing files?");
    });
    open(1, false);
    editor->startAll();
    QTest::qWait(60);
    kit::KDialog* prompt = nullptr;
    for (auto* dialog : area.findChildren<kit::KDialog*>(QStringLiteral("exportReplaceConfirm")))
        if (dialog->isVisible())
            prompt = dialog;
    if (prompt == nullptr)
        ++failures;
    shot("replace", prompt);
    if (prompt != nullptr)
        prompt->reject();
    return failures == 0 ? 0 : 1;
}
