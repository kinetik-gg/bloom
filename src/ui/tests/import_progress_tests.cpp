// Import progress (#377): the modal over every batch import, its threshold, cancellation with the
// keep-what-finished outcome and its undo, the summary with each skipped file's reason, folder
// and sequence imports, drops on Assets and on other panels, the selected-folder target, and
// proof that the work runs on a task thread while the interface keeps serving events.

#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include "import_progress_dialog.hpp"
#include "import_progress_test_support.hpp"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace bloom;
namespace assets = ui::assets;

void require(const bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <typename Predicate> bool waitUntil(Predicate predicate, const int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}
void writePng(const QString& path, const QColor& colour = Qt::white) {
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(colour);
    require(image.save(path), "generate PNG fixture");
}
void writeText(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}

struct Harness final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler};
    ui::AssetController controller{session, host, scheduler, bridge};
    QWidget window;
    ui::kit::KLineEdit* field = new ui::kit::KLineEdit(&window);
    ui::ImportProgressPresenter* presenter = new ui::ImportProgressPresenter(controller, window);
    QStringList notices;
    std::shared_ptr<ui::test::ImportGate> gate;

    Harness() {
        window.setWindowTitle(QStringLiteral("Bloom"));
        window.resize(640, 400);
        window.show();
        QObject::connect(presenter, &ui::ImportProgressPresenter::notice,
                         [this](const QString& message) { notices.push_back(message); });
    }
    ~Harness() {
        if (gate)
            gate->release();
        controller.setImportProgressHookForTesting({});
        waitUntil([this] { return !controller.busy(); });
        if (auto* dialog = presenter->dialog())
            dialog->dismiss();
    }
    void holdAt(const std::size_t index) { holdWhen(ui::test::itemStart(index)); }
    void holdWhen(ui::test::ImportReportPredicate at) {
        gate = std::make_shared<ui::test::ImportGate>(std::move(at));
        controller.setImportProgressHookForTesting(
            [held = gate](const commands::ImportProgress& state) { (*held)(state); });
    }
    [[nodiscard]] std::size_t assetCount() const {
        return session.snapshot().project().assets().size();
    }
    [[nodiscard]] ui::ImportProgressDialog* visibleDialog() const {
        auto* dialog = presenter->dialog();
        return dialog != nullptr && dialog->isVisible() ? dialog : nullptr;
    }
};

// While the worker is held mid-batch the dialog shows the overall bar, "n of N", the item in
// progress and Cancel, and advances as items complete; the interface thread keeps running.
void progressAdvancesAndStaysOffTheInterfaceThread(const QTemporaryDir& directory) {
    Harness harness;
    for (const auto* name : {"a.png", "b.png", "c.png"})
        writePng(directory.filePath(QStringLiteral("progress/%1").arg(name)));
    harness.holdAt(1);
    harness.controller.importFiles({directory.filePath("progress/a.png"),
                                    directory.filePath("progress/b.png"),
                                    directory.filePath("progress/c.png")});
    require(harness.controller.importing(), "batch import starts");
    require(waitUntil([&] { return harness.gate->reached(); }), "worker reaches the second item");
    // The worker is blocked, yet timers, painting and the dialog keep running here.
    require(waitUntil([&] { return harness.visibleDialog() != nullptr; }),
            "a held import shows the dialog after the threshold");
    auto* dialog = harness.visibleDialog();
    require(dialog->isModal() && dialog->windowModality() == Qt::ApplicationModal,
            "the import dialog blocks the window");
    require(waitUntil(
                [&] { return dialog->headingText() == QStringLiteral("Importing 2 of 3: b.png"); }),
            "the dialog counts and names the item in progress");
    require(dialog->detailText() == QStringLiteral("Reading file"), "a still reads as one file");
    require(dialog->progressBar()->completed() == 1 && dialog->progressBar()->total() == 3,
            "the bar shows one of three items done");
    require(dialog->cancelButton()->isVisible() && dialog->cancelButton()->isEnabled(),
            "Cancel is offered while importing");
    const auto progress = harness.controller.importProgress();
    require(progress.workerThread.has_value() &&
                *progress.workerThread != std::this_thread::get_id(),
            "the import is prepared on a task thread, not the interface thread");
    require(harness.assetCount() == 0, "nothing is added before the batch finishes");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "import finishes");
    require(harness.assetCount() == 3, "every file imports");
    require(waitUntil([&] { return harness.visibleDialog() == nullptr; }),
            "an import that added everything closes its dialog");
    require(harness.notices == QStringList{QStringLiteral("Imported 3 assets")},
            "a successful import still reports its outcome");
}

