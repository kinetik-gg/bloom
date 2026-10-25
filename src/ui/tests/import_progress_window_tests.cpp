// Import progress over the real main window (#377): MainWindow installs the presenter, so imports
// and drops on Timeline show the modal over it, a cancelled or partial import ends in a summary,
// and an import that added everything reports in the status bar. With BLOOM_IMPORT_CAPTURE_DIR set
// it saves each state as a PNG; BLOOM_IMPORT_CAPTURE_SCREEN names a command that is given the
// destination path to capture the whole screen, window titles included.

#include "import_progress_dialog.hpp"
#include "import_progress_test_support.hpp"
#include "window_fixture.hpp"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {
using namespace bloom;

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename Predicate> bool waitUntil(Predicate predicate, const int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}
void writePng(const QString& path, const QColor& colour) {
    QImage image(320, 180, QImage::Format_RGBA8888);
    image.fill(colour);
    QPainter painter(&image);
    painter.setBrush(colour.lighter(160));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPoint(160, 90), 60, 60);
    painter.end();
    require(image.save(path), "generate PNG fixture");
}

class Capture final {
  public:
    Capture()
        : directory_(qEnvironmentVariable("BLOOM_IMPORT_CAPTURE_DIR")),
          screen_(qEnvironmentVariable("BLOOM_IMPORT_CAPTURE_SCREEN")) {}
    void operator()(const QString& name, QWidget& widget) const {
        if (directory_.isEmpty())
            return;
        // Let the dialog's refresh and the compositor catch up before the picture is taken.
        QTest::qWait(250);
        require(widget.grab().save(QDir(directory_).filePath(name + QStringLiteral(".png"))),
                "save widget capture");
        if (!screen_.isEmpty())
            require(QProcess::execute(
                        screen_,
                        {QDir(directory_).filePath(name + QStringLiteral("-screen.png"))}) == 0,
                    "screen capture command");
    }

  private:
    QString directory_;
    QString screen_;
};

