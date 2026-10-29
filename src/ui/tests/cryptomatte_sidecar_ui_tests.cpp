// #364 the owner's setup, end to end: a beauty EXR sequence and a sidecar Cryptomatte-only EXR
// sequence (CryptoAsset ranks, no RGB), imported through the application's Assets controller and
// wired as the owner did (the sidecar's Channels and Image into a Cryptomatte, its Image into the
// layer). It checks each reported problem:
//  1. the sidecar is a valid "Sequence [N]" asset with an ID-colour thumbnail, no Relink warning,
//     and its Image source says plainly "No colour layer — use Channels";
//  2. Part and Layer (node card and Properties) are dropdowns listing only what the file has;
//  3. the manifest names are a searchable checklist;
//  4. Properties shows no "Mask: Resolving…" readout;
//  5. selecting the Cryptomatte shows the ID-colour preview in the Viewer (Preview on by default),
//     "Pick in Viewer" switches to the eyedropper, and the hint line is there;
//  6. with no mattes selected the Image output is transparent black.

#include "cryptomatte_fixture_support.hpp"
#include "node_editor_items.hpp"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/cryptomatte_inspection.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/tool_controller.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace bloom;
namespace fixture = bloom::cryptomatte_fixture;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Predicate> bool waitUntil(Predicate predicate, const int milliseconds = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return predicate();
}

// A plain RGBA beauty frame the size of the Cryptomatte fixture.
void writeBeauty(const std::filesystem::path& path, const float level) {
    const Imath::Box2i window(Imath::V2i(0, 0),
                              Imath::V2i(fixture::kWidth - 1, fixture::kHeight - 1));
    Imf::Header header(window, window);
    std::vector<std::vector<float>> planes(4, std::vector<float>(fixture::kWidth * fixture::kHeight));
    for (std::size_t index = 0; index < planes[0].size(); ++index) {
        planes[0][index] = level;
        planes[1][index] = 0.5F;
        planes[2][index] = 0.25F;
        planes[3][index] = 1.0F;
    }
    Imf::FrameBuffer frameBuffer;
    const std::array<const char*, 4> names{"R", "G", "B", "A"};
    for (std::size_t channel = 0; channel < names.size(); ++channel) {
        header.channels().insert(names[channel], Imf::Channel(Imf::FLOAT));
        frameBuffer.insert(names[channel],
                           Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(planes[channel].data()),
                                      sizeof(float), fixture::kWidth * sizeof(float)));
    }
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(fixture::kHeight);
}

