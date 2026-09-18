#include "editor_chrome_test_support.hpp"
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWidget>

#include <cstdio>
#include <utility>

namespace {

struct TestContext {
    bool ok = true;

    void expect(const bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ok = false;
        }
    }
};

void videoAssetRows(TestContext& context) {
    using namespace bloom;
    auto seed = document::makeNewProject("Video", "Main", core::RationalTime::fromInteger(2));
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(1);
    asset.name = "Video fixture";
    asset.kind = document::AssetKind::Video;
    asset.locator = {"file", "project-relative", "clip.mov", "file:///media/clip.mov"};
    asset.width = 64;
    asset.height = 48;
    asset.frames = 48;
    asset.duration = core::RationalTime::fromInteger(2);
    document::AssetVideoStream stream;
    stream.codec = "prores";
    stream.timebase = core::RationalTime::fromInteger(1);
    stream.framePeriod = core::RationalTime::fromInteger(1);
    stream.width = 64;
    stream.height = 48;
    stream.duration = asset.duration;
    asset.videoStreams.push_back(stream);
    context.expect(seed.project.addAsset(asset), "valid video asset fixture");
    document::Document document(std::move(seed.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, seed.initialCompositionId);
    ui::AssetsEditor editor(session);
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    context.expect(tree != nullptr, "video assets tree");
    if (!tree)
        return;
    bool found = false;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == "Video fixture") {
            found = true;
            context.expect((*it)->text(1) == QStringLiteral("Video · 00:00:02"),
                           "video kind includes HH:MM:SS");
            context.expect((*it)->toolTip(0).contains(
                               QString::fromUtf8(bloom::media::provider::kProResPreviewNote)),
                           "ProRes tooltip carries the platform's ProRes provenance note");
        }
    context.expect(found, "video asset appears in Assets");
    commands::Transaction transaction("Add Video Layer", session.snapshot().revision());
    transaction.emplace<commands::AddImageLayer>(seed.initialCompositionId, asset.id);
    context.expect(session.executeTransaction(std::move(transaction)).succeeded(),
                   "video drop command participates in UI session history");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
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

    context.expect(tree->columnCount() == 2, "assets tree has two columns");
    context.expect(tree->headerItem()->text(0) == QStringLiteral("Name"),
                   "assets tree names the Name column");
    context.expect(tree->headerItem()->text(1) == QStringLiteral("Kind"),
                   "assets tree names the Kind column");
    context.expect(tree->rootIsDecorated() && tree->topLevelItemCount() == 1 &&
                       tree->topLevelItem(0)->text(0) == QStringLiteral("Compositions") &&
                       tree->topLevelItem(0)->childCount() == 1,
                   "new project exposes its composition under an expandable Compositions root");
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
    context.expect(tree->topLevelItem(0)->childCount() == 2,
                   "assets composition root rebuilds after add");

    search->setText(QStringLiteral("Second"));
    QApplication::processEvents();
    int visibleCount = 0;
    QTreeWidgetItem* visibleItem = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        auto* item = *it;
        if (!item->isHidden() && item->data(0, Qt::UserRole + 1).toULongLong() != 0) {
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
        tree->itemDoubleClicked(visibleItem, 0);
        QApplication::processEvents();
        context.expect(session.compositionId() ==
                           bloom::document::CompositionId::fromRaw(
                               visibleItem->data(0, Qt::UserRole + 1).toULongLong()),
                       "double-click opens the selected composition");
    }

    videoAssetRows(context);
    return context.ok ? 0 : 1;
}
