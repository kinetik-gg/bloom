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
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QStyleOptionGraphicsItem>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUrl>
#include <atomic>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/platform/source_identity.hpp>
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
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000)
        QTest::qWait(1);
    return predicate();
}
class CatalogWorkerHold final {
  public:
    explicit CatalogWorkerHold(runtime::TaskScheduler& scheduler)
        : submission_(scheduler.submit<void>(
              runtime::TaskRequest("Hold input color catalogue worker",
                                   {.kind = runtime::TaskOwnerKind::Application,
                                    .id = runtime::TaskOwnerId::fromRaw(999)},
                                   runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu),
              [this](runtime::TaskContext&) {
                  entered_ = true;
                  while (!released_)
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  return runtime::TaskResult<void>::succeeded();
              })) {}
    ~CatalogWorkerHold() { release(); }
    [[nodiscard]] bool entered() const { return submission_.accepted() && entered_; }
    void release() {
        if (released_.exchange(true))
            return;
        if (submission_.accepted())
            while (!submission_.handle.tryTakeResult())
                QTest::qWait(1);
    }

  private:
    runtime::TaskSubmission<void> submission_;
    std::atomic<bool> entered_{false};
    std::atomic<bool> released_{false};
};
void inputColorCatalogFollowsWorkingSpace() {
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.rowBandWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    ui::ProjectHost host(scheduler);
    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    commands::Transaction seed("Seed color catalogue composition", session.snapshot().revision());
    seed.emplace<commands::AddComposition>("Main", document::CompositionFormat{},
                                           core::RationalTime::fromInteger(10));
    const auto seeded = session.executeTransaction(std::move(seed));
    const auto id = seeded.succeeded()
                        ? seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                        : std::nullopt;
    require(id && session.setComposition(*id), "catalogue fixture composition");

    auto aces = *host.colorSettings();
    const auto revision = color::ocioBuiltInContentRevision(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri);
    require(revision.has_value(), "ACES catalogue revision resolves");
    aces.ocioConfig.locator =
        document::BuiltInOcioConfigLocator{std::string(color::kAcesCgV1ConfigUri)};
    aces.ocioConfig.expectedRevision.digest = *revision;
    aces.processColorSpaceId = "ACEScg";
    require(host.setColorSettings(aces) == bloom::host::ProjectSessionColorSettingsStatus::Updated,
            "ACES catalogue settings apply to the project");
    session.setColorSettings(aces);
    ui::TaskUiBridge bridge(scheduler);
    ui::AssetController controller(session, host, scheduler, bridge);

    {
        CatalogWorkerHold hold(scheduler);
        require(waitUntil([&] { return hold.entered(); }), "catalogue worker barrier starts");
        controller.requestInputColorSpaces();
        require(controller.inputColorSpaces() == nullptr, "initial catalogue request is pending");
        require(session.setWorkingColorSpaceOverride(std::string("Linear P3-D65")),
                "pending catalogue working space changes");
        require(controller.inputColorSpaces() == nullptr,
                "old pending catalogue is not exposed after working-space change");
        hold.release();
    }
    require(waitUntil([&] {
                const auto* spaces = controller.inputColorSpaces();
                return spaces && !spaces->empty();
            }),
            "replacement working-space catalogue completes after old pending request is retired");

    {
        CatalogWorkerHold hold(scheduler);
        require(waitUntil([&] { return hold.entered(); }),
                "cached catalogue worker barrier starts");
        require(session.setWorkingColorSpaceOverride(std::nullopt),
                "cached catalogue working-space override clears");
        require(controller.inputColorSpaces() == nullptr,
                "cached choices are invalidated synchronously on working-space change");
        hold.release();
    }
    require(waitUntil([&] {
                const auto* spaces = controller.inputColorSpaces();
                return spaces && !spaces->empty();
            }),
            "catalogue prepares again after cached working-space change");
    const auto* stable = controller.inputColorSpaces();
    require(session.addSolidLayer("Unrelated", {0.1, 0.2, 0.3, 1.0}),
            "unrelated authoring edit succeeds");
    require(controller.inputColorSpaces() == stable,
            "unrelated snapshot keeps the ready catalogue without resubmission");
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
// A batch with one unreadable file imports the valid ones in one undoable step and reports the
// skipped file with its own reason.
void partialBatchImport() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary partial-import directory");
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    require(image.save(directory.filePath("valid.png")), "generate valid PNG");
    {
        QFile corrupt(directory.filePath("corrupt.png"));
        require(corrupt.open(QIODevice::WriteOnly) && corrupt.write("not an image") > 0,
                "write corrupt PNG");
    }
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);
    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    ui::TaskUiBridge bridge(scheduler);
    ui::AssetController controller(session, host, scheduler, bridge);
    ui::AssetsEditor assets(session);
    assets.show();
    QStringList notices;
    QObject::connect(&controller, &ui::AssetController::diagnostic, &assets,
                     [&](const QString& message) { notices.push_back(message); });
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(directory.filePath("corrupt.png")),
                   QUrl::fromLocalFile(directory.filePath("valid.png"))});
    auto* tree = assets.findChild<QTreeWidget*>("assetsTree");
    require(tree && drop(*tree->viewport(), files), "mixed files drop onto Assets");
    require(waitUntil([&] { return !controller.busy(); }), "partial import completes");
    require(session.snapshot().project().assets().size() == 1,
            "the valid file imports despite the corrupt one");
    require(notices.size() == 1 && notices.front().contains("corrupt.png"),
            "the skipped file is reported with its own reason");
    require(session.undo() && session.snapshot().project().assets().empty(),
            "the successful files form one undoable import");
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
    // Explicit seed opt-in: application startup is now blank (no composition), but this fixture
    // drops assets onto a node canvas and exercises the timeline, so it authors the one composition
    // it needs through the ordinary command path.
    if (session.composition() == nullptr) {
        commands::Transaction seed("Seed media fixture composition", session.snapshot().revision());
        seed.emplace<commands::AddComposition>("Main", document::CompositionFormat{},
                                               core::RationalTime::fromInteger(10));
        const auto seeded = session.executeTransaction(std::move(seed));
        const auto seededId =
            seeded.succeeded()
                ? seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                : std::nullopt;
        require(seededId.has_value() && session.setComposition(*seededId),
                "media fixture composition seed");
    }
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
            "Properties resolves Auto input color space and lists config spaces");
    auto* dimensions = properties.findChild<ui::kit::KLabel*>("propertiesImageDimensions");
    require(dimensions && dimensions->text() == QString::fromUtf8("32 × 16"), "source dimensions");
    // ADR 0026: the part and layer pickers read the probe through the Assets worker.
    ui::kit::KDropdown* layerPicker = nullptr;
    QElapsedTimer partsTimer;
    partsTimer.start();
    while (partsTimer.elapsed() < 15000) {
        QApplication::processEvents();
        layerPicker = properties.findChild<ui::kit::KDropdown*>("propertiesImageLayer");
        if (layerPicker != nullptr && layerPicker->toolTip().isEmpty() && layerPicker->count() > 0)
            break;
        QTest::qWait(10);
    }
    require(layerPicker && layerPicker->count() == 1 &&
                layerPicker->itemText(0) == "Color (root)",
            "the layer picker lists exactly the still's root color layer");
    auto* partPicker = properties.findChild<ui::kit::KDropdown*>("propertiesImagePart");
    require(partPicker && !partPicker->isEnabled() &&
                partPicker->itemText(partPicker->currentIndex()) == "First part",
            "a still has one part");
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
    cardAsset->chooseIndex(cardAsset->findData(QString::number(other.value())));
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
    // Task N1, item 2: a source card carries no preview, so the worker only prepares the Assets
    // panel's own thumbnails; waiting means waiting for that request to settle.
    const auto waitThumbnail = [&] {
        QElapsedTimer wait;
        wait.start();
        while (controller.thumbnailsPending() && wait.elapsed() < 15000)
            QTest::qWait(10);
        require(!controller.thumbnailsPending(), "latest thumbnail request completes");
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
    // Sources were hashed once at import; later refreshes verify by file identity only.
    const auto verifiedReads = platform::sourceReadWork().fullReads;
    // The header's trailing badge: where a source card says its asset cannot be read.
    const auto warningBadge = [&](const QImage& pixels) {
        const int side = ui::kit::px(ui::kit::Size::IconSmall);
        const int right = static_cast<int>(card->cardRect().width()) -
                          ui::kit::px(ui::kit::Spacing::S);
        const int top = (ui::kit::px(ui::kit::Size::NodeTitleBand) - side) / 2;
        int warm = 0;
        for (int y = top; y < top + side; ++y)
            for (int x = right - side; x < right; ++x) {
                const auto pixel = pixels.pixelColor(x, y);
                if (pixel.red() > pixel.blue() + 60)
                    ++warm;
            }
        return warm;
    };
    require(!card->assetProblem() && warningBadge(renderCard()) == 0,
            "a readable source shows no warning badge");
    document::AssetId sequence;
    for (const auto& record : session.snapshot().project().assets())
        if (record.kind == document::AssetKind::Sequence)
            sequence = record.id;
    cardAsset->chooseIndex(cardAsset->findData(QString::number(sequence.value())));
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
    const QPersistentModelIndex stableAssetRow(tree->model()->index(0, 0));
    QSignalSpy assetChanges(&controller, &ui::AssetController::changed);
    const auto nextFrame = core::RationalTime::create(1, 24);
    require(nextFrame && session.setCurrentTime(*nextFrame), "advance sequence time");
    waitThumbnail();
    require(platform::sourceReadWork().fullReads == verifiedReads,
            "edits, asset rebinding and time changes re-read no verified source");
    for (int i = 0; i < 100; ++i)
        static_cast<void>(session.setCurrentTime(i % 2 ? core::RationalTime{} : *nextFrame));
    waitThumbnail();
    require(assetChanges.empty() && stableAssetRow.isValid(),
            "time changes decode no node preview, preserve Assets rows and emit no asset change");
    const auto setSource = [&](std::string_view role, document::ParameterValue value) {
        const auto* parameter = ui::node_editor::parameterForRole(
            *session.composition()->graph().findNode(source), *session.composition(), role);
        require(parameter &&
                    session.setParameterValue(parameter->id, std::move(value), "Set source"),
                "source timing edit");
    };
    setSource("loopMode", std::int64_t{1});
    setSource("loopMode", std::int64_t{2});
    setSource("startFrame", std::int64_t{3});
    require(session.undo() && session.undo() && session.undo(), "restore source timing parameters");
    cardAsset->chooseIndex(cardAsset->findData(QString::number(asset.value())));
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
        control->chooseIndex(2);
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
    // The timeline drop selects the Layer it creates (ADR 0030); show the source again.
    session.selectNode(source);
    QApplication::processEvents();
    selector = properties.findChild<ui::kit::KDropdown*>("propertiesImageAsset");
    require(selector && selector->currentText() == "Missing asset" && selector->mutedValue(),
            "missing Properties asset remains explicit");
    require(card->assetProblem() && warningBadge(renderCard()) > 5,
            "a missing asset paints a warning badge in the source card's header");
    require(session.undo(), "asset removal is undoable");
    document::AssetId still;
    for (const auto& record : session.snapshot().project().assets())
        if (record.kind == document::AssetKind::Image)
            still = record.id;
    cardAsset->chooseIndex(cardAsset->findData(QString::number(still.value())));
    waitThumbnail();
    require(QFile::remove(directory.filePath("still.png")), "remove fixture media file");
    // Freshness follows document changes, never the playhead (bug #401): an edit refreshes.
    setSource("loopMode", std::int64_t{1});
    timer.restart();
    while (!controller.missing(still) && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(controller.missing(still) && controller.thumbnail(still).isNull(),
            "unreadable asset never reuses a stale decoded proxy");
    require(card->assetProblem() && warningBadge(renderCard()) > 5,
            "an unreadable source shows the warning badge");
    controller.cancel();
    bridge.beginShutdown();
    scheduler.beginShutdown();
    timer.restart();
    while (!scheduler.isQuiescent() && timer.elapsed() < 15000)
        QTest::qWait(10);
    require(scheduler.isQuiescent(), "media tasks shut down safely");
}
// Bug #401 / task N1, item 2: playback asks the media worker for nothing. With 25 image-sequence
// layers, 120 playback ticks start no thumbnail, probe or freshness rescan and read no source file.
void playbackStartsNoMediaJobs() {
    QTemporaryDir directory;
    require(directory.isValid(), "playback media directory");
    QStringList firsts;
    QImage frame(32, 16, QImage::Format_RGBA8888);
    for (int sequence = 0; sequence < 25; ++sequence)
        for (int index = 1; index <= 3; ++index) {
            frame.fill(QColor::fromHsv(sequence * 14, 120, 60 + index * 40));
            const auto path = directory.filePath(
                QStringLiteral("plate%1.%2.png").arg(sequence).arg(index, 4, 10, QLatin1Char('0')));
            require(frame.save(path), "generate sequence member");
            if (index == 1)
                firsts.push_back(path);
        }
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);
    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    if (session.composition() == nullptr) {
        commands::Transaction seed("Seed playback composition", session.snapshot().revision());
        seed.emplace<commands::AddComposition>("Main", document::CompositionFormat{},
                                               core::RationalTime::fromInteger(10));
        const auto seeded = session.executeTransaction(std::move(seed));
        const auto id = seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput);
        require(id.has_value() && session.setComposition(*id), "playback composition seed");
    }
    ui::TaskUiBridge bridge(scheduler);
    ui::AssetController controller(session, host, scheduler, bridge);
    controller.importFiles(firsts, std::nullopt);
    QElapsedTimer timer;
    timer.start();
    while ((controller.busy() || session.snapshot().project().assets().size() < 25) &&
           timer.elapsed() < 30000)
        QTest::qWait(10);
    require(session.snapshot().project().assets().size() == 25, "25 sequences import");
    commands::Transaction layers("Add Layers", session.snapshot().revision());
    for (const auto& asset : session.snapshot().project().assets())
        layers.emplace<commands::AddImageLayer>(session.compositionId(), asset.id);
    require(session.executeTransaction(std::move(layers)).succeeded(), "25 sequence layers");
    timer.restart();
    while ((controller.busy() || controller.thumbnailsPending()) && timer.elapsed() < 30000)
        QTest::qWait(10);
    require(!controller.thumbnailsPending(), "the media worker settles before playback");
    const auto before = platform::sourceReadWork();
    int started = 0;
    for (int tick = 0; tick < 120; ++tick) {
        const auto time = core::RationalTime::create(tick % 48, 24);
        require(time.has_value(), "playback tick");
        static_cast<void>(session.setCurrentTime(*time));
        QApplication::processEvents();
        if (controller.thumbnailsPending() || controller.busy())
            ++started;
    }
    // Longer than any delayed check would wait, so a deferred job would show up here too.
    timer.restart();
    while (timer.elapsed() < 1500) {
        QTest::qWait(20);
        if (controller.thumbnailsPending() || controller.busy())
            ++started;
    }
    const auto after = platform::sourceReadWork();
    require(started == 0, "120 playback ticks start no media job");
    require(after.fullReads == before.fullReads && after.identityChecks == before.identityChecks,
            "120 playback ticks probe and hash no source file");
    controller.cancel();
    bridge.beginShutdown();
    scheduler.beginShutdown();
    timer.restart();
    while (!scheduler.isQuiescent() && timer.elapsed() < 15000)
        QTest::qWait(10);
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        inputColorCatalogFollowsWorkingSpace();
        compositionDrops();
        run();
        partialBatchImport();
        playbackStartsNoMediaJobs();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
