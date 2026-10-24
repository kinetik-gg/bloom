#include "assets_editor_internal.hpp"
#include "editor_chrome_test_support.hpp"
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
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWidget>

#include <cstdio>
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
        context.expect(item->text(assets::SizeColumn) == QStringLiteral("64 × 48"),
                       "Size column shows the frame size");
        context.expect(item->text(assets::DurationColumn) == "0:00:02:00",
                       "Duration column shows After Effects timecode at the clip rate");
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
                       thumbnail->height() == ui::kit::px(ui::kit::Size::ImageThumbnail),
                   "selection header thumbnail cell is the 72-pixel cell and never blank");
    context.expect(label("assetsSelectionName") == "Video fixture", "header names the item");
    context.expect(label("assetsSelectionKind") ==
                       QStringLiteral("Video · 64 × 48 · Δ 0:00:02:00, 24 fps"),
                   "header states kind, size, duration and rate");
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
    tree->sortByColumn(assets::SizeColumn, Qt::AscendingOrder);
    context.expect(order() == "Zed,shot 1,Shot 2,Main,Shot 10",
                   "Size sorts by pixel count, not text");

    auto* header = tree->header();
    context.expect(header->isSectionHidden(assets::TagsColumn) &&
                       !header->isSectionHidden(assets::LocationColumn),
                   "Tags starts hidden; File Location shows");
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
    header->setSectionHidden(assets::LocationColumn, true);
    tree->setColumnWidth(assets::TypeColumn, 140);
    editor.reset();
    ui::AssetsEditor restored(session);
    auto* restoredTree = restored.findChild<QTreeWidget*>("assetsTree");
    context.expect(restoredTree && !restoredTree->header()->isSectionHidden(assets::TagsColumn) &&
                       restoredTree->header()->isSectionHidden(assets::LocationColumn) &&
                       restoredTree->columnWidth(assets::TypeColumn) == 140 &&
                       restoredTree->sortColumn() == assets::SizeColumn,
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

    context.expect(tree->columnCount() == assets::ColumnCount, "assets tree has seven columns");
    const QStringList headers{"Name",       "Type", "Size",         "Duration",
                              "Frame Rate", "Tags", "File Location"};
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
    return context.ok ? 0 : 1;
}
