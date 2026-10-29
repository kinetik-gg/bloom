#include "assets_editor_internal.hpp"
#include "colour_space_names.hpp"
#include "editor_chrome_test_support.hpp"
#include "reveal_in_file_manager.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/thumbnail.hpp>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QHeaderView>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWidget>

#include <cstdio>
#include <filesystem>
#include <utility>

namespace {
using namespace bloom;
namespace assets = ui::assets;

struct TestContext {
    bool ok = true;

    void expect(const bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ok = false;
        }
    }
};

QTreeWidgetItem* findText(QTreeWidget& tree, const QString& name) {
    for (QTreeWidgetItemIterator it(&tree); *it; ++it)
        if ((*it)->text(assets::NameColumn) == name)
            return *it;
    return nullptr;
}

// The object names of a context menu's actions; optionally triggers one after the menu closes.
QStringList contextMenu(QTreeWidget& tree, const QPoint position, const QString& trigger = {}) {
    QStringList names;
    QTimer::singleShot(0, &tree, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr)
            return;
        QAction* chosen = nullptr;
        for (auto* action : menu->actions()) {
            if (!action->objectName().isEmpty())
                names.push_back(action->objectName());
            if (!trigger.isEmpty() && action->objectName() == trigger)
                chosen = action;
        }
        menu->close();
        if (chosen != nullptr)
            chosen->trigger();
    });
    emit tree.customContextMenuRequested(position);
    QApplication::processEvents();
    return names;
}

document::AssetRecord videoAsset(const std::uint64_t id, const std::string& name,
                                 const std::string& codec, const std::uint32_t width,
                                 const std::uint32_t height) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(id);
    asset.name = name;
    asset.kind = document::AssetKind::Video;
    asset.locator = {"file", "project-relative", name + ".mov", "file:///media/" + name + ".mov"};
    asset.width = width;
    asset.height = height;
    asset.frames = 48;
    asset.duration = core::RationalTime::fromInteger(2);
    document::AssetVideoStream stream;
    stream.codec = codec;
    stream.timebase = *core::RationalTime::create(1, 24);
    stream.framePeriod = *core::RationalTime::create(1, 24);
    stream.width = width;
    stream.height = height;
    stream.duration = asset.duration;
    asset.videoStreams.push_back(stream);
    return asset;
}

