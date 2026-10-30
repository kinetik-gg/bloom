// Real-widget captures of the Export panel from hand-built snapshots: the empty list, a collapsed
// list, one job open with two outputs, a rendering job, a failed job, the inline replace question
// and the narrow width. Writes PNGs named <state>-<dpr>.png to the directory given as the first
// argument (or BLOOM_GRAMMAR_ARTIFACT_DIR) and prints their names.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "export_job_results.hpp"
#include "export_job_row.hpp"
#include "export_output_row.hpp"
#include "export_test_support.hpp"

#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_editor.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/theme.hpp>

#include <QApplication>
#include <QDir>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTest>

#include <iostream>

namespace {
using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::test;
namespace ep = bloom::ui::exportpanel;

host::ExportOutputView exrOutput(const std::uint64_t id) {
    return makeOutput(id, "builtin:exr-sequence", document::ExportFormat::OpenExr, "Exports/Shot 010.####.exr");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomExportCapture");
    app.setApplicationName("ExportCapture");
    QStandardPaths::setTestModeEnabled(true);
    kit::installKinetikTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    QDir().mkpath(directory);
    const QString dpr = app.devicePixelRatio() > 1.2 || qEnvironmentVariable("QT_SCALE_FACTOR") == "1.5"
                            ? QStringLiteral("dpr15")
                            : QStringLiteral("dpr1");

    auto seed = document::makeNewProject("Export Capture", "Shot 010",
                                         core::RationalTime::fromInteger(10));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    CompositionSession session(doc, stack, seed.initialCompositionId);
    FakeExportPort port;
    ExportQueueAdapter adapter(port);
    EditorRegistry registry;
    (void)registerExportEditor(registry, session, &adapter);
    EditorArea area(registry, "bloom.export", QString{});
    const QSize wide(900, 860);
    const QSize narrow(420, 860);
    area.resize(wide);
    area.show();
    QApplication::processEvents();
    auto* editor = area.findChild<ExportEditor*>();
    int failures = 0;
    const auto shot = [&](const QString& state, const QSize size) {
        area.resize(size);
        QTest::qWait(80);
        const QString name = QStringLiteral("%1-%2").arg(state, dpr);
        if (!area.grab().toImage().save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << area.width() << 'x' << area.height() << '\n';
    };
    const auto publish = [&](const std::function<void(host::ExportQueueSnapshot&)>& change) {
        port.publish([&](host::ExportQueueSnapshot& s) {
            change(s);
            // The queue only offers Stop All while something runs and Remove finished after a finish.
            bool finished = false;
            for (const auto& job : s.jobs)
                finished = finished || host::isTerminal(job.state);
            if (!s.activeJob.has_value())
                refuse(s.commands, host::ExportCommand::StopAll, host::ExportUnavailableCode::NothingToStop,
                       "No job is running.");
            if (!finished)
                refuse(s.commands, host::ExportCommand::RemoveFinished,
                       host::ExportUnavailableCode::NothingFinished, "No job has finished.");
        });
        adapter.drain();
        QTest::qWait(40);
    };
    const auto row = [&](const std::uint64_t id) {
        return editor->jobRow(document::ExportJobId::fromRaw(id));
    };
    const auto openOutput = [&](const std::uint64_t job, const std::uint64_t output, const bool quality) {
        auto* shown = row(job)->outputRow(document::ExportOutputId::fromRaw(output));
        shown->setExpanded(true);
        shown->qualitySection()->setCollapsed(!quality);
        QTest::qWait(40);
    };
    const auto composition = session.compositionId();

    publish([](host::ExportQueueSnapshot&) {});
    shot("empty", wide);

    // A collapsed list: one ready, one waiting its turn, one that finished.
    publish([&](host::ExportQueueSnapshot& s) {
        auto ready = makeJob(1, "Shot 010", composition, {makeOutput(11)});
        ready.record.labelColor = std::array<std::uint8_t, 3>{200, 120, 60};
        auto waiting = makeJob(2, "Shot 020", composition, {makeOutput(21)});
        waiting.state = host::ExportJobState::Waiting;
        auto done = makeJob(3, "Titles", composition, {makeOutput(31, "builtin:prores-422hq-mov",
                                                                    document::ExportFormat::ProResMov,
                                                                    "Exports/Titles.mov")});
        done.state = host::ExportJobState::Done;
        done.outputs[0].state = host::ExportOutputState::Published;
        done.record.labelColor = std::array<std::uint8_t, 3>{70, 150, 200};
        done.resolvedRange = host::ExportResolvedRange{0, 119};
        s.jobs = {ready, waiting, done};
    });
    for (const std::uint64_t id : {1UL, 2UL, 3UL})
        row(id)->setExpanded(false);
    shot("collapsed", wide);

    // One job open with two outputs: the first folded, the second open with its Quality.
    publish([&](host::ExportQueueSnapshot& s) {
        auto job = makeJob(1, "Shot 010", composition, {makeOutput(11), exrOutput(12)});
        job.record.labelColor = std::array<std::uint8_t, 3>{200, 120, 60};
        job.outputs[0].estimatedBytes = 38'000'000;
        job.outputs[0].freeBytesAtDestination = 212'000'000'000;
        job.outputs[1].destination.fileCount = 240;
        job.outputs[1].destination.firstPath = "/project/Exports/Shot 010.0000.exr";
        job.outputs[1].destination.lastPath = "/project/Exports/Shot 010.0239.exr";
        job.outputs[1].record.destination.path = "Exports/Shot 010.####.exr";
        s.jobs = {job, makeJob(2, "Shot 020", composition, {makeOutput(21)})};
    });
    row(1)->setExpanded(true);
    row(2)->setExpanded(false);
    openOutput(1, 12, true);
    shot("expanded", wide);

    // Rendering: the first job writes, the second waits for the Viewer.
    publish([&](host::ExportQueueSnapshot& s) {
        auto running = makeJob(1, "Shot 010", composition, {makeOutput(11), exrOutput(12)});
        running.state = host::ExportJobState::Rendering;
        running.progress = {.framesCompleted = 60, .framesTotal = 240, .outputIndex = 0, .outputCount = 2,
                            .currentFrame = 60, .fraction = 0.25,
                            .elapsed = std::chrono::milliseconds(31000),
                            .remaining = std::chrono::milliseconds(95000),
                            .route = host::ExportDeviceRoute::Gpu};
        running.outputs[0].state = host::ExportOutputState::Rendering;
        running.outputs[1].state = host::ExportOutputState::Pending;
        running.record.labelColor = std::array<std::uint8_t, 3>{200, 120, 60};
        refuse(running.commands, host::ExportCommand::EditJob, host::ExportUnavailableCode::LockedWhileRunning,
               "Edits are locked while the job runs.");
        for (auto& output : running.outputs)
            refuse(output.commands, host::ExportCommand::EditOutput,
                   host::ExportUnavailableCode::LockedWhileRunning, "Edits are locked while the job runs.");
        auto viewer = makeJob(2, "Shot 020", composition, {makeOutput(21)});
        viewer.state = host::ExportJobState::Rendering;
        viewer.progress = {.framesCompleted = 12, .framesTotal = 240, .currentFrame = 12, .fraction = 0.05,
                           .elapsed = std::chrono::milliseconds(8000),
                           .route = host::ExportDeviceRoute::WaitingForViewer};
        viewer.outputs[0].state = host::ExportOutputState::Rendering;
        auto next = makeJob(3, "Shot 030", composition, {makeOutput(31)});
        next.state = host::ExportJobState::Waiting;
        s.jobs = {running, viewer, next};
        s.activeJob = running.id;
    });
    row(1)->setExpanded(true);
    row(2)->setExpanded(false);
    row(3)->setExpanded(false);
    shot("rendering", wide);

    // Failed: the sentence, what was checked, the log and the details for a bug report.
    publish([&](host::ExportQueueSnapshot& s) {
        auto failed = makeJob(3, "Shot 030", composition,
                              {exrOutput(31)});
        failed.state = host::ExportJobState::Failed;
        failed.failure = host::ExportFailure{
            .code = host::ExportFailureCode::VerificationFailed,
            .reason = "The file has 239 frames but 240 were asked for. Render again, or choose another folder.",
            .summary = std::string(host::exportFailureSummary(host::ExportFailureCode::VerificationFailed))};
        failed.outputs[0].state = host::ExportOutputState::Failed;
        host::ExportOutputVerification verification;
        verification.output = failed.outputs[0].id;
        verification.overall = host::ExportCheckOutcome::Failed;
        verification.checks.push_back({.check = host::ExportCheck::Reopen, .outcome = host::ExportCheckOutcome::Passed});
        verification.checks.push_back({.check = host::ExportCheck::FrameCount,
                                       .outcome = host::ExportCheckOutcome::Failed,
                                       .expected = std::uint64_t{240}, .observed = std::uint64_t{239}});
        verification.provenance.gpuFrames = 239;
        failed.outputs[0].verification = verification;
        failed.lastLogSequence = 2;
        port.logs[3] = {{.sequence = 1, .sinceStart = std::chrono::milliseconds(1000), .code = "frame.ok",
                         .message = "Frame 1 written"},
                        {.sequence = 2, .sinceStart = std::chrono::milliseconds(9000), .code = "verify.frames",
                         .message = "Frame count differs"}};
        s.jobs = {makeJob(1, "Shot 010", composition, {makeOutput(11)}), failed};
        s.jobs[0].state = host::ExportJobState::Done;
        s.jobs[0].outputs[0].state = host::ExportOutputState::Published;
        s.activeJob = std::nullopt;
    });
    row(1)->setExpanded(false);
    row(3)->setExpanded(true);
    openOutput(3, 31, false);
    if (auto* log = row(3)->results()->logSection())
        log->setCollapsed(false);
    shot("failed", wide);

    // The inline replace question.
    publish([&](host::ExportQueueSnapshot& s) {
        auto output = makeOutput(11, "builtin:h264-mp4", document::ExportFormat::H264Mp4, "Exports/Shot 010.mp4");
        output.preflight.push_back({host::ExportPreflightCode::DestinationExists,
                                    host::ExportPreflightTarget::Destination,
                                    host::ExportPreflightSeverity::NeedsConfirmation,
                                    "Shot 010.mp4 already exists."});
        output.destination.firstPath = "/project/Exports/Shot 010.mp4";
        auto job = makeJob(1, "Shot 010", composition, {output});
        refuse(job.commands, host::ExportCommand::Start,
               host::ExportUnavailableCode::NeedsReplaceConfirmation, "Replace existing files?");
        auto second = makeJob(2, "Shot 020", composition, {makeOutput(21)});
        s.jobs = {job, second};
        s.activeJob = std::nullopt;
    });
    row(2)->setExpanded(false);
    shot("replace", wide);

    // Narrow: a rendering list and an open job.
    publish([&](host::ExportQueueSnapshot& s) {
        auto running = makeJob(1, "Shot 010", composition, {makeOutput(11), exrOutput(12)});
        running.state = host::ExportJobState::Rendering;
        running.progress = {.framesCompleted = 60, .framesTotal = 240, .currentFrame = 60, .fraction = 0.25,
                            .elapsed = std::chrono::milliseconds(31000),
                            .remaining = std::chrono::milliseconds(95000),
                            .route = host::ExportDeviceRoute::Gpu};
        running.outputs[0].state = host::ExportOutputState::Rendering;
        running.record.labelColor = std::array<std::uint8_t, 3>{200, 120, 60};
        auto ready = makeJob(2, "Shot 020", composition, {makeOutput(21)});
        auto done = makeJob(3, "Titles", composition, {makeOutput(31)});
        done.state = host::ExportJobState::Done;
        done.outputs[0].state = host::ExportOutputState::Published;
        s.jobs = {running, ready, done};
        s.activeJob = running.id;
    });
    row(1)->setExpanded(false);
    row(2)->setExpanded(false);
    row(3)->setExpanded(false);
    shot("narrow", narrow);
    row(2)->setExpanded(true);
    openOutput(2, 21, true);
    shot("narrow-open", narrow);
    return failures == 0 ? 0 : 1;
}
