#include "../asset_input_picker_guard.hpp"
#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace bloom;
namespace assets = ui::assets;
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
QPoint centre(QTreeWidget& tree, QTreeWidgetItem* item) {
    tree.scrollToItem(item);
    return tree.visualItemRect(item).center();
}
QPoint emptySpace(QTreeWidget& tree) { return {4, tree.viewport()->height() - 4}; }
std::unique_ptr<QMimeData> mimeFor(QTreeWidget& tree, const QList<QTreeWidgetItem*>& items) {
    tree.clearSelection();
    QModelIndexList indexes;
    for (auto* item : items) {
        item->setSelected(true);
        indexes.push_back(tree.indexFromItem(item));
    }
    tree.setCurrentItem(items.front(), 0, QItemSelectionModel::NoUpdate);
    return std::unique_ptr<QMimeData>(tree.model()->mimeData(indexes));
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
void rename(QTreeWidget& tree, QTreeWidgetItem* item, const QString& name, bool cancel = false,
            Qt::Key key = Qt::Key_F2) {
    tree.setCurrentItem(item);
    tree.setFocus();
    QTest::keyClick(&tree, key);
    QApplication::processEvents();
    auto* field = tree.findChild<QLineEdit*>("assetsRenameField");
    require(field, "inline rename editor opens");
    field->setText(name);
    QTest::keyClick(field, cancel ? Qt::Key_Escape : Qt::Key_Return);
    QApplication::processEvents();
    QApplication::processEvents();
}
// Answers the delete confirmation with one of its buttons (by object name), or Cancel.
class Confirmation final {
  public:
    explicit Confirmation(QString button) : button_(std::move(button)) {
        timer_.setInterval(5);
        QObject::connect(&timer_, &QTimer::timeout, [this] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (box == nullptr || !box->isVisible() ||
                    box->objectName() != QStringLiteral("assetsDeleteConfirmation"))
                    continue;
                ++shown_;
                text_ = box->text();
                if (auto* chosen = box->findChild<QAbstractButton*>(button_);
                    chosen && !button_.isEmpty())
                    chosen->click();
                else
                    box->reject();
            }
        });
        timer_.start();
    }
    [[nodiscard]] int shown() const noexcept { return shown_; }
    [[nodiscard]] const QString& text() const noexcept { return text_; }

  private:
    QString button_;
    QTimer timer_;
    int shown_ = 0;
    QString text_;
};
void pressDelete(QTreeWidget& tree, const QList<QTreeWidgetItem*>& items) {
    tree.clearSelection();
    for (auto* item : items)
        item->setSelected(true);
    tree.setCurrentItem(items.front(), 0, QItemSelectionModel::NoUpdate);
    tree.setFocus();
    QTest::keyClick(&tree, Qt::Key_Delete);
    QApplication::processEvents();
}
// Triggers one action of the context menu for `item`.
void contextAction(QTreeWidget& tree, QTreeWidgetItem* item, const QString& name) {
    bool found = false;
    QTimer::singleShot(0, &tree, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr)
            return;
        QAction* chosen = nullptr;
        for (auto* action : menu->actions())
            if (action->objectName() == name)
                chosen = action;
        menu->close();
        if (chosen) {
            found = true;
            chosen->trigger();
        }
    });
    emit tree.customContextMenuRequested(centre(tree, item));
    QApplication::processEvents();
    require(found, "context menu action exists");
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
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 15000)
        QTest::qWait(5);
    return predicate();
}