void videoAssetRows(TestContext& context) {
    auto seed = document::makeNewProject("Video", "Main", core::RationalTime::fromInteger(2));
    const auto asset = videoAsset(1, "Video fixture", "prores", 64, 48);
    context.expect(seed.project.addAsset(asset), "valid video asset fixture");
    document::Document document(std::move(seed.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, seed.initialCompositionId);
    ui::AssetsEditor editor(session);
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    context.expect(tree != nullptr, "video assets tree");
    if (!tree)
        return;
    auto* item = findText(*tree, "Video fixture");
    context.expect(item != nullptr, "video asset appears in Assets");
    if (item != nullptr) {
        context.expect(item->text(assets::TypeColumn) == "Video", "Type column names the kind");
        context.expect(item->text(assets::DimensionsColumn) == QStringLiteral("64 × 48"),
                       "Dimensions column shows the frame size");
        context.expect(item->text(assets::DurationColumn) == "0:00:02:00" &&
                           item->toolTip(assets::DurationColumn) ==
                               "0:00:02:00 (48 frames) at 24 fps",
                       "Duration column shows the timecode; frames and rate are its tooltip");
        context.expect(item->text(assets::FrameRateColumn) == "24", "Frame Rate column");
        context.expect(item->text(assets::LocationColumn) == "Video fixture.mov",
                       "File Location column shows the locator");
        context.expect(item->toolTip(assets::NameColumn)
                           .contains(QString::fromUtf8(media::provider::kProResPreviewNote)),
                       "ProRes tooltip carries the platform's ProRes provenance note");
    }
    const auto rebuilds = editor.rowRebuildCount();
    commands::Transaction transaction("Add Video Layer", session.snapshot().revision());
    transaction.emplace<commands::AddImageLayer>(seed.initialCompositionId, asset.id);
    context.expect(session.executeTransaction(std::move(transaction)).succeeded(),
                   "video drop command participates in UI session history");
    context.expect(editor.rowRebuildCount() == rebuilds,
                   "an ordinary edit that leaves asset records unchanged keeps the Assets rows");
    commands::Transaction addition("Add composition", session.snapshot().revision());
    addition.emplace<commands::AddComposition>("Other", document::CompositionFormat{},
                                               core::RationalTime::fromInteger(2));
    context.expect(session.executeTransaction(std::move(addition)).succeeded(),
                   "composition add succeeds");
    context.expect(editor.rowRebuildCount() == rebuilds + 1,
                   "a record change rebuilds the Assets rows once");

    // The selection header shows the thumbnail cell, the facts and how often it is used.
    tree->clearSelection();
    item = findText(*tree, "Video fixture");
    tree->setCurrentItem(item);
    QApplication::processEvents();
    const auto label = [&](const char* name) {
        auto* found = editor.findChild<ui::kit::KLabel*>(QString::fromLatin1(name));
        return found ? found->toolTip().isEmpty() ? found->text() : found->toolTip() : QString{};
    };
    auto* thumbnail = editor.findChild<ui::kit::KThumbnail*>("assetsSelectionThumbnail");
    context.expect(thumbnail != nullptr && !thumbnail->pixmap().isNull() &&
                       thumbnail->height() == ui::kit::px(ui::kit::Size::StripThumbnailHeight),
                   "summary strip thumbnail is the 40-pixel well and never blank");
    context.expect(label("assetsSelectionName") == "Video fixture", "header names the item");
    context.expect(label("assetsSelectionKind") ==
                       QStringLiteral("Video · 64 × 48 · 0:00:02:00 (48 frames), 24 fps"),
                   "header states kind, size, duration (one format, no delta) and rate");
    context.expect(label("assetsSelectionUsage") == "Used once in 1 composition",
                   "header counts the layer that uses the clip");
}

// Folders first in both directions, numeric sizes, natural names; columns chosen and remembered.
void sortingAndColumns(TestContext& context) {
    auto seed = document::makeNewProject("Sorting", "Main", core::RationalTime::fromInteger(2));
    context.expect(seed.project.addAssetFolder({document::AssetFolderId::fromRaw(1), "Zed", {}}),
                   "folder fixture");
    context.expect(seed.project.addAsset(videoAsset(1, "Shot 10", "h264", 3840, 2160)) &&
                       seed.project.addAsset(videoAsset(2, "Shot 2", "h264", 640, 480)) &&
                       seed.project.addAsset(videoAsset(3, "shot 1", "h264", 100, 100)),
                   "sorting fixtures");
    document::Document document(std::move(seed.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, seed.initialCompositionId);
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
    auto editor = std::make_unique<ui::AssetsEditor>(session);
    editor->resize(900, 400);
    editor->show();
    QApplication::processEvents();
    auto* tree = editor->findChild<QTreeWidget*>("assetsTree");
    if (tree == nullptr) {
        context.expect(false, "sorting tree");
        return;
    }
    const auto order = [&] {
        QStringList names;
        for (int index = 0; index < tree->topLevelItemCount(); ++index)
            names.push_back(tree->topLevelItem(index)->text(assets::NameColumn));
        return names.join(',');
    };
    context.expect(tree->isSortingEnabled() && tree->sortColumn() == assets::NameColumn,
                   "the tree sorts by Name by default");
    context.expect(order() == "Zed,Main,shot 1,Shot 2,Shot 10",
                   "folders first, then items in natural, case-insensitive name order");
    tree->sortByColumn(assets::NameColumn, Qt::DescendingOrder);
    context.expect(order() == "Zed,Shot 10,Shot 2,shot 1,Main",
                   "descending order keeps folders first");
    tree->sortByColumn(assets::DimensionsColumn, Qt::AscendingOrder);
    context.expect(order() == "Zed,shot 1,Shot 2,Main,Shot 10",
                   "Dimensions sorts by pixel count, not text");

    auto* header = tree->header();
    for (int column = 0; column < assets::ColumnCount; ++column)
        context.expect(header->isSectionHidden(column) == !assets::defaultColumnVisible(column),
                       "only Name, Type and Dimensions show by default");
    context.expect(!header->isSectionHidden(assets::NameColumn) &&
                       !header->isSectionHidden(assets::TypeColumn) &&
                       !header->isSectionHidden(assets::DimensionsColumn) &&
                       header->isSectionHidden(assets::LocationColumn),
                   "the default columns are Name, Type and Dimensions");
    QStringList actions;
    QTimer::singleShot(0, editor.get(), [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr)
            return;
        for (auto* action : menu->actions())
            actions.push_back(action->objectName());
        for (auto* action : menu->actions())
            if (action->objectName() ==
                QStringLiteral("assetsColumnAction%1").arg(assets::TagsColumn))
                action->trigger();
        menu->close();
    });
    emit header->customContextMenuRequested(QPoint(4, 4));
    QApplication::processEvents();
    context.expect(actions.size() == assets::ColumnCount - 1 &&
                       !actions.contains(QStringLiteral("assetsColumnAction0")),
                   "the column chooser lists every column but Name");
    context.expect(!header->isSectionHidden(assets::TagsColumn), "choosing Tags shows the column");
    tree->setColumnWidth(assets::TypeColumn, 140);
    editor.reset();
    ui::AssetsEditor restored(session);
    auto* restoredTree = restored.findChild<QTreeWidget*>("assetsTree");
    context.expect(restoredTree && !restoredTree->header()->isSectionHidden(assets::TagsColumn) &&
                       restoredTree->header()->isSectionHidden(assets::LocationColumn) &&
                       restoredTree->columnWidth(assets::TypeColumn) == 140 &&
                       restoredTree->sortColumn() == assets::DimensionsColumn,
                   "column visibility, width and sort persist per user");
    auto* columns = restored.findChild<QMenu*>("assetsColumnMenu");
    context.expect(columns != nullptr && columns->actions().size() == assets::ColumnCount - 1,
                   "View > Columns offers the same chooser");
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
}

// After Effects style context menus on each kind of row and on empty space.
void contextMenus(TestContext& context) {
    auto seed = document::makeNewProject("Menus", "Main", core::RationalTime::fromInteger(2));
    context.expect(seed.project.addAssetFolder({document::AssetFolderId::fromRaw(1), "Bin", {}}) &&
                       seed.project.addAsset(videoAsset(1, "Clip", "h264", 320, 240)),
                   "menu fixtures");
    document::Document document(std::move(seed.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, seed.initialCompositionId);
    ui::AssetsEditor editor(session);
    editor.resize(700, 500);
    editor.show();
    QApplication::processEvents();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    if (tree == nullptr)
        return;
    const auto at = [&](const QString& name) {
        return tree->visualItemRect(findText(*tree, name)).center();
    };
    const QStringList common{"assetsContextNewFolderAction", "assetsContextNewCompositionAction",
                             "assetsContextImportAction"};
    auto names = contextMenu(*tree, at("Clip"));
    for (const auto* name :
         {"assetsNewCompositionFromSelectionAction", "assetsInputColorAction",
          "assetsReplaceFootageAction", "assetsReloadAction", "assetsRevealAction",
          "assetsRenameAction", "assetsEditTagsAction", "assetsDeleteAction"})
        context.expect(names.contains(QString::fromLatin1(name)), name);
    for (const auto& name : common)
        context.expect(names.contains(name),
                       "asset menu ends with New Folder, Composition, Import");
    names = contextMenu(*tree, at("Main"));
    for (const auto* name : {"assetsOpenAction", "assetsCompositionSettingsAction",
                             "assetsRenameAction", "assetsDuplicateAction", "assetsDeleteAction"})
        context.expect(names.contains(QString::fromLatin1(name)), name);
    names = contextMenu(*tree, at("Bin"));
    context.expect(names.contains("assetsRenameAction") && names.contains("assetsDeleteAction") &&
                       names.contains("assetsContextNewFolderAction"),
                   "folder menu offers Rename, Delete and New Folder");
    names = contextMenu(*tree, QPoint(5, tree->viewport()->height() - 5));
    context.expect(names == common, "empty space offers only New Folder, Composition, Import");

    // New Folder from a folder's menu nests the folder and opens the inline rename.
    (void)contextMenu(*tree, at("Bin"), "assetsContextNewFolderAction");
    auto* field = tree->findChild<QLineEdit*>("assetsRenameField");
    context.expect(field != nullptr, "the new folder renames in place");
    if (field != nullptr) {
        field->setText("Nested");
        QTest::keyClick(field, Qt::Key_Return);
        QApplication::processEvents();
        QApplication::processEvents();
    }
    const auto& project = session.snapshot().project();
    context.expect(project.assetFolders().size() == 2 &&
                       std::ranges::any_of(project.assetFolders(),
                                           [](const auto& folder) {
                                               return folder.name == "Nested" &&
                                                      folder.parent ==
                                                          document::AssetFolderId::fromRaw(1);
                                           }),
                   "New Folder lands inside the clicked folder with its typed name");

    // New Composition from Selection: sized from the clip, filed with it, opened.
    (void)contextMenu(*tree, at("Clip"), "assetsNewCompositionFromSelectionAction");
    const auto* created = session.composition();
    context.expect(created != nullptr && created->name() == "Clip" &&
                       created->format().width() == 320 &&
                       created->graph().layerOutputs().size() == 1,
                   "New Composition from Selection opens a clip-sized composition with its layer");
}

// #380: video, sequences and compositions share one duration format, timecode at the item's rate
// with its frame count, in the Duration column and the selection header. A sequence plays one
// member per composition frame, so it is timed at the active composition's rate.
void durationFormat(TestContext& context) {
    QTemporaryDir directory;
    context.expect(directory.isValid(), "temporary sequence directory");
    QImage frame(64, 48, QImage::Format_RGBA8888);
    frame.fill(Qt::gray);
    std::vector<std::filesystem::path> paths;
    for (int index = 1; index <= 24; ++index) {
        const auto path =
            directory.filePath(QStringLiteral("shot_%1.png").arg(index, 4, 10, QLatin1Char('0')));
        context.expect(frame.save(path), "write sequence member");
        if (index == 1)
            paths.emplace_back(path.toStdString());
    }
    auto seed = document::makeNewProject("Durations", "Main", core::RationalTime::fromInteger(2));
    document::Document document(std::move(seed.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, seed.initialCompositionId);
    commands::Transaction import("Import", session.snapshot().revision());
    import.emplace<commands::ImportAssets>(paths, directory.path().toStdString());
    context.expect(session.executeTransaction(std::move(import)).succeeded(),
                   "the sequence imports");
    ui::AssetsEditor editor(session);
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    if (tree == nullptr || session.snapshot().project().assets().empty()) {
        context.expect(false, "sequence fixture");
        return;
    }
    const auto name = QString::fromStdString(session.snapshot().project().assets().front().name);
    auto* sequence = findText(*tree, name);
    auto* composition = findText(*tree, "Main");
    context.expect(sequence != nullptr &&
                       sequence->text(assets::DurationColumn) == "0:00:01:00" &&
                       sequence->toolTip(assets::DurationColumn) ==
                           "0:00:01:00 (24 frames) at 24 fps",
                   "a 24-frame sequence in a 24 fps composition lasts 0:00:01:00 (24 frames)");
    context.expect(composition != nullptr &&
                       composition->text(assets::DurationColumn) == "0:00:02:00" &&
                       composition->toolTip(assets::DurationColumn) ==
                           "0:00:02:00 (48 frames) at 24 fps",
                   "a composition uses the same duration format");
    const auto label = [&](const char* object) {
        auto* found = editor.findChild<ui::kit::KLabel*>(QString::fromLatin1(object));
        return found ? found->toolTip() : QString{};
    };
    tree->setCurrentItem(sequence);
    QApplication::processEvents();
    context.expect(label("assetsSelectionKind") ==
                       QStringLiteral("Sequence · 64 × 48 · 0:00:01:00 (24 frames), 24 fps"),
                   "the header times a sequence like a video");
    tree->setCurrentItem(findText(*tree, "Main"));
    QApplication::processEvents();
    context.expect(label("assetsSelectionKind") ==
                       QStringLiteral("Composition · 1920 × 1080 · 0:00:02:00 (48 frames), 24 fps"),
                   "the header times a composition the same way");
    // A 25 fps composition becomes active: the sequence now lasts 24 frames at 25 fps.
    commands::Transaction other("Add composition", session.snapshot().revision());
    other.emplace<commands::AddComposition>(
        "PAL",
        *document::CompositionFormat::create(720, 576, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(25, 1)),
        core::RationalTime::fromInteger(2));
    const auto added = session.executeTransaction(std::move(other));
    const auto pal = added.outputId<document::CompositionId>(commands::kAddCompositionOutput);
    context.expect(pal && session.setComposition(*pal), "switch to the 25 fps composition");
    QApplication::processEvents();
    sequence = findText(*tree, name);
    context.expect(sequence != nullptr && sequence->text(assets::DurationColumn) == "0:00:00:24" &&
                       sequence->toolTip(assets::DurationColumn) ==
                           "0:00:00:24 (24 frames) at 25 fps",
                   "the sequence follows the active composition's rate");
    context.expect(assets::durationText(1, document::FrameRate::framesPerSecond24()) ==
                       "0:00:00:01 (1 frame)",
                   "one frame reads in the singular");
}

// #380: Bloom Neutral's interchange identifiers read as plain names; other configs' own names
// are already written for people.
void colourSpaceNames(TestContext& context) {
    context.expect(ui::colourSpaceDisplayName("lin_rec709_scene") == "Linear Rec.709",
                   "lin_rec709_scene reads as Linear Rec.709");
    context.expect(ui::colourSpaceDisplayName("srgb_rec709_display") == "sRGB",
                   "srgb_rec709_display reads as sRGB");
    context.expect(ui::colourSpaceDisplayName("Rec.709 Camera (Bloom)") == "Rec.709 video",
                   "the synthesized camera space reads as Rec.709 video");
    context.expect(ui::colourSpaceDisplayName("ACEScg") == "ACEScg" &&
                       ui::colourSpaceDisplayName("sRGB - Texture") == "sRGB - Texture",
                   "another config's names are shown as it spells them");
}

// #380: Reveal in File Manager selects the file where the platform allows it.
void revealPlans(TestContext& context) {
    using ui::RevealPlan;
    using ui::RevealPlatform;
    const std::filesystem::path file = "/media/shots/plate 01.mov";
    const auto linux = ui::revealPlan(RevealPlatform::Linux, file);
    context.expect(linux.kind == RevealPlan::Kind::ShowItems &&
                       linux.uris == QStringList{"file:///media/shots/plate%2001.mov"} &&
                       linux.folder == "/media/shots",
                   "Linux asks FileManager1.ShowItems for the file URI, with the folder to fall "
                   "back to");
    const auto mac = ui::revealPlan(RevealPlatform::MacOS, file);
    context.expect(mac.kind == RevealPlan::Kind::Process && mac.program == "open" &&
                       mac.arguments == QStringList{"-R", "/media/shots/plate 01.mov"} &&
                       mac.folder == "/media/shots",
                   "macOS runs open -R on the file");
    const auto windows = ui::revealPlan(RevealPlatform::Windows, "C:/Media/Shots/plate 01.mov");
    context.expect(windows.kind == RevealPlan::Kind::Process && windows.program == "explorer.exe" &&
                       windows.nativeArguments == R"(/select,"C:\Media\Shots\plate 01.mov")" &&
                       windows.folder == "C:/Media/Shots",
                   "Windows runs explorer /select, with the quoted native path");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomAssetsEditorTests"));
    QApplication::setApplicationName(QStringLiteral("AssetsEditor"));
    QStandardPaths::setTestModeEnabled(true);
    TestContext context;

    auto newProject = bloom::document::makeNewProject("Assets Test", "Main",
                                                      bloom::core::RationalTime::fromInteger(10));
    const auto compositionId = newProject.initialCompositionId;
    bloom::document::Document document(std::move(newProject.project));
    bloom::commands::CommandStack commandStack(document);
    bloom::ui::CompositionSession session(document, commandStack, compositionId);
    bloom::ui::AssetsEditor editor(session);
    editor.resize(500, 400);
    editor.show();
    QApplication::processEvents();

    auto* tree = editor.findChild<QTreeWidget*>(QStringLiteral("assetsTree"));
    auto* search = editor.findChild<QLineEdit*>(QStringLiteral("assetsSearchField"));
    context.expect(tree != nullptr, "assets tree exists");
    context.expect(search != nullptr, "assets search exists");
    if (tree == nullptr || search == nullptr) {
        return 1;
    }

    context.expect(tree->columnCount() == assets::ColumnCount, "assets tree has eight columns");
    const QStringList headers{"Name",       "Type",      "Dimensions", "Duration",
                              "Frame Rate", "File Size", "Tags",       "File Location"};
    for (int column = 0; column < assets::ColumnCount; ++column)
        context.expect(tree->headerItem()->text(column) == headers[column],
                       "assets tree names its columns");
    context.expect(tree->rootIsDecorated() && tree->topLevelItemCount() == 1 &&
                       tree->topLevelItem(0)->text(0) == QStringLiteral("Main") &&
                       tree->topLevelItem(0)->text(assets::TypeColumn) == "Composition",
                   "new project lists its composition in the one tree, at the root");
    context.expect(editor.findChild<QMenu*>(QStringLiteral("assetsViewMenu")) != nullptr,
                   "assets View menu exists");
    context.expect(editor.findChild<QMenu*>(QStringLiteral("assetsAddMenu")) != nullptr,
                   "assets Add menu exists");
    context.expect(editor.findChild<QMenu*>(QStringLiteral("assetsSelectMenu")) != nullptr,
                   "assets Select menu exists");

    auto* newFolder = editor.findChild<QAction*>(QStringLiteral("assetsNewFolderAction"));
    auto* import =
        editor.findChild<bloom::ui::kit::KIconButton*>(QStringLiteral("assetsImportButton"));
    context.expect(newFolder != nullptr && newFolder->isEnabled(), "header New Folder is live");
    context.expect(import != nullptr && import->isEnabled(), "footer Import is enabled");
    context.expect(import != nullptr && import->toolTip() == QStringLiteral("Import"),
                   "Import has its action tooltip");
    for (const auto* name : {"assetsNewCompositionButton", "assetsNewFolderButton",
                             "assetsImportButton", "assetsDeleteButton"})
        context.expect(editor.findChild<bloom::ui::kit::KIconButton*>(name) != nullptr,
                       "Assets footer uses icon buttons");
    context.expect(bloom::ui::test::header(editor) != nullptr, "header provider returns a widget");
    context.expect(bloom::ui::test::header(editor) != nullptr, "header provider is take-once");
    context.expect(bloom::ui::test::footer(editor) != nullptr, "footer provider returns a widget");
    context.expect(bloom::ui::test::footer(editor) != nullptr, "footer provider is take-once");

    bloom::commands::Transaction addTransaction("Add composition", session.snapshot().revision());
    addTransaction.emplace<bloom::commands::AddComposition>(
        "Second", bloom::document::CompositionFormat{}, bloom::core::RationalTime::fromInteger(8));
    const auto addResult = session.executeTransaction(std::move(addTransaction));
    context.expect(addResult.succeeded(), "assets can observe a newly added composition");
    context.expect(tree->topLevelItemCount() == 2, "assets rebuild after a composition is added");

    search->setText(QStringLiteral("Second"));
    QApplication::processEvents();
    int visibleCount = 0;
    QTreeWidgetItem* visibleItem = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        auto* item = *it;
        if (!item->isHidden() && item->data(0, assets::kCompositionRole).toULongLong() != 0) {
            ++visibleCount;
            visibleItem = item;
        }
    }
    context.expect(visibleCount == 1, "assets search filters the tree");
    context.expect(visibleItem != nullptr && visibleItem->text(0) == QStringLiteral("Second"),
                   "assets search leaves the matching composition visible");
    if (visibleItem != nullptr) {
        // The offscreen QPA plugin does not synthesize QTreeWidget's double-click signal
        // reliably. Invoke the same signal path used by the production itemDoubleClicked
        // connection so this test remains deterministic in the required headless gate.
        // #376: double-click opens the composition only; no Composition Settings modal appears.
        bool settingsShown = false;
        QTimer::singleShot(0, [&settingsShown] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QStringLiteral("compositionSettingsDialog") &&
                    widget->isVisible()) {
                    settingsShown = true;
                    if (auto* dialog = qobject_cast<QDialog*>(widget))
                        dialog->reject();
                }
        });
        const auto expected = bloom::document::CompositionId::fromRaw(
            visibleItem->data(0, assets::kCompositionRole).toULongLong());
        emit tree->itemDoubleClicked(visibleItem, assets::TypeColumn);
        QApplication::processEvents();
        context.expect(session.compositionId() == expected,
                       "double-click opens the selected composition from any column");
        context.expect(!settingsShown, "double-click opens no Composition Settings modal");
    }
    search->clear();

    videoAssetRows(context);
    sortingAndColumns(context);
    contextMenus(context);
    durationFormat(context);
    colourSpaceNames(context);
    revealPlans(context);
    return context.ok ? 0 : 1;
}