ui::ImportProgressDialog* visibleDialog(ui::test::WindowFixture& fixture) {
    for (auto* dialog : fixture.window->findChildren<ui::ImportProgressDialog*>())
        if (dialog->isVisible())
            return dialog;
    return nullptr;
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BloomImportProgressWindow"));
    app.setApplicationName(QStringLiteral("ImportProgressWindow"));
    ui::kit::installKinetikTheme(app);
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const auto file = [&](const QString& name) { return directory.filePath(name); };
    QDir(directory.path()).mkpath(QStringLiteral("More"));
    writePng(file("Plate_A.png"), QColor(65, 120, 200));
    writePng(file("Plate_B.png"), QColor(200, 120, 65));
    writePng(file("Plate_C.png"), QColor(90, 170, 110));
    for (int frame = 1; frame <= 12; ++frame)
        writePng(file(QStringLiteral("Pulse.%1.png").arg(frame, 4, 10, QChar('0'))),
                 QColor(40 + frame * 12, 60, 140));
    writePng(file("More/Sky.png"), QColor(120, 160, 230));
    writePng(file("More/Ground.png"), QColor(110, 90, 60));
    {
        QFile broken(file("Broken.png"));
        require(broken.open(QIODevice::WriteOnly) && broken.write("not an image") > 0, "broken");
        QFile lut(file("Grade.cube"));
        require(lut.open(QIODevice::WriteOnly) && lut.write("not a lut") > 0, "lut");
    }

    ui::test::WindowFixture fixture;
    fixture.window->show();
    require(QTest::qWaitForWindowExposed(fixture.window.get()), "window exposed");
    QTest::qWait(200);
    auto& assets = *fixture.assets;
    const auto capture = Capture{};
    std::shared_ptr<ui::test::ImportGate> gate;
    const auto holdWhen = [&](ui::test::ImportReportPredicate at) {
        gate = std::make_shared<ui::test::ImportGate>(std::move(at));
        assets.setImportProgressHookForTesting(
            [held = gate](const commands::ImportProgress& state) { (*held)(state); });
    };
    const auto holdAt = [&](const std::size_t index) { holdWhen(ui::test::itemStart(index)); };
    struct Release final {
        std::shared_ptr<ui::test::ImportGate>& gate;
        ~Release() {
            if (gate)
                gate->release();
        }
    } releaseOnExit{gate};
    const auto baseAssets = fixture.session.snapshot().project().assets().size();
    const auto assetCount = [&] {
        return fixture.session.snapshot().project().assets().size() - baseAssets;
    };

    // Mid-import over the main window.
    holdAt(2);
    assets.importFiles(
        {file("Plate_A.png"), file("Plate_B.png"), file("Pulse.0001.png"), file("Plate_C.png")});
    require(waitUntil([&] { return gate->reached() && visibleDialog(fixture) != nullptr; }),
            "MainWindow shows the import dialog");
    auto* dialog = visibleDialog(fixture);
    require(dialog->parentWidget() == fixture.window.get() && dialog->isModal(),
            "the dialog is modal over the main window");
    require(waitUntil([&] {
                return dialog->headingText() ==
                       QStringLiteral("Importing 3 of 4: Pulse · sequence");
            }),
            "progress over the main window names the sequence");
    capture(QStringLiteral("01-import-progress"), *dialog);

    // Cancel: acknowledged at once, then the cancelled summary.
    QTest::mouseClick(dialog->cancelButton(), Qt::LeftButton);
    capture(QStringLiteral("02-import-cancelling"), *dialog);
    gate->release();
    require(waitUntil([&] { return !assets.busy() && dialog->summarizing(); }),
            "cancelled summary");
    require(assetCount() == 2 &&
                dialog->detailText() == QStringLiteral("Imported 2 · Not imported 2"),
            "Cancel keeps the two finished items");
    capture(QStringLiteral("03-import-cancelled"), *dialog);
    QTest::mouseClick(dialog->closeButton(), Qt::LeftButton);
    require(waitUntil([&] { return visibleDialog(fixture) == nullptr; }), "summary closes");
    require(fixture.session.undo() && assetCount() == 0, "one Undo removes what was kept");

    // A partial import: the summary lists each skipped file and its reason.
    holdAt(0);
    assets.importFiles(
        {file("Broken.png"), file("Plate_A.png"), file("Grade.cube"), file("Pulse.0001.png")});
    require(waitUntil([&] { return gate->reached() && visibleDialog(fixture) != nullptr; }),
            "partial import dialog");
    dialog = visibleDialog(fixture);
    gate->release();
    require(waitUntil([&] { return !assets.busy() && dialog->summarizing(); }), "partial summary");
    require(assetCount() == 2 && dialog->headingText() == QStringLiteral("Import finished") &&
                dialog->detailText() == QStringLiteral("Imported 2 · Skipped 2"),
            "the partial summary counts imported and skipped files");
    capture(QStringLiteral("04-import-summary-skipped"), *dialog);
    QTest::keyClick(dialog, Qt::Key_Escape);
    require(waitUntil([&] { return visibleDialog(fixture) == nullptr; }), "Escape closes it");

    // A folder dropped on the Timeline that imports everything: the dialog closes on its own,
    // the imported files become layers (#380) and the status bar reports both.
    auto* timeline = fixture.window->findChild<ui::TimelineEditor*>();
    require(timeline != nullptr, "timeline");
    const auto layersBefore = fixture.session.composition()->graph().layerOutputs().size();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(file("More"))});
    holdAt(0);
    const QPoint point(timeline->width() / 2, timeline->height() / 2);
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(timeline, &enter);
    require(enter.isAccepted(), "the Timeline accepts a dropped folder");
    QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(timeline, &drop);
    require(drop.isAccepted(), "the Timeline takes the folder");
    require(waitUntil([&] { return gate->reached() && visibleDialog(fixture) != nullptr; }),
            "a Timeline drop shows the import dialog");
    gate->release();
    require(waitUntil([&] { return !assets.busy() && visibleDialog(fixture) == nullptr; }),
            "an import that added everything closes its dialog");
    require(assetCount() == 4, "both dropped files import");
    require(fixture.session.composition()->graph().layerOutputs().size() == layersBefore + 2,
            "the dropped files become layers once the import commits");
    require(fixture.window->statusStrip()->messageTextForTest() ==
                QStringLiteral("Imported 2 assets · Added 2 layers"),
            "the status bar reports the import and the layers it added");
    capture(QStringLiteral("05-import-success-status"), *fixture.window);

    // One long sequence: discovery with a busy bar, then reading frame by frame.
    using Phase = commands::ImportProgress::Phase;
    QDir(directory.path()).mkpath(QStringLiteral("Shot"));
    constexpr int shotFrames = 60;
    for (int frame = 1; frame <= shotFrames; ++frame)
        writePng(
            file(QStringLiteral("Shot/SC020_SH070_image_%1.png").arg(frame, 4, 10, QChar('0'))),
            QColor(30 + frame * 3, 70, 120));
    const auto shot = file("Shot/SC020_SH070_image_0001.png");
    holdWhen([](const commands::ImportProgress& state) {
        return state.phase == Phase::Finding && state.findingFrames && state.found == 37;
    });
    assets.importFiles({shot});
    require(waitUntil([&] { return gate->reached() && visibleDialog(fixture) != nullptr; }),
            "discovery dialog");
    dialog = visibleDialog(fixture);
    require(waitUntil([&] { return dialog->headingText() == QStringLiteral("Finding frames…"); }),
            "discovery over the main window");
    capture(QStringLiteral("06-import-finding-frames"), *dialog);
    gate->release();
    require(waitUntil([&] { return !assets.busy(); }), "sequence import finishes");
    require(fixture.session.undo(), "undo the sequence");
    holdWhen([](const commands::ImportProgress& state) {
        return state.phase == Phase::Reading && state.frames == shotFrames &&
               state.framesRead == 24;
    });
    assets.importFiles({shot});
    require(waitUntil([&] { return gate->reached() && visibleDialog(fixture) != nullptr; }),
            "reading dialog");
    dialog = visibleDialog(fixture);
    require(
        waitUntil([&] { return dialog->detailText() == QStringLiteral("Reading frame 25 of 60"); }),
        "reading a sequence over the main window");
    capture(QStringLiteral("07-import-reading-sequence"), *dialog);
    gate->release();
    require(waitUntil([&] { return !assets.busy(); }), "second sequence import finishes");
    assets.setImportProgressHookForTesting({});
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
