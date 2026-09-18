#include "asset_drop.hpp"
#include "node_editor_add.hpp"
#include "node_editor_items.hpp"
#include "timeline_property_rows.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsView>
#include <QImage>
#include <QStyleOptionGraphicsItem>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUrl>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
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
void compositionDrops() {
    auto initial =
        document::makeNewProject("Composition drops", "Outer", core::RationalTime::fromInteger(10));
    const auto outer = initial.initialCompositionId;
    document::Document document(std::move(initial.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, outer);
    commands::Transaction create("Create nested fixture", session.snapshot().revision());
    create.emplace<commands::AddComposition>("Nested", document::CompositionFormat{},
                                             core::RationalTime::fromInteger(3));
    require(session.executeTransaction(std::move(create)).succeeded(), "create nested composition");
    document::CompositionId nested;
    for (const auto& composition : session.snapshot().project().compositions())
        if (composition.id() != outer)
            nested = composition.id();
    require(nested.isValid(), "nested fixture identity");
    ui::NodeGraphEditor nodes(session);
    nodes.show();
    QApplication::processEvents();
    auto* view = nodes.findChild<QGraphicsView*>();
    QMimeData mime;
    mime.setData(ui::kCompositionMimeType,
                 QByteArray::number(static_cast<qulonglong>(nested.value())));
    const auto before = session.composition()->graph().nodes().size();
    require(view && drop(*view->viewport(), mime), "composition canvas drop accepted");
    require(session.composition()->graph().nodes().size() == before + 1 &&
                session.composition()->graph().layerOutputs().empty(),
            "canvas creates only one composition source");
    document::NodeId source;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == document::kCompositionSourceNodeType) {
            source = node.id;
            require(ui::compositionSourceId(*session.composition(), node) == nested,
                    "drop preserves composition identity");
        }
    QApplication::processEvents();
    bool named = false;
    for (auto* item : view->scene()->items())
        if (const auto* card = dynamic_cast<ui::node_editor::NodeItem*>(item);
            card && card->id() == source)
            named = card->title() == "Nested";
    require(named, "composition node card displays the nested name");
    session.selectNode(source);
    ui::PropertiesEditor properties(session);
    properties.show();
    QApplication::processEvents();
    auto* picker = properties.findChild<ui::kit::KDropdown*>("propertiesCompositionSource");
    auto* open = properties.findChild<ui::kit::KButton*>("propertiesOpenComposition");
    require(picker && picker->currentText() == "Nested" && open && open->isEnabled(),
            "Properties names the source and offers Open");
    open->click();
    require(session.compositionId() == nested, "Open switches the active composition");
    require(session.setComposition(outer), "restore outer composition");
    require(session.undo(), "undo source drop");
    require(session.composition()->graph().nodes().size() == before, "source drop is one undo");
    QWidget timeline;
    ui::installAssetDropTarget(timeline, session);
    require(drop(timeline, mime), "composition timeline drop accepted");
    const auto* composition = session.composition();
    require(composition->graph().nodes().size() == before + 2 &&
                composition->graph().layerOutputs().size() == 1,
            "timeline creates source and one Layer");
    const auto layer = composition->graph().layerOutputs().front();
    require(layer.name == "Nested" &&
                layer.endPoint(composition->duration()) == core::RationalTime::fromInteger(3),
            "nested bar has the source name and duration");
    require(composition->graph().layerStack().entries().size() == 1,
            "image and audio share one Merge slot");
    require(session.snapshot().project().validate().ok(), "drop produces valid project");
    require(session.undo() && session.redo(), "nested layer undo and redo");
    require(session.setComposition(nested), "activate nested for reverse drop");
    mime.setData(ui::kCompositionMimeType,
                 QByteArray::number(static_cast<qulonglong>(outer.value())));
    const auto revision = session.snapshot().revision();
    require(!drop(timeline, mime) && session.snapshot().revision() == revision,
            "reverse drop refuses A to B to A without publication");
}
void run() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary media directory");
    QImage image(32, 16, QImage::Format_RGBA8888);
    image.fill(QColor(128, 64, 32, 128));
    for (const auto* name : {"still.png", "clip.0001.png", "clip.0002.png"}) {
        image.fill(QString(name).contains("0002") ? Qt::black : Qt::white);
        require(image.save(directory.filePath(name)), "generate PNG fixture");
    }
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
    mime.setData(ui::kAssetMimeType, ui::assetMimePayload(session, asset));
    auto* view = nodes.findChild<QGraphicsView*>();
    QString refused;
    QObject::connect(&session, &ui::CompositionSession::commandRejected, &nodes,
                     [&](const QString& message) { refused = message; });
    QMimeData compositionMime;
    compositionMime.setData(ui::kCompositionMimeType, "1");
    const auto nodeCount = session.composition()->graph().nodes().size();
    require(view && !drop(*view->viewport(), compositionMime) && !refused.isEmpty() &&
                session.composition()->graph().nodes().size() == nodeCount,
            "self-composition canvas drop is refused with a reason and no graph edit");
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
    auto* inputSpace = properties.findChild<ui::kit::KDropdown*>("propertiesImageInputColorSpace");
    require(inputSpace && inputSpace->count() > 1 &&
                inputSpace->itemText(0).startsWith("Auto (resolved: "),
            "Properties resolves Auto input colour space and lists config spaces");
    auto* dimensions = properties.findChild<ui::kit::KLabel*>("propertiesImageDimensions");
    require(dimensions && dimensions->text() == QString::fromUtf8("32 × 16"), "source dimensions");
    ui::node_editor::NodeItem* card = nullptr;
    for (auto* item : view->scene()->items())
        if (auto* candidate = dynamic_cast<ui::node_editor::NodeItem*>(item);
            candidate && candidate->id() == source)
            card = candidate;
    require(card, "Image card exists");
    ui::kit::KDropdown* cardAsset = nullptr;
    for (auto* child : card->childItems())
        if (auto* proxy = dynamic_cast<QGraphicsProxyWidget*>(child))
            if (auto* dropdown = proxy->widget()->findChild<ui::kit::KDropdown*>("nodeImageAsset"))
                cardAsset = dropdown;
    require(cardAsset && cardAsset->currentData() == selector->currentData(), "card asset picker");
    require(!cardAsset->itemIcon(cardAsset->currentIndex()).isNull(), "asset kind icon");
    require(card->title() ==
                ui::imageAssetDisplayName(*session.snapshot().project().findAsset(asset)),
            "source title derives from asset name");
    commands::Transaction renameAsset("Rename Asset", session.snapshot().revision());
    renameAsset.emplace<commands::RenameAsset>(asset, "Hero plate");
    require(session.executeTransaction(std::move(renameAsset)).changed(), "rename imported asset");
    require(card->title() == "Hero plate" &&
                properties.findChild<ui::kit::KDropdown*>("propertiesImageAsset")
                    ->currentText()
                    .startsWith("Hero plate"),
            "asset rename updates card title and Properties selector");
    commands::Transaction addNamedLayer("Add Layer", session.snapshot().revision());
    addNamedLayer.emplace<commands::AddImageLayer>(session.compositionId(), asset);
    require(session.executeTransaction(std::move(addNamedLayer)).changed(),
            "named image layer fixture");
    const auto layerId = session.composition()->graph().layerOutputs().back().layerId;
    require(ui::mediaLayerDisplayName(session, layerId) == "Hero plate",
            "Timeline reads asset display name for imported layer label");
    commands::Transaction customName("Rename Layer", session.snapshot().revision());
    customName.emplace<commands::RenameLayer>(session.compositionId(), layerId, "Artist layer");
    require(session.executeTransaction(std::move(customName)).changed() &&
                ui::mediaLayerDisplayName(session, layerId) == "Artist layer",
            "Timeline preserves an independently authored Layer name");
    require(session.undo() && session.undo(), "remove layer naming fixture");
    const auto other = session.snapshot().project().assets().back().id;
    cardAsset->setCurrentIndex(cardAsset->findData(QString::number(other.value())));
    require(selector->currentData().toString() == QString::number(other.value()),
            "card asset edit projects to Properties");
    require(session.undo(), "asset picker undo");
    require(cardAsset->currentData().toString() == QString::number(asset.value()),
            "asset picker undo restores stable id");
    const auto checkEnums = [](ui::kit::KDropdown* control, const QStringList& names) {
        require(control && control->count() == names.size(), "closed enum dropdown");
        for (int index = 0; index < names.size(); ++index)
            require(control->itemText(index) == names[index] &&
                        control->itemData(index).toLongLong() == index,
                    "enum display and stored value agree");
    };
    for (const auto& [role, names] : std::vector<std::pair<QString, QStringList>>{
             {"loopMode", {"Hold", "Loop", "Ping-pong"}},
             {"colorSpace", {"Auto", "sRGB", "Linear", "Raw"}}}) {
        ui::kit::KDropdown* control = nullptr;
        for (auto* child : card->childItems())
            if (auto* proxy = dynamic_cast<QGraphicsProxyWidget*>(child))
                for (auto* dropdown : proxy->widget()->findChildren<ui::kit::KDropdown*>())
                    if (dropdown->property("nodeParameterRole").toString() == role)
                        control = dropdown;
        checkEnums(control, names);
        const auto* const object =
            role == "loopMode" ? "propertiesImageLoopMode" : "propertiesImageColorSpace";
        checkEnums(properties.findChild<ui::kit::KDropdown*>(object), names);
    }
    const auto waitThumbnail = [&] {
        QElapsedTimer wait;
        wait.start();
        while (controller.nodeThumbnail(source).isNull() && wait.elapsed() < 15000)
            QTest::qWait(10);
        require(!controller.nodeThumbnail(source).isNull(), "node worker thumbnail arrives");
    };
    const auto renderCard = [&] {
        const auto rect = card->boundingRect();
        QImage pixels(rect.size().toSize(), QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::transparent);
        QPainter painter(&pixels);
        QStyleOptionGraphicsItem option;
        card->paint(&painter, &option, nullptr);
        return pixels;
    };
    waitThumbnail();
    const auto bright = renderCard();
    const int cx = bright.width() / 2;
    const int cy =
        ui::kit::px(ui::kit::Size::NodeTitleBand) + ui::kit::px(ui::kit::Size::ImageThumbnail) / 2;
    require(bright.pixelColor(cx, cy).lightness() >
                ui::kit::color(ui::kit::Color::SurfaceSunken).lightness() + 75,
            "decoded image is visibly brighter than the sunken thumbnail cell");
    document::AssetId sequence;
    for (const auto& record : session.snapshot().project().assets())
        if (record.kind == document::AssetKind::Sequence)
            sequence = record.id;
    cardAsset->setCurrentIndex(cardAsset->findData(QString::number(sequence.value())));
    waitThumbnail();
    ui::kit::KLabel* cardDimensions = nullptr;
    ui::kit::KLabel* cardRange = nullptr;
    for (auto* child : card->childItems())
        if (auto* proxy = dynamic_cast<QGraphicsProxyWidget*>(child)) {
            if (auto* label = proxy->widget()->findChild<ui::kit::KLabel*>("nodeImageDimensions"))
                cardDimensions = label;
            if (auto* label = proxy->widget()->findChild<ui::kit::KLabel*>("nodeImageRange"))
                cardRange = label;
        }
    require(cardDimensions && cardDimensions->text() == "32 × 16", "card dimensions readout");
    require(cardRange && cardRange->text() == "2 frames · 1–2", "card sequence range readout");
    const auto* range = properties.findChild<ui::kit::KLabel*>("propertiesImageRange");
    require(range && range->text() == cardRange->text(), "Properties and card range agree");
    const auto firstFrame = controller.nodeThumbnail(source);
    const auto nextFrame = core::RationalTime::create(1, 24);
    require(nextFrame && session.setCurrentTime(*nextFrame), "advance sequence time");
    waitThumbnail();
    require(controller.nodeThumbnail(source).pixelColor(0, 0).lightness() <
                firstFrame.pixelColor(0, 0).lightness() - 75,
            "sequence thumbnail follows session time");
    require(session.setCurrentTime(core::RationalTime::fromInteger(0)), "return sequence time");
    waitThumbnail();
    require(controller.nodeThumbnail(source).cacheKey() == firstFrame.cacheKey(),
            "returning to a frame reuses the content and frame proxy cache");
    const auto setSource = [&](std::string_view role, document::ParameterValue value) {
        const auto* parameter = ui::node_editor::parameterForRole(
            *session.composition()->graph().findNode(source), *session.composition(), role);
        require(parameter &&
                    session.setParameterValue(parameter->id, std::move(value), "Set source"),
                "source timing edit");
    };
    const auto frameTwo = core::RationalTime::create(2, 24);
    require(frameTwo && session.setCurrentTime(*frameTwo), "frame after sequence end");
    waitThumbnail();
    require(controller.nodeThumbnail(source).pixelColor(0, 0).lightness() < 75,
            "Hold clamps to last member");
    setSource("loopMode", std::int64_t{1});
    waitThumbnail();
    require(controller.nodeThumbnail(source).pixelColor(0, 0).lightness() > 180,
            "Loop wraps to first member");
    setSource("loopMode", std::int64_t{2});
    waitThumbnail();
    require(controller.nodeThumbnail(source).pixelColor(0, 0).lightness() > 180,
            "Ping-pong reverses to first member");
    setSource("startFrame", std::int64_t{3});
    waitThumbnail();
    require(controller.nodeThumbnail(source).pixelColor(0, 0).lightness() > 180,
            "time before Start Frame holds first member");
    require(session.undo() && session.undo() && session.undo(), "restore source timing parameters");
    require(session.setCurrentTime(core::RationalTime::fromInteger(0)), "restore sequence time");
    cardAsset->setCurrentIndex(cardAsset->findData(QString::number(asset.value())));
    waitThumbnail();
    // Timeline installs this exact typed target; its drop must create the Layer/Merge topology.
    QWidget timelineTarget;
    ui::installAssetDropTarget(timelineTarget, session);
    require(!drop(timelineTarget, mime), "stale asset drag is rejected after a revision change");
    mime.setData(ui::kAssetMimeType, ui::assetMimePayload(session, asset));
    QMimeData foreign;
    foreign.setData(ui::kAssetMimeType, "foreign-session:0:1");
    require(!drop(timelineTarget, foreign), "foreign-session asset drag is rejected");
    require(drop(timelineTarget, mime), "timeline asset drop");
    require(session.composition()->graph().layerOutputs().size() == 1, "drop creates a Layer");
    require(session.snapshot().project().validate().ok(), "drop preserves valid project graph");
    const auto layer = session.composition()->graph().layerOutputs().front().layerId;
    ui::TimelineLayerEntry entry;
    entry.layerId = layer;
    const auto rows = ui::timelinePropertyEntries(session, {entry}, {layer});
    int enumRows = 0;
    for (const auto& item : rows) {
        if (item.role != "loopMode" && item.role != "colorSpace")
            continue;
        ++enumRows;
        ui::TimelinePropertyRow row(session, nullptr);
        row.bind(item);
        auto* control = row.findChild<ui::kit::KDropdown*>("timelinePropertyAlignment");
        checkEnums(control, item.role == "loopMode" ? QStringList{"Hold", "Loop", "Ping-pong"}
                                                    : QStringList{"Auto", "sRGB", "Linear", "Raw"});
        control->setCurrentIndex(2);
        require(std::get<std::int64_t>(
                    std::get<document::ConstantValueSource>(
                        session.composition()->parameters().find(item.parameterId)->source)
                        .value) == 2,
                "timeline enum commits");
        require(session.undo(), "timeline enum undo");
    }
    require(enumRows == 2, "timeline exposes both image enum rows");
    controller.remove(asset);
    require(!session.snapshot().project().findAsset(asset), "Remove publishes asset removal");
    require(cardAsset->currentText() == "Missing asset" && cardAsset->mutedValue() &&
                cardAsset->toolTip().contains(QString::number(asset.value())),
            "missing card asset is muted with stable id in tooltip");
    require(selector->currentText() == "Missing asset" && selector->mutedValue(),
            "missing Properties asset remains explicit");
    const auto missingCard = renderCard();
    int glyphPixels = 0;
    for (int y = cy - 10; y <= cy + 10; ++y)
        for (int x = cx - 10; x <= cx + 10; ++x)
            if (missingCard.pixelColor(x, y).lightness() >
                ui::kit::color(ui::kit::Color::SurfaceSunken).lightness() + 25)
                ++glyphPixels;
    require(glyphPixels > 5, "missing thumbnail paints a visible warning glyph");
    require(session.undo(), "asset removal is undoable");
    document::AssetId still;
    for (const auto& record : session.snapshot().project().assets())
        if (record.kind == document::AssetKind::Image)
            still = record.id;
    cardAsset->setCurrentIndex(cardAsset->findData(QString::number(still.value())));
    waitThumbnail();
    require(QFile::remove(directory.filePath("still.png")), "remove fixture media file");
    require(nextFrame && session.setCurrentTime(*nextFrame), "refresh after file disappears");
    timer.restart();
    while (!controller.missing(still) && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(controller.missing(still) && controller.nodeThumbnail(source).isNull(),
            "unreadable asset never reuses a stale decoded proxy");
    const auto unreadable = renderCard();
    require(unreadable.pixelColor(cx, cy).lightness() < bright.pixelColor(cx, cy).lightness(),
            "unreadable source replaces the decoded pixels with the missing state");
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
        compositionDrops();
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