// Cancel keeps what finished as one undoable step and summarizes what was not imported.
void cancelKeepsWhatFinished(const QTemporaryDir& directory) {
    Harness harness;
    for (const auto* name : {"a.png", "b.png", "c.png"})
        writePng(directory.filePath(QStringLiteral("cancel/%1").arg(name)));
    harness.holdAt(1);
    harness.controller.importFiles({directory.filePath("cancel/a.png"),
                                    directory.filePath("cancel/b.png"),
                                    directory.filePath("cancel/c.png")});
    require(waitUntil([&] { return harness.visibleDialog() != nullptr; }), "dialog shows");
    auto* dialog = harness.visibleDialog();
    QTest::mouseClick(dialog->cancelButton(), Qt::LeftButton);
    require(dialog->headingText() == QStringLiteral("Cancelling…") &&
                !dialog->cancelButton()->isEnabled(),
            "Cancel acknowledges at once while the worker reaches a safe point");
    require(harness.controller.importProgress().stopping, "the controller records the stop");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "cancelled import finishes");
    require(harness.assetCount() == 1, "the item finished before Cancel is kept");
    require(waitUntil([&] { return harness.visibleDialog() && dialog->summarizing(); }),
            "a cancelled import shows its summary");
    require(dialog->headingText() == QStringLiteral("Import cancelled") &&
                dialog->detailText() == QStringLiteral("Imported 1 · Not imported 2"),
            "the summary counts what was kept and what was not");
    require(harness.notices.isEmpty(), "the summary, not a notice, reports a cancelled import");
    require(harness.session.undoLabel() == QStringLiteral("Import Assets"),
            "the kept items are one undoable step");
    require(harness.session.undo() && harness.assetCount() == 0, "one Undo removes the kept item");
    QTest::keyClick(dialog, Qt::Key_Return);
    require(waitUntil([&] { return harness.visibleDialog() == nullptr; }), "Return closes it");

    // Escape is Cancel while importing.
    harness.holdAt(1);
    harness.controller.importFiles(
        {directory.filePath("cancel/a.png"), directory.filePath("cancel/b.png")});
    require(waitUntil([&] { return harness.visibleDialog() != nullptr; }), "second dialog shows");
    QTest::keyClick(harness.visibleDialog(), Qt::Key_Escape);
    require(harness.controller.importProgress().stopping, "Escape requests the stop");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "escaped import finishes");
    require(harness.assetCount() == 1, "Escape keeps what finished too");
}

// Skipped files are listed with their plain reasons, and the summary appears even for a short
// import; a short import that added everything shows no dialog at all.
void summaryAndThreshold(const QTemporaryDir& directory) {
    Harness harness;
    writePng(directory.filePath("summary/good.png"));
    writeText(directory.filePath("summary/broken.png"), "not an image");
    writeText(directory.filePath("summary/odd.cube"), "not a lut");

    QElapsedTimer timer;
    timer.start();
    harness.controller.importFiles({directory.filePath("summary/good.png")});
    bool dialogSeen = false;
    require(waitUntil([&] {
                dialogSeen = dialogSeen || harness.visibleDialog() != nullptr;
                return !harness.controller.busy();
            }),
            "short import finishes");
    if (timer.elapsed() < ui::ImportProgressPresenter::kShowDelay.count())
        require(!dialogSeen, "an import shorter than the threshold never shows the dialog");
    QTest::qWait(static_cast<int>(ui::ImportProgressPresenter::kShowDelay.count()) + 50);
    require(harness.visibleDialog() == nullptr, "nothing appears after a quick success");
    require(harness.notices == QStringList{QStringLiteral("Imported 1 asset")},
            "a quick success reports in the status bar");

    harness.controller.importFiles({directory.filePath("summary/broken.png"),
                                    directory.filePath("summary/good.png"),
                                    directory.filePath("summary/odd.cube")});
    require(waitUntil([&] { return !harness.controller.busy(); }), "mixed import finishes");
    require(waitUntil([&] { return harness.visibleDialog() != nullptr; }),
            "a short import that skipped files still shows its summary");
    auto* dialog = harness.visibleDialog();
    require(dialog->summarizing() &&
                dialog->headingText() == QStringLiteral("Nothing was imported"),
            "nothing new was added: good.png is already in the project");
    require(dialog->detailText() == QStringLiteral("Imported 0 · Skipped 3"), "summary counts");
    QStringList files;
    QStringList reasons;
    for (auto* label : dialog->findChildren<ui::kit::KLabel*>(QStringLiteral("importSkippedFile")))
        files.push_back(label->text());
    for (auto* label :
         dialog->findChildren<ui::kit::KLabel*>(QStringLiteral("importSkippedReason")))
        reasons.push_back(label->text());
    require(files == QStringList({QStringLiteral("broken.png"), QStringLiteral("odd.cube"),
                                  QStringLiteral("good.png")}),
            "each skipped file is listed");
    require(reasons.size() == 3 && !reasons[0].isEmpty() &&
                reasons[1].startsWith(QStringLiteral("The LUT")) &&
                reasons[2] == QStringLiteral("Already in the project"),
            "each skipped file has its plain reason, with no internal identifier");
    require(dialog->closeButton()->isVisible() && !dialog->cancelButton()->isVisible(),
            "the summary offers Close");
    QTest::mouseClick(dialog->closeButton(), Qt::LeftButton);
    require(waitUntil([&] { return harness.visibleDialog() == nullptr; }), "Close closes it");
}