void run() {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    const auto shots = document::AssetFolderId::fromRaw(1);
    const auto plates = document::AssetFolderId::fromRaw(2);
    require(initial.project.addAssetFolder({shots, "Shots", {}}), "folder fixture");
    require(initial.project.addAssetFolder({plates, "Plates", shots}), "nested folder fixture");
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
        asset.folder = id == a ? std::optional(plates) : std::nullopt;
        if (id == a)
            asset.tags = {"approved", "hero"};
        asset.order = id == c ? 1 : 0;
        require(initial.project.addAsset(asset), "asset fixture");
    }
    document::Document document(std::move(initial.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, initial.initialCompositionId);
    const auto main = initial.initialCompositionId;
    {
        commands::Transaction fixture("Fixture", session.snapshot().revision());
        fixture.emplace<commands::AddComposition>("Edit", document::CompositionFormat{},
                                                  core::RationalTime::fromInteger(4));
        const auto added = session.executeTransaction(std::move(fixture));
        const auto edit = added.outputId<document::CompositionId>(commands::kAddCompositionOutput);
        require(edit.has_value(), "second composition fixture");
        commands::Transaction file("Fixture", session.snapshot().revision());
        file.emplace<commands::MoveCompositions>(std::vector{*edit}, shots);
        file.emplace<commands::AddImageLayer>(main, b);
        require(session.executeTransaction(std::move(file)).succeeded(),
                "file the composition and use one asset");
    }
    const auto edit =
        std::ranges::find_if(session.snapshot().project().compositions(),
                             [](const auto& composition) { return composition.name() == "Edit"; })
            ->id();
    const auto inputSnapshot = session.snapshot();
    const auto originalInput = inputSnapshot.project().findAsset(a)->interpretation;
    auto unrelatedEdit = inputSnapshot.project();
    unrelatedEdit.setName("Renamed project");
    require(ui::detail::assetInputPickerTargetMatches(inputSnapshot.project().id(), a,
                                                      originalInput, unrelatedEdit),
            "unrelated edit does not invalidate asset input picker");
    auto conflictingEdit = unrelatedEdit;
    conflictingEdit.findAsset(a)->interpretation.colorSpace = document::AssetColorSpace::Raw;
    require(!ui::detail::assetInputPickerTargetMatches(inputSnapshot.project().id(), a,
                                                       originalInput, conflictingEdit),
            "concurrent input edit invalidates asset input picker");
    ui::AssetsEditor editor(session);
    editor.resize(720, 640);
    editor.show();
    QApplication::processEvents();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    auto* search = editor.findChild<QLineEdit*>("assetsSearchField");
    require(tree && search, "Assets controls");
    tree->header()->setSectionHidden(assets::TagsColumn, false);
    const auto item = [&](int role, std::uint64_t id) { return find(*tree, role, id); };
    auto* shotsItem = item(assets::kFolderRole, shots.value());
    auto* platesItem = item(assets::kFolderRole, plates.value());
    require(platesItem->parent() == shotsItem, "nested folder projection");
    require(item(assets::kAssetRole, a.value())->parent() == platesItem, "asset under its folder");
    require(item(assets::kCompositionRole, edit.value())->parent() == shotsItem,
            "a composition sits in its folder");
    require(item(assets::kCompositionRole, main.value())->parent() == nullptr,
            "an unfiled composition sits at the root");
    require(item(assets::kAssetRole, a.value())->text(assets::TagsColumn) == "approved, hero",
            "Tags column lists the sorted tags");

    // Search: tags and names, matching ancestors open, the folder's own choice comes back.
    shotsItem->setExpanded(false);
    search->setText("tag:HERO");
    require(!item(assets::kAssetRole, a.value())->isHidden() &&
                item(assets::kAssetRole, b.value())->isHidden() && shotsItem->isExpanded(),
            "tag filter matches tags only and exposes matching ancestors");
    search->clear();
    require(!item(assets::kFolderRole, shots.value())->isExpanded(),
            "clearing filter restores expansion choice");
    search->setText("approved");
    require(!item(assets::kAssetRole, a.value())->isHidden() &&
                item(assets::kAssetRole, c.value())->isHidden(),
            "plain search matches tags");
    search->clear();
    tree->expandAll();
    auto* aItem = item(assets::kAssetRole, a.value());
    emit tree->itemClicked(aItem, assets::TagsColumn);
    require(search->text() == "tag:approved", "a tag cell filters by its first tag");
    search->clear();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      centre(*tree, item(assets::kAssetRole, c.value())));
    require(tree->currentItem() == item(assets::kAssetRole, c.value()) &&
                tree->currentItem()->isSelected(),
            "a click selects the row");
    QTest::keyClick(tree, Qt::Key_Up);
    require(tree->currentItem() != item(assets::kAssetRole, c.value()),
            "arrow keys move through the tree");

    // Inline rename: F2 and Return, every kind, with Escape authoring nothing.
    rename(*tree, item(assets::kAssetRole, a.value()), "Hero plate");
    require(session.snapshot().project().findAsset(a)->name == "Hero plate",
            "inline rename authors asset name");
    require(session.undo(), "asset rename undo");
    require(session.snapshot().project().findAsset(a)->name == "First plate",
            "asset rename undo projection");
    rename(*tree, item(assets::kFolderRole, shots.value()), "Production", false, Qt::Key_Return);
    require(session.snapshot().project().findAssetFolder(shots)->name == "Production",
            "Return renames a folder in place");
    rename(*tree, item(assets::kCompositionRole, edit.value()), "Cut");
    require(session.snapshot().project().findComposition(edit)->name() == "Cut",
            "inline composition rename");
    const auto beforeCancel = session.snapshot().revision();
    rename(*tree, item(assets::kAssetRole, a.value()), "Discard", true);
    require(session.snapshot().revision() == beforeCancel &&
                item(assets::kAssetRole, a.value())->text(assets::NameColumn) == "First plate",
            "cancelled rename restores row without authoring");

    // Edit Tags from the context menu.
    bool editedDialog = false;
    QTimer::singleShot(50, &editor, [&] {
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
    contextAction(*tree, item(assets::kAssetRole, a.value()), "assetsEditTagsAction");
    require(editedDialog && session.snapshot().project().findAsset(a)->tags ==
                                std::vector<std::string>{"hero", "review"},
            "tag dialog authors normalized tags");

    // Drag out: the canvas payloads keep their formats.
    auto mime = mimeFor(*tree, {item(assets::kAssetRole, b.value())});
    require(mime->hasFormat(assets::kInternalMimeType) && mime->hasFormat(ui::kAssetMimeType),
            "internal and canvas MIME coexist");
    auto compositionMime = mimeFor(*tree, {item(assets::kCompositionRole, main.value())});
    require(compositionMime->hasFormat(ui::kCompositionMimeType) &&
                !compositionMime->hasFormat(ui::kAssetMimeType),
            "a composition drags out with the composition payload");

    // Drag in: assets, compositions and folders move between folders.
    require(drop(*tree, *mime, centre(*tree, item(assets::kFolderRole, plates.value()))),
            "an asset drops into a folder");
    require(session.snapshot().project().findAsset(b)->folder == plates &&
                session.snapshot().project().findAsset(b)->order == 1,
            "the drop files the asset after the folder's assets");
    const auto movedRevision = session.snapshot().revision();
    require(!drop(*tree, *mime, centre(*tree, item(assets::kFolderRole, shots.value()))) &&
                session.snapshot().revision() == movedRevision,
            "a stale drag is refused");
    mime = mimeFor(
        *tree, {item(assets::kAssetRole, c.value()), item(assets::kCompositionRole, main.value())});
    require(drop(*tree, *mime, centre(*tree, item(assets::kAssetRole, a.value()))),
            "a mixed selection drops onto a row inside a folder");
    require(session.snapshot().project().findAsset(c)->folder == plates &&
                session.snapshot().project().findComposition(main)->folder() == plates,
            "dropping on a row files into that row's folder");
    require(session.undo() && !session.snapshot().project().findAsset(c)->folder &&
                !session.snapshot().project().findComposition(main)->folder(),
            "one undo returns the whole drag");
    mime = mimeFor(*tree, {item(assets::kFolderRole, shots.value())});
    require(!drop(*tree, *mime, centre(*tree, item(assets::kFolderRole, plates.value()))),
            "a folder never drops into its own subfolder");
    mime = mimeFor(*tree, {item(assets::kFolderRole, plates.value())});
    require(drop(*tree, *mime, emptySpace(*tree)) &&
                !session.snapshot().project().findAssetFolder(plates)->parent,
            "a folder dropped on empty space moves to the root");
    require(session.undo() && session.snapshot().project().findAssetFolder(plates)->parent == shots,
            "folder move undo");

    // Delete: unused items go at once; a used one asks first.
    {
        Confirmation answer({});
        pressDelete(*tree, {item(assets::kAssetRole, c.value())});
        require(answer.shown() == 0 && !session.snapshot().project().findAsset(c),
                "an unused asset deletes without a warning");
        require(session.undo(), "unused delete undo");
    }
    {
        Confirmation answer({});
        pressDelete(*tree, {item(assets::kAssetRole, b.value())});
        require(answer.shown() == 1 && answer.text().contains("used once in 1 composition") &&
                    session.snapshot().project().findAsset(b),
                "deleting a used asset warns with its usage, and Cancel keeps it");
    }
    {
        Confirmation answer(QStringLiteral("assetsDeleteConfirmButton"));
        pressDelete(*tree, {item(assets::kAssetRole, b.value())});
        require(answer.shown() == 1 && !session.snapshot().project().findAsset(b),
                "confirming deletes the used asset");
        require(session.undo() && session.snapshot().project().findAsset(b),
                "used delete undo restores it");
    }
    {
        Confirmation answer(QStringLiteral("assetsDeleteFoldersOnlyButton"));
        pressDelete(*tree, {item(assets::kFolderRole, shots.value())});
        const auto& project = session.snapshot().project();
        require(answer.shown() == 1 && !project.findAssetFolder(shots) &&
                    project.findAssetFolder(plates) && !project.findAssetFolder(plates)->parent &&
                    !project.findComposition(edit)->folder(),
                "Delete Folders Only moves the contents up");
        require(session.undo(), "folder-only delete undo");
    }
    {
        Confirmation answer(QStringLiteral("assetsDeleteConfirmButton"));
        pressDelete(*tree, {item(assets::kFolderRole, shots.value())});
        const auto& project = session.snapshot().project();
        require(answer.shown() == 1 && !project.findAssetFolder(shots) &&
                    !project.findAssetFolder(plates) && !project.findAsset(a) &&
                    !project.findAsset(b) && !project.findComposition(edit),
                "Delete with Contents removes the folder, its subfolder and everything inside");
        require(session.undo() && session.snapshot().project().findComposition(edit) &&
                    session.snapshot().project().findComposition(edit)->folder() == shots &&
                    session.snapshot().project().findAsset(a)->folder == plates,
                "one undo restores the whole folder");
    }

    // Header New Folder nests under the selected item's folder and renames in place.
    tree->clearSelection();
    tree->setCurrentItem(item(assets::kAssetRole, a.value()));
    auto* newFolder = editor.findChild<QAction*>("assetsNewFolderAction");
    require(newFolder && newFolder->isEnabled(), "header folder action live");
    newFolder->trigger();
    QApplication::processEvents();
    auto* field = tree->findChild<QLineEdit*>("assetsRenameField");
    require(field, "new folder immediately renames in place");
    QTest::keyClick(field, Qt::Key_Escape);
    QApplication::processEvents();
    require(session.snapshot().project().assetFolders().size() == 3 &&
                std::ranges::any_of(session.snapshot().project().assetFolders(),
                                    [&](const auto& folder) { return folder.parent == plates; }),
            "header creates the folder inside the selected item's folder");
    require(session.undo() && session.snapshot().project().assetFolders().size() == 2,
            "folder creation undo");

    // View > Collapse All is remembered across a rebuild like a folder collapsed by hand.
    auto* collapseAll = editor.findChild<QAction*>("assetsCollapseAllAction");
    require(collapseAll != nullptr, "Collapse All action");
    collapseAll->trigger();
    {
        commands::Transaction touch("Rename", session.snapshot().revision());
        touch.emplace<commands::RenameAsset>(c, "Other plate v2");
        require(session.executeTransaction(std::move(touch)).succeeded(), "rebuild fixture");
    }
    require(!item(assets::kFolderRole, shots.value())->isExpanded() &&
                !item(assets::kFolderRole, plates.value())->isExpanded(),
            "folders stay collapsed after a rebuild");
    require(session.undo(), "rebuild fixture undo");

    // The organisation survives save and open.
    const auto snapshot = session.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    require(static_cast<bool>(archive), "save UI organization");
    auto opened = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    require(opened.outcome() == project::OpenArchiveOutcome::Opened, "reopen UI organization");
    auto restored = std::move(opened).takeOpened();
    const auto& reopened = restored.document->snapshot().project();
    require(reopened.findComposition(edit)->folder() == shots &&
                reopened.findAsset(a)->tags == std::vector<std::string>{"hero", "review"},
            "composition folders and edited tags persist through save/open");
    if (const auto capture = qEnvironmentVariable("BLOOM_ASSETS_TEST_CAPTURE");
        !capture.isEmpty()) {
        tree->expandAll();
        QApplication::processEvents();
        require(editor.grab().save(capture), "asset panel QA capture");
    }
}

// Imports land in the selected folder: File > Import and the footer through the controller's
// import folder, and file drops in the folder they are dropped on.
void importIntoSelectedFolder() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary import directory");
    QImage image(8, 8, QImage::Format_RGBA8888);
    for (const auto* name : {"one.png", "two.png", "three.png"}) {
        image.fill(QString(name) == "two.png" ? Qt::black : Qt::white);
        require(image.save(directory.filePath(name)), "generate PNG fixture");
    }
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);
    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    ui::TaskUiBridge bridge(scheduler);
    ui::AssetController controller(session, host, scheduler, bridge);
    commands::Transaction create("Folders", session.snapshot().revision());
    create.emplace<commands::CreateAssetFolder>("Footage");
    const auto folder = session.executeTransaction(std::move(create))
                            .outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    require(folder.has_value(), "import folder fixture");
    ui::AssetsEditor editor(session);
    editor.resize(640, 480);
    editor.show();
    QApplication::processEvents();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    require(tree != nullptr, "import tree");
    tree->setCurrentItem(find(*tree, assets::kFolderRole, folder->value()));
    require(controller.importFolder() == folder, "selecting a folder makes it the import folder");
    const auto imported = [&](const char* name) {
        for (const auto& asset : session.snapshot().project().assets())
            if (asset.locator.path.ends_with(name))
                return std::optional(asset);
        return std::optional<document::AssetRecord>{};
    };
    controller.importFiles({directory.filePath("one.png")});
    require(waitUntil([&] { return !controller.busy() && imported("one.png"); }),
            "selected-folder import completes");
    require(imported("one.png")->folder == folder, "File > Import lands in the selected folder");
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(directory.filePath("two.png"))});
    require(drop(*tree, files, centre(*tree, find(*tree, assets::kFolderRole, folder->value()))),
            "files drop onto a folder row");
    require(waitUntil([&] { return !controller.busy() && imported("two.png"); }),
            "folder drop import completes");
    require(imported("two.png")->folder == folder && imported("two.png")->order == 1,
            "a file dropped on a folder lands in it after its assets");
    QMimeData rootFiles;
    rootFiles.setUrls({QUrl::fromLocalFile(directory.filePath("three.png"))});
    require(drop(*tree, rootFiles, emptySpace(*tree)), "files drop onto empty space");
    require(waitUntil([&] { return !controller.busy() && imported("three.png"); }),
            "root drop import completes");
    require(!imported("three.png")->folder, "a file dropped on empty space lands at the root");
    // Dragging out: Timeline, Viewer and Nodes read the canvas payload of the first asset.
    auto* row = find(*tree, assets::kAssetRole, imported("two.png")->id.value());
    auto mime = mimeFor(*tree, {row});
    require(ui::assetFromMime(*mime, session) == imported("two.png")->id,
            "an asset drags to Timeline, Viewer and Nodes with the asset payload");
    tree->clearSelection();
    require(!controller.importFolder(), "no selection imports at the root");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomAssetOrganizationTests"));
    QApplication::setApplicationName(QStringLiteral("AssetOrganization"));
    QStandardPaths::setTestModeEnabled(true);
    bloom::ui::kit::installKinetikTheme(app);
    try {
        run();
        importIntoSelectedFolder();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
