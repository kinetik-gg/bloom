// Real-widget captures of the Export panel from hand-built snapshots: empty, three jobs (one
// rendering, one waiting for the Viewer, one failed with verification), an output's detail, the
// replace confirmation and a narrow width. Writes PNGs to the directory given as the first
// argument (or BLOOM_GRAMMAR_ARTIFACT_DIR) and prints their names.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "export_job_tree.hpp"
#include "export_test_support.hpp"

#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_editor.hpp>
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
}

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
    area.resize(1000, 780);
    area.show();
    QApplication::processEvents();
    int failures = 0;
    const auto shot = [&](const QString& state, const QSize size) {
        area.resize(size);
        QTest::qWait(60);
        const QString name = QStringLiteral("export-%1-%2-%3").arg(state, size.width() > 600 ? "wide" : "narrow", dpr);
        if (!area.grab().toImage().save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << area.width() << 'x' << area.height() << '\n';
    };
    const auto publish = [&](const std::function<void(host::ExportQueueSnapshot&)>& change) {
        port.publish(change);
        adapter.drain();
        QTest::qWait(30);
    };
    const QSize wide(1000, 780);
    const QSize narrow(420, 780);
    shot("empty", wide);

    publish([&](host::ExportQueueSnapshot& s) { fillThreeJobs(s, session.compositionId());
        // The second job waits for the Viewer instead of merely queueing.
        s.jobs[1].state = host::ExportJobState::Rendering;
        s.jobs[1].progress = {.framesCompleted = 60, .framesTotal = 240, .currentFrame = 60,
                              .fraction = 0.25, .elapsed = std::chrono::milliseconds(31000),
                              .route = host::ExportDeviceRoute::WaitingForViewer};
        s.jobs[0].state = host::ExportJobState::Rendering;
        s.activeJob = s.jobs[1].id;
        s.jobs[0].record.labelColor = std::array<std::uint8_t, 3>{200, 120, 60};
    });
    auto* tree = area.findChild<exportpanel::ExportJobTree*>();
    tree->clearSelection();
    shot("three-jobs", wide);
    // The waiting job is the active one in the strip; show the failed job's output detail.
    tree->setCurrentItem(tree->topLevelItem(2)->child(0));
    QTest::qWait(60);
    shot("output-detail", wide);
    // The replace confirmation.
    publish([&](host::ExportQueueSnapshot& s) {
        auto& job = s.jobs[0];
        job.state = host::ExportJobState::Ready;
        job.commands = allAvailable();
        job.outputs[0].preflight.push_back({host::ExportPreflightCode::DestinationExists,
                                            host::ExportPreflightTarget::Destination,
                                            host::ExportPreflightSeverity::NeedsConfirmation,
                                            "renders/out.mp4 already exists."});
        refuse(job.commands, host::ExportCommand::Start,
               host::ExportUnavailableCode::NeedsReplaceConfirmation, "Replace existing files?");
        s.activeJob = std::nullopt;
    });
    tree->setCurrentItem(tree->topLevelItem(0));
    QTest::qWait(60);
    if (auto* scroll = area.findChild<QScrollArea*>("exportDetailScroll"))
        if (auto* replace = area.findChild<QWidget*>("exportReplaceButton"))
            scroll->ensureWidgetVisible(replace, 0, 120);
    QTest::qWait(60);
    shot("replace", wide);
    shot("narrow", narrow);
    return failures == 0 ? 0 : 1;
}
