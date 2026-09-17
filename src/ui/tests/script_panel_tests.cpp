#include "script_panel.hpp"
#include "window_fixture.hpp"

#include <QAction>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <bloom/commands/operations.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <iostream>

namespace {
using namespace bloom;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename Predicate> void waitFor(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 30000)
        QTest::qWait(5);
    require(predicate(), "Script integration timed out");
}
core::RationalTime halfSecond() {
    const auto time = core::RationalTime::create(1, 2);
    if (!time)
        throw std::runtime_error("Invalid test time");
    return *time;
}
QString quote(const QString& text) {
    const auto json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
QByteArray bytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Render artifact exists");
    return file.readAll();
}
void exercise() {
    ui::test::WindowFixture fixture([](auto& fixture) {
        const auto format = document::CompositionFormat::create(64, 64);
        require(format.has_value(), "Small format");
        commands::Transaction transaction("Small scene", fixture.session.snapshot().revision());
        transaction.template emplace<commands::SetCompositionFormat>(
            fixture.session.compositionId(),
            *format); // NOLINT(bugprone-unchecked-optional-access) -- checked above.
        require(fixture.session.executeTransaction(std::move(transaction)).succeeded(),
                "Small scene applies");
        require(fixture.session.addSolidLayer("Background", {0.1, 0.5, 0.8, 1}), "Solid layer");
        require(fixture.session.setSelectedPosition(32, 32), "Position solid");
        // Production connects rebinding before registering editors.
        QObject::connect(
            &fixture.projectHost, &ui::ProjectHost::sessionReplaced, &fixture.session, [&fixture] {
                const auto [document, stack] = fixture.projectHost.liveDocumentAndStack();
                if (document && stack)
                    fixture.session.rebind(*document, *stack,
                                           fixture.projectHost.lowestCompositionId());
            });
    });
    const auto areas = fixture.window->findChildren<ui::EditorArea*>();
    require(!areas.empty() && areas.front()->setEditorId("bloom.script"),
            "Script is a registered editor");
    auto* panel = areas.front()->findChild<ui::ScriptPanel*>();
    auto* input = panel->findChild<ui::kit::KLineEdit*>("scriptInput");
    auto* output = panel->findChild<ui::kit::KLabel*>("scriptOutput");
    require(input && output && input->isEnabled(), "Script uses enabled kit controls");
    QSignalSpy finished(panel, &ui::ScriptPanel::executionFinished);
    const auto start = [&](const QString& source) {
        input->setText("exec(" + quote(source) + ")");
        fixture.window->activateWindow();
        input->setFocus();
        QTest::qWait(20);
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
        require(input->text().isEmpty(), "Ctrl+Enter submits the input");
    };
    const auto run = [&](const QString& source, bool success = true) {
        const auto before = finished.size();
        start(source);
        waitFor([&] { return finished.size() > before; });
        if (finished.last().front().toBool() != success) {
            std::cerr << output->text().toStdString() << '\n';
            throw std::runtime_error("Unexpected Python result");
        }
    };
    bool uiThread = true;
    auto* stack = fixture.projectHost.liveDocumentAndStack().second;
    auto observer = stack->addObserver([&](const commands::CommandEvent&) {
        uiThread = uiThread && QThread::currentThread() == qApp->thread();
    });
    const auto* position = fixture.session.parameterForSelection(document::kPositionParameterRole);
    require(position != nullptr, "Position parameter exists");
    const auto parameter = position->id;
    const auto revision = fixture.session.snapshot().revision();
    run(QStringLiteral(
            "import threading\n"
            "assert bloom.app.gui and not bloom.app.headless\n"
            "assert int(bloom.context.composition) == %1\n"
            "assert bloom.context.selection\n"
            "old = bloom.context.project\n"
            "events = []\n"
            "subscription = bloom.events.subscribe(lambda event: "
            "events.append((threading.get_ident(), event)))\n"
            "with bloom.transactions.group('Python key', expected_revision=%2):\n"
            "    bloom.ops.animation.create_for_parameter(composition=%1, parameter=%3, time=(0, "
            "1))\n"
            "    bloom.ops.animation.set_keyframe_at_time_for_parameter_component(composition=%1, "
            "parameter=%3, component=0, time=(1, 2), value=40.0)\n"
            "assert bloom.events.pump() > 0\n"
            "assert all(identity == threading.get_ident() for identity, event in events)\n"
            "print('Keyed position X at frame 12')")
            .arg(fixture.session.compositionId().value())
            .arg(revision.value())
            .arg(parameter.value()));
    require(uiThread, "Every Python transaction executes on the UI thread");
    require(fixture.session.snapshot().revision().value() == revision.value() + 1,
            "Python group makes exactly one revision");
    const auto* keyed = fixture.session.composition()->parameters().find(parameter);
    require(keyed && std::holds_alternative<document::AnimationCurveSource>(keyed->source),
            "Parameter animated");
    const auto curveId = std::get<document::AnimationCurveSource>(keyed->source).curveId;
    const auto* curve = fixture.session.composition()->animationCurves().findComponent(
        curveId, document::AnimationComponent::X);
    require(curve && curve->keyframes.size() == 2 && curve->keyframes.back().value == 40.0,
            "Python writes the requested keyframe");
    auto* undo = fixture.window->findChild<QAction*>("undoAction");
    require(undo && undo->isEnabled() && undo->text().contains("Python key"),
            "UI offers Python undo step");
    undo->trigger();
    const auto* reverted = fixture.session.composition()->parameters().find(parameter);
    require(reverted && std::holds_alternative<document::ConstantValueSource>(reverted->source),
            "UI undo removes both Python operations");
    require(fixture.session.redo(), "UI redo restores Python keys");
    run("assert bloom.events.pump() > 0\nprint('UI undo and redo observed')");

    QTemporaryDir directory(QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR "/script-test-XXXXXX"));
    require(directory.isValid(), "Artifact directory");
    const auto project = directory.filePath("scene.bloom");
    QSignalSpy saved(&fixture.projectHost, &ui::ProjectHost::saveFinished);
    fixture.projectHost.beginSaveAs(project.toStdString());
    waitFor([&] { return !saved.empty(); });
    require(QFile::exists(project), "UI saved the shared project");
    const auto pythonPath = directory.filePath("python.png");
    run("result = bloom.render.frame(12, out=" + quote(pythonPath) +
        ")\nassert result.published_frames == 1");
    QProcess cli;
    cli.start(QStringLiteral(BLOOM_SCRIPT_CLI),
              {"render", "--project", project, "--frame", "12", "--preset", "PngRgba8SrgbV1",
               "--out", directory.filePath("cli")});
    waitFor([&] { return cli.state() == QProcess::NotRunning; });
    require(cli.exitStatus() == QProcess::NormalExit && cli.exitCode() == 0,
            "CLI rendered frame 12");
    require(fixture.session.setCurrentTime(halfSecond()), "UI selects frame 12");
    fixture.exporter->setApprovalDecisionProvider(
        [](const auto&) { return ui::FrameExportApprovalDecision::Export; });
    QSignalSpy exported(fixture.exporter.get(), &ui::FrameExportController::exportFinished);
    const auto uiPath = directory.filePath("ui.png");
    fixture.exporter->beginExport(uiPath.toStdString());
    waitFor([&] { return !exported.empty(); });
    const auto rendered = bytes(pythonPath);
    require(!rendered.isEmpty() && rendered == bytes(uiPath) &&
                rendered == bytes(directory.filePath("cli/frame.png")),
            "Python, CLI and UI publish identical frame-12 bytes");
    run("assert bloom.context.time.numerator == 1 and bloom.context.time.denominator == 2");

    int heartbeats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start(5);
    run("import time\ntime.sleep(0.2)\nprint('Still responsive')");
    require(heartbeats > 5, "Python does not block the UI event loop");
    run("raise ValueError('inline error')", false);
    require(output->text().contains("ValueError: inline error"), "Errors appear inline");
    run("def work(task):\n    task.report_progress(1, 100, 'Computing')\n    while True: pass\njob "
        "= bloom.tasks.submit(work)");
    ui::kit::KButton* cancel = nullptr;
    for (auto* button : areas.front()->findChildren<ui::kit::KButton*>())
        if (button->text() == "Cancel")
            cancel = button;
    require(cancel != nullptr, "Cancel kit button exists");
    waitFor([&] { return cancel->isEnabled(); });
    cancel->click();
    QTest::qWait(100);
    run("assert job.cancellation_requested\nassert job.done\nprint('Task cancelled')");
    const auto beforeCancel = finished.size();
    start("while True: pass");
    QTimer::singleShot(100, cancel, &ui::kit::KButton::click);
    waitFor([&] { return finished.size() > beforeCancel; });
    require(!finished.last().front().toBool() && output->text().contains("CancelledError"),
            "Foreground loop cancels");
    run("print('Console recovered after cancellation')");
    heartbeat.stop();
    const auto expected = fixture.session.snapshot().revision().value();
    QTimer::singleShot(50, &fixture.session, [&fixture] {
        commands::Transaction change("UI wins", fixture.session.snapshot().revision());
        change.emplace<commands::SetProjectName>("UI wins");
        require(fixture.session.executeTransaction(std::move(change)).succeeded(),
                "Concurrent UI edit");
    });
    run(QStringLiteral(
            "time.sleep(0.2)\nwith bloom.transactions.group('Stale', expected_revision=%1):\n    "
            "bloom.ops.project.set_name(name='Must not win')")
            .arg(expected),
        false);
    require(fixture.session.snapshot().project().name() == "UI wins",
            "Queued revision conflict preserves the UI edit");
    require(uiThread, "UI owns all command events");
    stack->removeObserver(observer);

    fixture.projectHost.setUnsavedChangeDecisionProvider(
        [] { return ui::UnsavedChangeDecision::Discard; });
    const auto beforeReplace = finished.size();
    start("time.sleep(0.2)\nbloom.ops.project.set_name(name='Must not reach new project')");
    QTimer::singleShot(50, &fixture.projectHost, &ui::ProjectHost::newProject);
    waitFor([&] { return finished.size() > beforeReplace; });
    require(!finished.last().front().toBool(), "Project replacement cancels old work");
    run("assert bloom.context.project.name == 'Untitled'\ntry:\n    old.resolve()\nexcept "
        "bloom.StaleObjectError as error:\n    assert error.id == old.id\nelse:\n    raise "
        "AssertionError('Old proxy resolved in new project')\nprint('Replacement invalidated old "
        "proxies')");
    require(
        areas.front()->grab().save(QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR "/script-panel.png")),
        "Panel capture saved");
    std::cout << "Script panel: UI transaction, undo, context, events, cancellation, replacement "
                 "and frame-12 byte parity passed\n";
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomTests");
    app.setApplicationName("ScriptPanel");
    ui::kit::installKinetikTheme(app);
    try {
        exercise();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
