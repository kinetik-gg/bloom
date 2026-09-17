#include "assets_editor_internal.hpp"
#include <QApplication>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTest>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace bloom;
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
QTreeWidgetItem* find(QTreeWidget& tree, int role, std::uint64_t id) {
    for (QTreeWidgetItemIterator it(&tree); *it; ++it)
        if ((*it)->data(0, role).toULongLong() == id)
            return *it;
    throw std::runtime_error("tree item missing");
}
std::unique_ptr<QMimeData> mimeFor(QTreeWidget& tree, QTreeWidgetItem* item) {
    tree.setCurrentItem(item);
    return std::unique_ptr<QMimeData>(tree.model()->mimeData({tree.indexFromItem(item)}));
}
bool drop(QTreeWidget& tree, const QMimeData& mime, QPoint position) {
    QDragEnterEvent enter(position, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &enter);
    if (!enter.isAccepted())
        return false;
    QDragMoveEvent move(position, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &move);
    if (!move.isAccepted())
        return false;
    QDropEvent event(position, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree.viewport(), &event);
    QApplication::processEvents();
    return event.isAccepted();
}
void rename(QTreeWidget& tree, QTreeWidgetItem* item, const QString& name, bool cancel = false) {
    tree.setCurrentItem(item);
    tree.setFocus();
    QTest::keyClick(&tree, Qt::Key_F2);
    QApplication::processEvents();
    auto* field = tree.findChild<QLineEdit*>("assetsRenameField");
    require(field, "inline rename editor opens");
    field->setText(name);
    QTest::keyClick(field, cancel ? Qt::Key_Escape : Qt::Key_Return);
    QApplication::processEvents();
    QApplication::processEvents();
}
project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory");
    return std::move(*operation);
}
void run() {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    const auto folder = document::AssetFolderId::fromRaw(1);
    const auto nested = document::AssetFolderId::fromRaw(2);
    require(initial.project.addAssetFolder({folder, "Shots", {}}), "folder fixture");
    require(initial.project.addAssetFolder({nested, "Plates", folder}), "nested folder fixture");
    const auto a = document::AssetId::fromRaw(1), b = document::AssetId::fromRaw(2),
               c = document::AssetId::fromRaw(3);
    for (const auto id : {a, b, c}) {
        document::AssetRecord asset;
        asset.id = id;
        asset.name = id == a ? "First plate" : id == b ? "Hero name only" : "Other plate";
        asset.locator = {"file", "project-relative", "media/" + std::to_string(id.value()) + ".png",
                         "file:///media/image.png"};
        asset.width = 4;
        asset.height = 3;
        asset.folder = id == a ? std::optional(nested) : std::nullopt;
        if (id == a)
            asset.tags = {"approved", "hero"};
        asset.order = id == c ? 1 : 0;
        require(initial.project.addAsset(asset), "asset fixture");
    }
    document::Document document(std::move(initial.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, initial.initialCompositionId);
    ui::AssetsEditor editor(session);
    editor.resize(620, 420);
    editor.show();
    QApplication::processEvents();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    auto* search = editor.findChild<QLineEdit*>("assetsSearchField");
    require(tree && search, "Assets controls");
    require(tree->rootIsDecorated(), "hierarchical tree");
    auto* folderItem = find(*tree, ui::assets::kFolderRole, folder.value());
    auto* nestedItem = find(*tree, ui::assets::kFolderRole, nested.value());
    require(nestedItem->parent() == folderItem, "nested folder projection");
    require(find(*tree, ui::assets::kAssetRole, a.value())->parent() == nestedItem,
            "asset under its folder");
    auto* row = qobject_cast<ui::kit::KRow*>(tree->itemWidget(folderItem, 0));
    require(row && row->disclosureButton(), "folder kit disclosure");
    row->disclosureButton()->click();
    require(!folderItem->isExpanded(), "kit disclosure collapses folder");
    search->setText("tag:HERO");
    require(!find(*tree, ui::assets::kAssetRole, a.value())->isHidden() &&
                find(*tree, ui::assets::kAssetRole, b.value())->isHidden() &&
                folderItem->isExpanded(),
            "tag filter matches tags only and exposes matching ancestors");
    search->clear();
    require(!folderItem->isExpanded(), "clearing filter restores expansion choice");
    search->setText("approved");
    require(!find(*tree, ui::assets::kAssetRole, a.value())->isHidden() &&
                find(*tree, ui::assets::kAssetRole, c.value())->isHidden(),
            "plain search matches tags");
    search->clear();
    folderItem->setExpanded(true);
    auto* aItem = find(*tree, ui::assets::kAssetRole, a.value());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualItemRect(aItem).center());
    require(tree->currentItem() == aItem && aItem->isSelected(),
            "kit row selects through the tree");
    const auto selectedPixels = tree->viewport()->grab().toImage();
    require(selectedPixels.pixelColor(ui::kit::px(ui::kit::Spacing::S),
                                      tree->visualItemRect(aItem).center().y()) ==
                ui::kit::color(ui::kit::Color::SurfaceRaised),
            "tree selection gutter matches kit row background");
    auto* assetRow = qobject_cast<ui::kit::KRow*>(tree->itemWidget(aItem, 0));
    auto* chip = assetRow->findChild<ui::kit::KButton*>("assetsTagChip");
    require(chip && chip->text() == "approved", "tag chip displays sorted first tag");
    chip->click();
    require(search->text() == "tag:approved", "tag chip activates filter");
    search->clear();
    rename(*tree, find(*tree, ui::assets::kAssetRole, a.value()), "Hero plate");
    require(session.snapshot().project().findAsset(a)->name == "Hero plate",
            "inline rename authors asset name");
    require(session.undo(), "asset rename undo");
    require(session.snapshot().project().findAsset(a)->name == "First plate",
            "asset rename undo projection");
    rename(*tree, find(*tree, ui::assets::kFolderRole, folder.value()), "Production");
    require(session.snapshot().project().findAssetFolder(folder)->name == "Production",
            "inline folder rename");
    const auto beforeCancel = session.snapshot().revision();
    rename(*tree, find(*tree, ui::assets::kAssetRole, a.value()), "Discard", true);
    require(session.snapshot().revision() == beforeCancel &&
                tree->itemWidget(find(*tree, ui::assets::kAssetRole, a.value()), 0),
            "cancelled rename restores row without authoring");
    bool editedDialog = false;
    QTimer::singleShot(0, &editor, [&] {
        auto* dialog = editor.findChild<QDialog*>("assetsTagsDialog");
        if (!dialog)
            return;
        const auto fields = dialog->findChildren<QLineEdit*>("assetsTagField");
        if (fields.size() == 2) {
            fields[0]->setText("review");
            fields[1]->setText("hero");
            editedDialog = true;
        }
        if (auto* apply = dialog->findChild<ui::kit::KButton*>("assetsApplyTagsButton"))
            apply->click();
        else
            dialog->reject();
    });
    assetRow = qobject_cast<ui::kit::KRow*>(
        tree->itemWidget(find(*tree, ui::assets::kAssetRole, a.value()), 0));
    auto* more = assetRow->findChild<ui::kit::KButton*>("assetsMoreTagsChip");
    require(more, "extra tags chip");
    more->click();
    require(editedDialog && session.snapshot().project().findAsset(a)->tags ==
                                std::vector<std::string>{"hero", "review"},
            "tag dialog authors normalized tags");
    auto mime = mimeFor(*tree, find(*tree, ui::assets::kAssetRole, b.value()));
    require(mime->hasFormat(ui::assets::kInternalMimeType) &&
                mime->hasFormat("application/x-bloom-asset"),
            "internal and canvas MIME coexist");
    const auto destination =
        tree->visualItemRect(find(*tree, ui::assets::kFolderRole, nested.value())).center();
    require(drop(*tree, *mime, destination), "internal drop moves asset into folder");
    require(session.snapshot().project().findAsset(b)->folder == nested &&
                session.snapshot().project().findAsset(b)->order == 1,
            "drop changes folder and appends after current assets");
    const auto movedRevision = session.snapshot().revision();
    require(!drop(*tree, *mime, destination) && session.snapshot().revision() == movedRevision,
            "stale drag refused");
    mime = mimeFor(*tree, find(*tree, ui::assets::kAssetRole, b.value()));
    const auto targetRect = tree->visualItemRect(find(*tree, ui::assets::kAssetRole, a.value()));
    require(drop(*tree, *mime, QPoint(targetRect.center().x(), targetRect.top() + 1)),
            "internal drop reorders within folder");
    require(session.snapshot().project().findAsset(b)->order == 0 &&
                session.snapshot().project().findAsset(a)->order == 1,
            "drop before sibling persists its new index");
    const auto snapshot = session.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    require(static_cast<bool>(archive), "save UI organization");
    auto opened = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    require(opened.outcome() == project::OpenArchiveOutcome::Opened, "reopen UI organization");
    auto restored = std::move(opened).takeOpened();
    require(restored.document->snapshot().project().findAsset(b)->order == 0 &&
                restored.document->snapshot().project().findAsset(a)->tags ==
                    std::vector<std::string>{"hero", "review"},
            "drag reorder and edited tags persist through save/open");
    tree->setCurrentItem(find(*tree, ui::assets::kAssetRole, c.value()));
    auto* newFolder = editor.findChild<QAction*>("assetsNewFolderAction");
    require(newFolder && newFolder->isEnabled(), "header folder action live");
    newFolder->trigger();
    QApplication::processEvents();
    auto* field = tree->findChild<QLineEdit*>("assetsRenameField");
    require(field, "new folder immediately renames in place");
    QTest::keyClick(field, Qt::Key_Escape);
    QApplication::processEvents();
    require(session.snapshot().project().assetFolders().size() == 3, "header creates folder");
    require(session.undo() && session.snapshot().project().assetFolders().size() == 2,
            "folder creation undo");
    tree->expandAll();
    editor.resize(300, editor.height());
    QApplication::processEvents();
    assetRow = qobject_cast<ui::kit::KRow*>(
        tree->itemWidget(find(*tree, ui::assets::kAssetRole, a.value()), 0));
    auto* compact = assetRow->findChild<ui::kit::KButton*>("assetsCompactTagsChip");
    require(compact && compact->isVisible() &&
                !assetRow->findChild<ui::kit::KButton*>("assetsTagChip")->isVisible(),
            "narrow rows use a compact tag count");
    require(compact->width() >= compact->sizeHint().width(),
            "compact tag count has room for its caption");
    require(assetRow->findChildren<ui::kit::KLabel*>().front()->width() >
                ui::kit::px(ui::kit::Size::IconControl),
            "narrow tagged row preserves visible asset-name space");
    editor.resize(620, editor.height());
    QApplication::processEvents();
    require(!compact->isVisible() &&
                assetRow->findChild<ui::kit::KButton*>("assetsTagChip")->isVisible(),
            "wide rows restore named tag chips");
    if (const auto capture = qEnvironmentVariable("BLOOM_ASSETS_TEST_CAPTURE");
        !capture.isEmpty()) {
        const auto width = qEnvironmentVariableIntValue("BLOOM_ASSETS_TEST_WIDTH");
        if (width > 0)
            editor.resize(width, editor.height());
        tree->expandAll();
        QApplication::processEvents();
        require(editor.grab().save(capture), "asset panel QA capture");
    }
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::ui::kit::installKinetikTheme(app);
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
