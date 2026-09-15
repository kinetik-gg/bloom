#include "asset_drop.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QGraphicsView>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUrl>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <iostream>

namespace {
using namespace bloom;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool drop(QWidget& target, const QMimeData& mime) {
    QDragEnterEvent enter(QPoint(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &enter);
    if (!enter.isAccepted())
        return false;
    QDropEvent event(QPointF(50, 50), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &event);
    return event.isAccepted();
}
void run() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary media directory");
    QImage image(32, 16, QImage::Format_RGBA8888);
    image.fill(QColor(128, 64, 32, 128));
    for (const auto* name : {"still.png", "clip.0001.png", "clip.0002.png"})
        require(image.save(directory.filePath(name)), "generate PNG fixture");
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);
    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    ui::TaskUiBridge bridge(scheduler);
    ui::AssetController controller(session, host, scheduler, bridge);
    ui::AssetsEditor assets(session);
    assets.show();
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(directory.filePath("still.png")),
                   QUrl::fromLocalFile(directory.filePath("clip.0001.png"))});
    auto* tree = assets.findChild<QTreeWidget*>("assetsTree");
    require(tree && drop(*tree->viewport(), files), "files drop onto Assets");
    QElapsedTimer timer;
    timer.start();
    while (controller.busy() && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(session.snapshot().project().assets().size() == 2, "single atomic two-asset import");
    const auto asset = session.snapshot().project().assets().front().id;
    while (controller.thumbnail(asset).isNull() && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(!controller.thumbnail(asset).isNull(), "worker thumbnail arrives");
    require(tree->topLevelItemCount() == 3, "Assets projects two media rows and composition");
    ui::NodeGraphEditor nodes(session);
    nodes.show();
    QMimeData mime;
    mime.setData(ui::kAssetMimeType, QByteArray::number(session.snapshot().project().id().value()) +
                                         ':' + QByteArray::number(asset.value()));
    auto* view = nodes.findChild<QGraphicsView*>();
    require(view && drop(*view->viewport(), mime), "asset drop onto node canvas");
    document::NodeId source;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == "bloom.image-source")
            source = node.id;
    require(source.isValid(), "node drop creates Image source");
    session.selectNode(source);
    ui::PropertiesEditor properties(session);
    properties.show();
    QApplication::processEvents();
    auto* selector = properties.findChild<ui::kit::KDropdown*>("propertiesImageAsset");
    require(selector && selector->currentIndex() > 0, "asset selector reflects source binding");
    auto* dimensions = properties.findChild<ui::kit::KLabel*>("propertiesImageDimensions");
    require(dimensions && dimensions->text() == QString::fromUtf8("32 × 16"), "source dimensions");
    // Timeline installs this exact typed target; its drop must create the Layer/Merge topology.
    QWidget timelineTarget;
    ui::installAssetDropTarget(timelineTarget, session);
    require(drop(timelineTarget, mime), "timeline asset drop");
    require(session.composition()->graph().layerOutputs().size() == 1, "drop creates a Layer");
    require(session.snapshot().project().validate().ok(), "drop preserves valid project graph");
    controller.remove(asset);
    require(!session.snapshot().project().findAsset(asset), "Remove publishes asset removal");
    require(host.liveDocumentAndStack().second->undo().succeeded(), "asset removal is undoable");
    controller.cancel();
    bridge.beginShutdown();
    scheduler.beginShutdown();
    timer.restart();
    while (!scheduler.isQuiescent() && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(scheduler.isQuiescent(), "media tasks shut down safely");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