// Window input is held back during the threshold so a not-yet-shown import still blocks.
void inputIsHeldDuringTheThreshold(const QTemporaryDir& directory) {
    Harness harness;
    writePng(directory.filePath("hold/a.png"));
    writePng(directory.filePath("hold/b.png"));
    harness.holdAt(0);
    harness.field->setFocus();
    harness.controller.importFiles(
        {directory.filePath("hold/a.png"), directory.filePath("hold/b.png")});
    require(waitUntil([&] { return harness.gate->reached(); }), "worker held");
    require(harness.visibleDialog() == nullptr, "dialog not shown yet");
    QTest::keyClicks(harness.field, QStringLiteral("typed"));
    require(harness.field->text().isEmpty(),
            "typing during the threshold does not reach the window");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "held import finishes");
    QTest::keyClicks(harness.field, QStringLiteral("ok"));
    require(harness.field->text() == QStringLiteral("ok"), "input returns once the import ends");
}

// A folder contributes its media below it; a sequence counts as one item however many of its
// members are chosen; both land in the selected Assets folder.
void folderAndSequenceIntoSelectedFolder(const QTemporaryDir& directory) {
    Harness harness;
    QDir(directory.path()).mkpath("shoot/plates");
    writePng(directory.filePath("shoot/still.png"));
    for (int frame = 1; frame <= 3; ++frame)
        writePng(directory.filePath(
                     QStringLiteral("shoot/plates/Pulse.%1.png").arg(frame, 4, 10, QChar('0'))),
                 QColor(frame * 60, 40, 40));
    writeText(directory.filePath("shoot/notes.txt"), "not media");
    commands::Transaction create("Folders", harness.session.snapshot().revision());
    create.emplace<commands::CreateAssetFolder>("Footage");
    const auto folder = harness.session.executeTransaction(std::move(create))
                            .outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    require(folder.has_value(), "selected folder fixture");
    harness.controller.setImportFolder(folder);

    harness.holdAt(1);
    harness.controller.importFiles({directory.filePath("shoot")});
    require(waitUntil([&] { return harness.gate->reached(); }), "folder import reaches item 2");
    const auto total = harness.controller.importProgress().total;
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "folder import finishes");
    require(total == 2, "a folder with a still and a three-frame sequence is two items");
    const auto& project = harness.session.snapshot().project();
    require(project.assets().size() == 2, "the still and the sequence import");
    std::size_t sequences = 0;
    for (const auto& asset : project.assets()) {
        require(asset.folder == folder, "folder imports land in the selected Assets folder");
        if (asset.kind == document::AssetKind::Sequence && asset.manifest.members.size() == 3)
            ++sequences;
    }
    require(sequences == 1, "the numbered files import as one sequence");
    require(harness.notices.back() == QStringLiteral("Imported 2 assets"), "folder notice");

    // Choosing every member of a sequence imports it once, as one item.
    require(harness.session.undo() && harness.assetCount() == 0, "undo the folder import");
    harness.holdAt(0);
    harness.controller.importFiles({directory.filePath("shoot/plates/Pulse.0001.png"),
                                    directory.filePath("shoot/plates/Pulse.0002.png"),
                                    directory.filePath("shoot/plates/Pulse.0003.png")});
    require(waitUntil([&] { return harness.gate->reached(); }), "sequence import held");
    require(harness.controller.importProgress().total == 1, "three chosen members are one item");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "sequence import finishes");
    require(harness.assetCount() == 1 &&
                harness.session.snapshot().project().assets().front().kind ==
                    document::AssetKind::Sequence,
            "one sequence asset");
}