ui::node_editor::NodeItem* card(ui::NodeGraphEditor& nodes, const document::NodeId id) {
    return dynamic_cast<ui::node_editor::NodeItem*>(nodes.graphScene()->findNodeItem(id));
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        std::mt19937_64 random{std::random_device{}()};
        const auto folder = std::filesystem::temp_directory_path() /
                            ("bloom-cryptomatte-sidecar-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        for (int frame = 1; frame <= 2; ++frame) {
            writeBeauty(folder / ("SC_image_000" + std::to_string(frame) + ".exr"),
                        0.25F * static_cast<float>(frame));
            fixture::writeCryptomatteExr(
                folder / ("SC_cryptomatte_asset_000" + std::to_string(frame) + ".exr"), true,
                "CryptoAsset", {}, 0, false);
        }

        runtime::TaskScheduler scheduler;
        ui::ProjectHost host(scheduler);
        ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                       *host.liveDocumentAndStack().second,
                                       host.lowestCompositionId());
        if (session.composition() == nullptr) {
            commands::Transaction seed("Seed", session.snapshot().revision());
            seed.emplace<commands::AddComposition>(
                "Main",
                *document::CompositionFormat::create(fixture::kWidth, fixture::kHeight,
                                                     core::PixelAspectRatio::square(),
                                                     *document::FrameRate::create(24, 1)),
                core::RationalTime::fromInteger(1));
            const auto seeded = session.executeTransaction(std::move(seed));
            const auto id = seeded.succeeded() ? seeded.outputId<document::CompositionId>(
                                                     commands::kAddCompositionOutput)
                                               : std::nullopt;
            if (!id || !session.setComposition(*id))
                throw std::runtime_error("composition seed");
        }
        ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});
        ui::AssetController assets(session, host, scheduler, bridge);

        runtime::NodeDefinitionRegistry definitions;
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("definitions");
        definitions.freeze();
        runtime::SnapshotCompiler compiler(definitions);
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(folder);
        runtime::CpuReferenceDisplayPreparer displayPreparer;
        runtime::QualifiedDisplayProcessorProvider qualified;
        ui::CompositionPreviewController preview(
            session, scheduler, bridge,
            ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualified));
        ui::CryptomatteController cryptomatte(
            session, scheduler, bridge,
            [&](const document::Snapshot& snapshot, const document::CompositionId composition,
                const document::NodeId node, const core::RationalTime time,
                const runtime::EvaluationColorIntent& colorIntent,
                const runtime::CancellationToken& cancellation) {
                return runtime::listCryptomatteLayers(compiler, snapshot, composition, node, folder,
                                                      time, colorIntent, cancellation);
            },
            [&](const document::Snapshot& snapshot, const document::CompositionId composition,
                const document::NodeId node, const core::RationalTime time,
                const runtime::EvaluationColorIntent& colorIntent,
                const std::optional<runtime::ChannelInspection>& inspection, const std::int64_t x,
                const std::int64_t y, const runtime::CancellationToken& cancellation) {
                return runtime::pickCryptomatte(compiler, snapshot, composition, node, folder, time,
                                                colorIntent, inspection, x, y, cancellation);
            });

        // 1. Import both sequences through Assets.
        assets.importFiles({QString::fromStdString((folder / "SC_image_0001.exr").string()),
                            QString::fromStdString(
                                (folder / "SC_cryptomatte_asset_0001.exr").string())});
        waitUntil([&] { return !assets.busy(); });
        const auto& records = session.snapshot().project().assets();
        expect(records.size() == 2, "both sequences import");
        if (records.size() != 2)
            return 1;
        const auto sidecar = std::ranges::find_if(records, [](const auto& record) {
                                 return record.name.find("cryptomatte") != std::string::npos;
                             })->id;
        const auto* sidecarRecord = session.snapshot().project().findAsset(sidecar);
        expect(sidecarRecord != nullptr && sidecarRecord->kind == document::AssetKind::Sequence,
               "the sidecar is a sequence asset");
        expect(waitUntil([&] { return !assets.thumbnail(sidecar).isNull(); }),
               "the sidecar gets a thumbnail");
        expect(!assets.missing(sidecar) && assets.problem(sidecar).isEmpty() &&
                   assets.dataOnly(sidecar),
               "the sidecar is valid and data-only: no Relink, no problem");

        // Wire it as the owner did.
        const auto composition = session.compositionId();
        const auto run = [&](const auto& build) {
            commands::Transaction transaction("Owner setup", session.snapshot().revision());
            build(transaction);
            const auto result = session.executeTransaction(std::move(transaction));
            if (!result.succeeded())
                throw std::runtime_error("owner setup command refused");
            return result;
        };
        run([&](auto& t) { t.template emplace<commands::AddImageLayer>(composition, sidecar); });
        document::NodeId source;
        document::NodeId layer;
        for (const auto& node : session.composition()->graph().nodes()) {
            if (node.typeId == document::kImageSourceNodeType)
                source = node.id;
            if (node.typeId == document::kLayerOutputNodeType)
                layer = node.id;
        }
        const auto node = *run([&](auto& t) {
                               t.template emplace<commands::AddNode>(
                                   composition, std::string(document::kCryptomatteNodeType),
                                   document::Vec2d{240, 0});
                           }).template outputId<document::NodeId>(commands::kAddNodeOutput);
        run([&](auto& t) {
            t.template emplace<commands::SetNodeParameterValue>(
                composition, node, std::string(document::kCryptomatteLayerParameterRole),
                document::ParameterValue{std::string("CryptoAsset")});
        });
        const auto connect = [&](const document::NodeId from, const std::string_view output,
                                 const document::NodeId to, const std::string_view input) {
            run([&](auto& t) {
                t.template emplace<commands::ConnectPorts>(
                    composition, document::OutputPortRef{from, std::string(output)},
                    document::NodeInputRef{to, std::string(input)});
            });
        };
        connect(source, document::kImageSourceChannelsOutputPort, node, document::kChannelSetPort);
        connect(source, document::kImageSourceImageOutputPort, node,
                document::kCryptomatteImageInputPort);
        connect(node, document::kCryptomatteImageOutputPort, layer,
                document::kLayerOutputContentInputPort);

        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* assetsPanel = new ui::AssetsEditor(session, &window);
        auto* nodes = new ui::NodeGraphEditor(session, &window);
        auto* properties = new ui::PropertiesEditor(session, &window);
        auto* viewer = new ui::ViewerEditor(session, preview, nullptr, &window);
        layout->addWidget(assetsPanel);
        layout->addWidget(nodes);
        layout->addWidget(properties);
        layout->addWidget(viewer);
        window.resize(1200, 1800);
        window.show();
        QApplication::processEvents();

        // Assets row: a two-frame Sequence for both, no warning glyph or tooltip (#378 columns).
        auto* tree = assetsPanel->findChild<QTreeWidget*>("assetsTree");
        bool rowsOk = tree != nullptr;
        if (tree != nullptr)
            for (int index = 0; index < tree->topLevelItemCount(); ++index) {
                const auto* item = tree->topLevelItem(index);
                if (item->text(0).contains("SC_"))
                    rowsOk = rowsOk && item->text(1) == "Sequence" &&
                             item->text(3).endsWith(QStringLiteral(" (2 frames)")) &&
                             item->data(0, Qt::UserRole + 8).toString().isEmpty() &&
                             !item->toolTip(0).contains("Missing") &&
                             !item->toolTip(0).contains("Replace Footage");
            }
        expect(rowsOk, "Assets lists both as two-frame Sequences without a missing warning");

        // 2. Part and Layer are dropdowns of what the file has, on the card and in Properties.
        auto* sourceCard = card(*nodes, source);
        ui::kit::KDropdown* cardPart = nullptr;
        ui::kit::KDropdown* cardLayer = nullptr;
        expect(waitUntil([&] {
                   QApplication::processEvents();
                   nodes->graphScene()->refreshValues();
                   cardPart = sourceCard ? qobject_cast<ui::kit::KDropdown*>(
                                               sourceCard->fieldWidget("nodeImagePart"))
                                         : nullptr;
                   cardLayer = sourceCard ? qobject_cast<ui::kit::KDropdown*>(
                                                sourceCard->fieldWidget("nodeImageLayer"))
                                          : nullptr;
                   return cardPart != nullptr && cardLayer != nullptr &&
                          cardPart->toolTip().isEmpty() && cardLayer->count() > 0 &&
                          cardLayer->itemText(cardLayer->currentIndex()) == "No colour layer";
               }),
               "the Image source card shows Part and Layer dropdowns once the file is read");
        expect(cardPart && cardPart->count() == 1 && cardPart->itemText(0) == "First part",
               "the card's Part dropdown lists only the file's one part");
        expect(cardLayer && cardLayer->itemText(cardLayer->currentIndex()) == "No colour layer" &&
                   !cardLayer->isEnabled(),
               "the card's Layer dropdown says the file has no colour layer");
        expect(sourceCard && sourceCard->fieldWidget("nodeOperandTextEditor") == nullptr,
               "the Image source card has no free-text Part or Layer field");
        session.selectNode(source);
        QApplication::processEvents();
        QLabel* note = nullptr;
        expect(waitUntil([&] {
                   QApplication::processEvents();
                   note = nullptr;
                   for (auto* label : properties->findChildren<QLabel*>("propertiesImageColourNote"))
                       if (label->isVisible())
                           note = label;
                   return note != nullptr;
               }),
               "Properties shows the colour note for the data-only source");
        expect(note && note->text() == "No colour layer — use Channels",
               "the note says plainly what to use");

        // The Cryptomatte card: a Layer dropdown, no matte text field.
        auto* cryptoCard = card(*nodes, node);
        ui::kit::KDropdown* cryptoLayer = nullptr;
        expect(waitUntil([&] {
                   QApplication::processEvents();
                   nodes->graphScene()->refreshValues();
                   cryptoLayer = cryptoCard ? qobject_cast<ui::kit::KDropdown*>(
                                                  cryptoCard->fieldWidget("nodeCryptomatteLayer"))
                                            : nullptr;
                   return cryptoLayer != nullptr && cryptoLayer->isEnabled();
               }) || (cryptoLayer && cryptoLayer->count() == 1),
               "the Cryptomatte card shows its Layer as a dropdown");
        expect(cryptoLayer && cryptoLayer->itemText(cryptoLayer->currentIndex()) == "CryptoAsset",
               "the card's Layer dropdown shows CryptoAsset");
        expect(cryptoCard && cryptoCard->fieldWidget("nodeOperandTextEditor") == nullptr,
               "the Cryptomatte card has no matte text field");

        // 4/5/3. Select the Cryptomatte.
        session.selectNode(node);
        QApplication::processEvents();
        expect(properties->findChild<QLabel*>("propertiesValueOutput.mask") == nullptr,
               "Properties shows no Mask readout stuck on Resolving");
        auto* previewToggle = properties->findChild<ui::kit::KSwitch*>("propertiesCryptomattePreview");
        expect(previewToggle && previewToggle->isChecked(), "Preview is on by default");
        auto* hint = properties->findChild<QLabel*>("propertiesCryptomatteHint");
        expect(hint && hint->text() == "Click objects in the Viewer to add them",
               "the hint line is shown");
        expect(waitUntil([&] {
                   const auto& inspection = preview.viewerChannelInspection();
                   return inspection && inspection->node == node &&
                          inspection->port == document::kCryptomattePreviewInspectionPort;
               }),
               "selecting the Cryptomatte shows its ID-colour preview in the Viewer");
        expect(waitUntil([&] {
                   const auto& state = preview.state();
                   return state.activity == ui::PreviewActivity::Ready && state.frame != nullptr &&
                          state.frame->desiredIdentity().channelInspection ==
                              preview.viewerChannelInspection() &&
                          state.frame->hasProcessFrame();
               }),
               "the preview renders");
        if (const auto frame = preview.state().frame; frame && frame->hasProcessFrame()) {
            // Pixel 0 is the sphere alone at full coverage, mixed halfway with no colour image.
            const auto value = frame->processImage().read(0, 0);
            const auto colour = runtime::cryptomatteIdColour(runtime::cryptomatteNameId("sphere"));
            expect(value && std::abs(value.value()->red() - 0.5F * colour[0]) < 1e-5F &&
                       std::abs(value.value()->alpha() - 0.5F) < 1e-5F,
                   "the preview shows the sphere in its ID colour");
        }
        auto* pick = properties->findChild<ui::kit::KButton*>("propertiesCryptomattePick");
        session.tools().setTool(ui::ViewerTool::Select);
        if (pick != nullptr)
            pick->click();
        QApplication::processEvents();
        // ADR 0031: the pick chooses the one app-wide eyedropper, for every Viewer.
        expect(session.tools().tool() == ui::ViewerTool::Cryptomatte,
               "Pick in Viewer switches the app-wide tool to the eyedropper");

        QList<ui::kit::KCheckBox*> names;
        expect(waitUntil([&] {
                   QApplication::processEvents();
                   names = properties->findChildren<ui::kit::KCheckBox*>(
                       "propertiesCryptomatteName");
                   return static_cast<std::size_t>(names.size()) == fixture::kObjects.size();
               }),
               "the manifest names are listed");
        for (auto* check : names)
            if (check->property("matteName").toString() == "cube")
                check->click();
        QApplication::processEvents();
        expect(cryptomatte.mattes(node) == std::vector<std::string>{"cube"},
               "checking a name adds it to the matte");
        ui::kit::KLineEdit* search = nullptr;
        waitUntil([&] {
            for (auto* field :
                 properties->findChildren<ui::kit::KLineEdit*>("propertiesCryptomatteSearch"))
                if (field->isVisible())
                    search = field;
            return search != nullptr;
        }, 3000);
        expect(search != nullptr, "the name search field is shown");
        if (search != nullptr)
            search->setText("fl");
        QApplication::processEvents();
        int visible = 0;
        for (auto* check : properties->findChildren<ui::kit::KCheckBox*>("propertiesCryptomatteName"))
            visible += check->isVisibleTo(properties) ? 1 : 0;
        expect(visible == 1, "searching narrows the name list");

        // 6. With no mattes, the Image output (shown through the layer) is transparent black.
        expect(cryptomatte.setMattes(node, {}, "Clear"), "clear the mattes");
        previewToggle->click();
        expect(!cryptomatte.preview(node), "the toggle turns the preview off");
        expect(waitUntil([&] { return !preview.viewerChannelInspection().has_value(); }),
               "turning Preview off returns the Viewer to the composition");
        expect(waitUntil([&] {
                   const auto& state = preview.state();
                   return state.activity == ui::PreviewActivity::Ready && state.frame != nullptr &&
                          !state.frame->desiredIdentity().channelInspection &&
                          state.frame->desiredIdentity().sourceRevision ==
                              session.snapshot().revision() &&
                          state.frame->hasProcessFrame();
               }),
               "the composition renders");
        if (const auto frame = preview.state().frame; frame && frame->hasProcessFrame()) {
            bool black = true;
            for (const auto& pixel : frame->processImage().pixels())
                black = black && pixel.alpha() == 0.0F && pixel.red() == 0.0F;
            expect(black, "no mattes give a transparent black Image output");
        }

        preview.beginShutdown();
        bridge.beginShutdown();
        scheduler.beginShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!scheduler.isQuiescent() && timer.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: Cryptomatte sidecar workflow\n";
    return 0;
}
