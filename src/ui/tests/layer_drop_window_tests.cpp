// #380 in the whole window: the Assets selection header for a video (plain colour name, the
// assumed-colour note once, one duration format) and for a composition (its rendered frame), and
// After Effects style drags from Assets onto the Timeline (the row insertion line, the time
// marker, a sequenced multi-item drop). Set BLOOM_LAYER_DROP_CAPTURE_DIR to save each state, and
// BLOOM_LAYER_DROP_CAPTURE_SCREEN to a command that saves the screen to its argument (for example
// a compositor screenshot on a private headless session) to capture the window with its title.

#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include "window_fixture.hpp"

#include <QDir>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QImage>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_thumbnail_controller.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/thumbnail.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace bloom;

namespace {
void require(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename Predicate> bool waitUntil(Predicate predicate, const int timeoutMs = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(10);
    return predicate();
}
core::RationalTime frame(const std::int64_t index) {
    return *core::RationalTime::create(index, 24);
}

void writePng(const QString& path, const QColor& colour) {
    QImage image(480, 270, QImage::Format_RGBA8888);
    image.fill(colour);
    QPainter painter(&image);
    painter.setBrush(colour.lighter(150));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPoint(240, 135), 90, 90);
    painter.end();
    require(image.save(path), "write PNG fixture");
}

class Capture final {
  public:
    Capture()
        : directory_(qEnvironmentVariable("BLOOM_LAYER_DROP_CAPTURE_DIR")),
          screen_(qEnvironmentVariable("BLOOM_LAYER_DROP_CAPTURE_SCREEN")) {}
    void operator()(const QString& name, QWidget& window) const {
        if (directory_.isEmpty())
            return;
        // Let the panels repaint and the compositor catch up before the picture is taken.
        QTest::qWait(400);
        require(window.grab().save(QDir(directory_).filePath(name + QStringLiteral("-window.png"))),
                "save window capture");
        if (!screen_.isEmpty())
            require(QProcess::execute(screen_, {QDir(directory_).filePath(name + ".png")}) == 0,
                    "screen capture command");
    }

  private:
    QString directory_;
    QString screen_;
};

void selectInAssets(ui::AssetsEditor& assets, const QStringList& names) {
    auto* tree = assets.findChild<QTreeWidget*>("assetsTree");
    tree->clearSelection();
    for (const auto& name : names)
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == name) {
                tree->selectionModel()->select(tree->indexFromItem(*it),
                                               QItemSelectionModel::Select |
                                                   QItemSelectionModel::Rows);
                tree->setCurrentItem(*it, 0, QItemSelectionModel::NoUpdate);
            }
    QApplication::processEvents();
}

// Maximizes (or restores) the panel holding `editor`, as its header's maximize button does.
void toggleMaximized(QWidget& editor) {
    for (auto* widget = editor.parentWidget(); widget != nullptr; widget = widget->parentWidget())
        if (auto* area = qobject_cast<ui::EditorArea*>(widget)) {
            emit area->maximizeRequested(area);
            QApplication::processEvents();
            return;
        }
}

QString headerLine(ui::AssetsEditor& assets, const char* name) {
    auto* label = assets.findChild<ui::kit::KLabel*>(name);
    return label ? label->toolTip() : QString{};
}