// Reading reports never move backwards, keep one work total, and end at 100%.
void requireMonotonicWork(const std::vector<commands::ImportProgress>& log,
                          const std::uint64_t workTotal) {
    using Phase = commands::ImportProgress::Phase;
    bool reading = false;
    std::uint64_t last = 0;
    for (const auto& state : log) {
        if (state.phase == Phase::Finding) {
            require(!reading, "nothing is found after reading starts");
            continue;
        }
        reading = true;
        require(state.workTotal == workTotal, "the work total stays fixed while reading");
        require(state.workDone >= last && state.workDone <= workTotal, "the bar never goes back");
        last = state.workDone;
    }
    require(reading && last == workTotal, "the worker reports 100% before the import commits");
}

// One long sequence is one item but many units of work: its frames are found first, with a busy
// bar, then read against a known total, so the bar moves inside the single item.
void longSequenceMovesTheBar(const QTemporaryDir& directory) {
    using Phase = commands::ImportProgress::Phase;
    Harness harness;
    constexpr int frames = 40;
    QDir(directory.path()).mkpath("long");
    for (int frame = 1; frame <= frames; ++frame)
        writePng(directory.filePath(
                     QStringLiteral("long/SC020_SH070_image_%1.png").arg(frame, 4, 10, QChar('0'))),
                 QColor(frame * 5, 40, 90));
    const auto first = directory.filePath("long/SC020_SH070_image_0001.png");

    // Discovery: the member count is not known yet.
    harness.holdWhen([](const commands::ImportProgress& state) {
        return state.phase == Phase::Finding && state.findingFrames && state.found == 25;
    });
    harness.controller.importFiles({first});
    require(waitUntil([&] { return harness.gate->reached() && harness.visibleDialog(); }),
            "discovery is held with the dialog shown");
    auto* dialog = harness.visibleDialog();
    require(waitUntil([&] { return dialog->headingText() == QStringLiteral("Finding frames…"); }),
            "discovery says it is finding frames");
    require(dialog->detailText() == QStringLiteral("SC020_SH070_image · 25 found"),
            "discovery names the sequence and counts what it found");
    require(dialog->progressBar()->busy() && dialog->progressBar()->sweeping(),
            "the bar is busy and animating while the total is unknown");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "sequence import finishes");
    require(harness.assetCount() == 1, "the sequence imports");
    const auto log = harness.gate->log();
    requireMonotonicWork(log, frames);
    std::set<std::uint64_t> steps;
    for (const auto& state : log)
        if (state.phase == Phase::Reading)
            steps.insert(state.workDone);
    require(steps.size() == frames + 1, "every frame read moves the bar");
    require(harness.session.undo() && harness.assetCount() == 0, "undo the sequence");

    // Reading: held mid-sequence with a known total.
    harness.holdWhen([](const commands::ImportProgress& state) {
        return state.phase == Phase::Reading && state.frames == frames && state.framesRead == 20;
    });
    harness.controller.importFiles({first});
    require(waitUntil([&] { return harness.gate->reached() && harness.visibleDialog(); }),
            "reading is held mid-sequence");
    dialog = harness.visibleDialog();
    require(
        waitUntil([&] { return dialog->detailText() == QStringLiteral("Reading frame 21 of 40"); }),
        "the dialog says which frame it is reading");
    require(dialog->headingText() == QStringLiteral("Importing SC020_SH070_image · sequence"),
            "a single item is named without a count");
    require(!dialog->progressBar()->busy() && dialog->progressBar()->completed() == 20 &&
                dialog->progressBar()->total() == frames,
            "the bar is half full inside the one sequence");
    require(harness.assetCount() == 0, "nothing commits mid-sequence");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "second import finishes");
    requireMonotonicWork(harness.gate->log(), frames);
}

// Work is summed across items: two stills and a ten-frame sequence are twelve units.
void mixedItemsWeighByWork(const QTemporaryDir& directory) {
    Harness harness;
    QDir(directory.path()).mkpath("mixed");
    writePng(directory.filePath("mixed/a.png"));
    writePng(directory.filePath("mixed/b.png"));
    for (int frame = 1; frame <= 10; ++frame)
        writePng(
            directory.filePath(QStringLiteral("mixed/Seq_%1.png").arg(frame, 4, 10, QChar('0'))));
    harness.holdAt(2);
    harness.controller.importFiles({directory.filePath("mixed/a.png"),
                                    directory.filePath("mixed/Seq_0001.png"),
                                    directory.filePath("mixed/b.png")});
    require(waitUntil([&] { return harness.gate->reached(); }), "held at the last still");
    const auto state = harness.controller.importProgress();
    require(!state.finding && state.total == 3 && state.completed == 2 && state.workTotal == 12 &&
                state.workDone == 11,
            "a still and a ten-frame sequence are eleven of twelve units");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "mixed import finishes");
    require(harness.assetCount() == 3, "both stills and the sequence import");
    requireMonotonicWork(harness.gate->log(), 12);
}