void sendDrag(QWidget& target, const QEvent::Type type, const QPoint position,
              const QMimeData& mime, const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    if (type == QEvent::Drop) {
        QDropEvent event(QPointF(position), Qt::CopyAction, &mime, Qt::LeftButton, modifiers);
        QApplication::sendEvent(&target, &event);
        require(event.isAccepted(), "the Timeline takes the drop");
        return;
    }
    if (type == QEvent::DragEnter) {
        QDragEnterEvent event(position, Qt::CopyAction, &mime, Qt::LeftButton, modifiers);
        QApplication::sendEvent(&target, &event);
        require(event.isAccepted(), "the Timeline accepts the drag");
        return;
    }
    QDragMoveEvent event(position, Qt::CopyAction, &mime, Qt::LeftButton, modifiers);
    QApplication::sendEvent(&target, &event);
    require(event.isAccepted(), "the Timeline follows the drag");
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BloomLayerDropWindow"));
    app.setApplicationName(QStringLiteral("LayerDropWindow"));
    ui::kit::installKinetikTheme(app);
    std::filesystem::path fixtures;
    for (int index = 1; index + 1 < argc; ++index)
        if (std::string_view(argv[index]) == "--media-fixtures")
            fixtures = argv[index + 1];
    QTemporaryDir directory;
    require(directory.isValid(), "fixture directory");
    const auto file = [&](const QString& name) { return directory.filePath(name); };
    writePng(file("Plate.png"), QColor(65, 120, 200));
    for (int index = 1; index <= 24; ++index)
        writePng(file(QStringLiteral("Pulse_%1.png").arg(index, 4, 10, QChar('0'))),
                 QColor(40 + index * 6, 70, 140));
    for (int index = 1; index <= 12; ++index)
        writePng(file(QStringLiteral("Flare_%1.png").arg(index, 4, 10, QChar('0'))),
                 QColor(200, 110 + index * 6, 60));

    std::unique_ptr<ui::CompositionThumbnailController> thumbnails;
    document::CompositionId titleCard;
    ui::test::WindowFixture fixture([&](ui::test::WindowFixture& f) {
        auto& session = f.session;
        // The header's composition frames, as the application root installs them.
        thumbnails = std::make_unique<ui::CompositionThumbnailController>(
            session, f.scheduler, f.bridge,
            ui::makeCompositionPreviewPipeline(f.compiler, f.evaluator, f.display, f.qualified),
            std::size_t{512} << 20U);
        std::vector<std::filesystem::path> paths{file("Plate.png").toStdString(),
                                                 file("Pulse_0001.png").toStdString(),
                                                 file("Flare_0001.png").toStdString()};
        if (!fixtures.empty())
            paths.push_back(fixtures / "untagged-hd-prores.mov");
        commands::Transaction import("Import", session.snapshot().revision());
        import.emplace<commands::ImportAssets>(paths, directory.path().toStdString());
        require(session.executeTransaction(std::move(import)).succeeded(), "fixture import");
        const auto main = session.compositionId();
        commands::Transaction card("Title Card", session.snapshot().revision());
        card.emplace<commands::AddComposition>("Title Card", document::CompositionFormat{},
                                               core::RationalTime::fromInteger(3));
        const auto added = session.executeTransaction(std::move(card));
        titleCard = added.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                        .value_or(document::CompositionId{});
        require(session.setComposition(titleCard) &&
                    session.addSolidLayer("Card", {0.55, 0.12, 0.30, 1.0}) &&
                    session.addTextLayer("Title", "Title Card", 220.0) &&
                    session.setComposition(main),
                "title card composition");
        require(session.addSolidLayer("Background", {0.035, 0.045, 0.075, 1.0}) &&
                    session.addTextLayer("Caption", "Bloom", 120.0),
                "main composition layers");
    });
    // The controller goes before the session it registered with, whatever happens below.
    struct ResetThumbnails {
        std::unique_ptr<ui::CompositionThumbnailController>& controller;
        ~ResetThumbnails() { controller.reset(); }
    } resetThumbnails{thumbnails};
    const Capture capture;
    auto& session = fixture.session;
    auto* window = fixture.window.get();
    require(QTest::qWaitForWindowExposed(window), "window exposed");
    auto* assets = window->findChild<ui::AssetsEditor*>();
    auto* timeline = window->findChild<ui::TimelineEditor*>();
    require(assets != nullptr && timeline != nullptr, "Assets and Timeline panels");
    const auto& project = [&]() -> const document::Project& {
        return session.snapshot().project();
    };
    const auto assetName = [&](const std::string& part) {
        for (const auto& asset : project().assets())
            if (asset.locator.path.find(part) != std::string::npos)
                return QString::fromStdString(asset.name);
        return QString{};
    };
    auto* thumbnail = assets->findChild<ui::kit::KThumbnail*>("assetsSelectionThumbnail");

    // The header shots show the Assets panel maximized, so its lines are not cut short.
    toggleMaximized(*assets);
    // A video's header: its plain colour-space name, the assumed-colour note once, one duration.
    if (!fixtures.empty()) {
        const auto video = assetName("untagged-hd-prores");
        selectInAssets(*assets, {video});
        const auto* controller = session.assetController();
        const auto id = [&] {
            for (const auto& asset : project().assets())
                if (QString::fromStdString(asset.name) == video)
                    return asset.id;
            return document::AssetId{};
        }();
        require(waitUntil([&] { return !controller->thumbnail(id).isNull(); }), "video thumbnail");
        selectInAssets(*assets, {video});
        const auto colour = headerLine(*assets, "assetsSelectionColour");
        std::cout << "video header: " << headerLine(*assets, "assetsSelectionKind").toStdString()
                  << " | " << colour.toStdString() << '\n';
        require(colour.count(QStringLiteral("assumed")) == 1 && !colour.contains("lin_rec709") &&
                    !colour.contains("srgb_rec709"),
                "the video header names its colour plainly and states the note once");
        require(headerLine(*assets, "assetsSelectionKind").contains(" frames), "),
                "the video header shows timecode with the frame count");
        capture(QStringLiteral("01-video-header"), *window);
    }

    // A composition's header: its rendered frame.
    selectInAssets(*assets, {QStringLiteral("Title Card")});
    require(waitUntil([&] {
                QApplication::processEvents();
                const auto image = thumbnail->pixmap().toImage();
                return thumbnails->current(titleCard) && !image.isNull() &&
                       image.pixelColor(image.width() / 2, image.height() / 4).red() > 100;
            }),
            "the composition header shows its rendered frame");
    capture(QStringLiteral("02-composition-header"), *window);
    toggleMaximized(*assets);

    // Dragging over the layer stack: the insertion line above the row under the pointer.
    auto* stack = timeline->layerStackForTest();
    auto* lanes = timeline->laneRegionForTest();
    const auto layersBefore = stack->entries().size();
    const auto drag = [&](const QStringList& names) {
        selectInAssets(*assets, names);
        return std::unique_ptr<QMimeData>(
            static_cast<ui::assets::AssetTree*>(assets->findChild<QTreeWidget*>("assetsTree"))
                ->selectionMimeDataForTest());
    };
    const QStringList items{assetName("Pulse"), assetName("Flare"), QStringLiteral("Title Card")};
    auto mime = drag(items);
    require(mime->hasFormat(ui::kLayerItemsMimeType), "an Assets drag carries its items");
    int lastLayerRow = 0;
    for (int row = 0; row < stack->rowCount(); ++row)
        if (stack->entries()[static_cast<std::size_t>(row)].rowKind ==
            ui::TimelineLayerEntry::Kind::Layer)
            lastLayerRow = row;
    const auto rowPoint =
        stack->mapTo(timeline, QPoint(stack->width() / 3, stack->rowTop(lastLayerRow) + 4));
    sendDrag(*timeline, QEvent::DragEnter, rowPoint, *mime);
    sendDrag(*timeline, QEvent::DragMove, rowPoint, *mime);
    auto* indicator = stack->findChild<QWidget*>("timelineLayerInsertionIndicator");
    require(indicator != nullptr && indicator->isVisible(), "the row insertion line shows");
    capture(QStringLiteral("03-timeline-row-insertion"), *window);

    // Over the lanes: the time marker at the frame under the pointer.
    const auto axis = timeline->rulerForTest()->axisForWidth(lanes->width());
    require(axis.has_value(), "timeline axis");
    const auto x = static_cast<int>(std::lround(axis->pixelForTime(frame(48)) + 1.0));
    const auto lanePoint = lanes->mapTo(timeline, QPoint(x, lanes->rowTop(0) + 4));
    sendDrag(*timeline, QEvent::DragMove, lanePoint, *mime);
    require(lanes->dropMarkerTimeForTest() == frame(48), "the time marker sits on frame 48");
    auto* marker = lanes->findChild<QWidget*>("timelineDropTimeMarker");
    require(marker != nullptr && marker->isVisible(), "the time marker is shown above the rows");
    capture(QStringLiteral("04-timeline-time-marker"), *window);

    // Alt-drop the three items: end to end from frame 48, in selection order, one undo step.
    sendDrag(*timeline, QEvent::Drop, lanePoint, *mime, Qt::AltModifier);
    QApplication::processEvents();
    const auto& layers = session.composition()->graph().layerStack().entries();
    require(stack->entries().size() > layersBefore && layers.size() == 5, "three layers are added");
    const auto* first = session.composition()->graph().findLayer(layers[0].layerId);
    const auto* second = session.composition()->graph().findLayer(layers[1].layerId);
    require(first->inPoint == frame(48) && first->outPoint == frame(72) &&
                second->inPoint == frame(72) && second->outPoint == frame(84),
            "the sequenced layers run end to end from frame 48");
    // The drop selected its three layers, the uppermost primary, so Properties shows it.
    const auto* primary = std::get_if<document::LayerId>(&session.selection().primary);
    require(primary != nullptr && *primary == layers[0].layerId &&
                session.selectedNodes().size() == 3,
            "the dropped layers are selected");
    QApplication::processEvents();
    capture(QStringLiteral("05-multi-item-drop"), *window);
    require(session.undo() && session.composition()->graph().layerStack().entries().size() == 2,
            "one undo removes the whole drop");
    std::cout << "PASS: layer drop window\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