bool sendDrop(QWidget& target, const QMimeData& mime, const QPoint position) {
    QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &enter);
    if (!enter.isAccepted())
        return false;
    QDragMoveEvent move(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &move);
    if (!move.isAccepted())
        return false;
    QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &drop);
    QApplication::processEvents();
    return drop.isAccepted();
}

// File and folder drops go through the same import and dialog on Assets and on other panels.
void dropsImportWithProgress(const QTemporaryDir& directory) {
    Harness harness;
    QDir(directory.path()).mkpath("drops/folder");
    writePng(directory.filePath("drops/one.png"));
    writePng(directory.filePath("drops/folder/two.png"));
    writePng(directory.filePath("drops/folder/three.png"));
    commands::Transaction create("Folders", harness.session.snapshot().revision());
    create.emplace<commands::CreateAssetFolder>("Dropped");
    const auto folder = harness.session.executeTransaction(std::move(create))
                            .outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    require(folder.has_value(), "drop folder fixture");

    // Assets: a folder dropped onto the Dropped folder row.
    ui::AssetsEditor editor(harness.session);
    editor.resize(480, 320);
    editor.show();
    QApplication::processEvents();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    require(tree != nullptr, "assets tree");
    QTreeWidgetItem* row = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, assets::kFolderRole).toULongLong() == folder->value())
            row = *it;
    require(row != nullptr, "folder row");
    QMimeData folderDrop;
    folderDrop.setUrls({QUrl::fromLocalFile(directory.filePath("drops/folder"))});
    harness.holdAt(0);
    require(sendDrop(*tree->viewport(), folderDrop, tree->visualItemRect(row).center()),
            "a folder drops onto Assets");
    require(waitUntil([&] { return harness.visibleDialog() != nullptr; }),
            "a dropped folder shows the import dialog");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "folder drop finishes");
    require(harness.assetCount() == 2, "both files from the dropped folder import");
    for (const auto& asset : harness.session.snapshot().project().assets())
        require(asset.folder == folder, "a folder dropped on a folder row lands in it");

    // Another panel (Timeline and Nodes share this drop target): the selected Assets folder.
    harness.controller.setImportFolder(std::nullopt);
    QWidget panel(&harness.window);
    panel.resize(200, 100);
    panel.show();
    ui::installAssetDropTarget(panel, harness.session);
    QMimeData fileDrop;
    fileDrop.setUrls({QUrl::fromLocalFile(directory.filePath("drops/one.png"))});
    harness.holdAt(0);
    require(sendDrop(panel, fileDrop, {10, 10}), "a file drops onto another panel");
    require(waitUntil([&] { return harness.visibleDialog() != nullptr; }),
            "a panel drop shows the import dialog");
    harness.gate->release();
    require(waitUntil([&] { return !harness.controller.busy(); }), "panel drop finishes");
    require(harness.assetCount() == 3, "the dropped file imports");
    bool atRoot = false;
    for (const auto& asset : harness.session.snapshot().project().assets())
        atRoot = atRoot || (asset.locator.path.ends_with("one.png") && !asset.folder);
    require(atRoot, "a panel drop lands in the selected Assets folder (the root here)");
    QMimeData other;
    other.setText(QStringLiteral("not a file"));
    require(!sendDrop(panel, other, {10, 10}), "a non-file drop is still refused");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomImportProgressTests"));
    QApplication::setApplicationName(QStringLiteral("ImportProgress"));
    QStandardPaths::setTestModeEnabled(true);
    ui::kit::installKinetikTheme(app);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        for (const auto* sub : {"progress", "cancel", "summary", "hold"})
            require(QDir(directory.path()).mkpath(sub), "fixture folder");
        progressAdvancesAndStaysOffTheInterfaceThread(directory);
        cancelKeepsWhatFinished(directory);
        summaryAndThreshold(directory);
        inputIsHeldDuringTheThreshold(directory);
        folderAndSequenceIntoSelectedFolder(directory);
        dropsImportWithProgress(directory);
        longSequenceMovesTheBar(directory);
        mixedItemsWeighByWork(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
