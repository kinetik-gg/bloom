#include "editor_chrome_test_support.hpp"
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
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
    context.expect(tree->topLevelItemCount() == 1, "new project exposes one composition");
    context.expect(editor.findChild<QMenu*>(QStringLiteral("assetsViewMenu")) != nullptr,
                   "assets View menu exists");
    context.expect(editor.findChild<QMenu*>(QStringLiteral("assetsAddMenu")) != nullptr,
                   "assets Add menu exists");
    context.expect(editor.findChild<QMenu*>(QStringLiteral("assetsSelectMenu")) != nullptr,
                   "assets Select menu exists");

    auto* newFolder = editor.findChild<QAction*>(QStringLiteral("assetsNewFolderAction"));
    auto* import =
        editor.findChild<bloom::ui::kit::KIconButton*>(QStringLiteral("assetsImportButton"));
    context.expect(newFolder != nullptr && !newFolder->isEnabled(),
                   "header New Folder is disabled");
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
    context.expect(tree->topLevelItemCount() == 2, "assets tree rebuilds after add");

    search->setText(QStringLiteral("Second"));
    QApplication::processEvents();
    int visibleCount = 0;
    QTreeWidgetItem* visibleItem = nullptr;
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        auto* item = tree->topLevelItem(index);
        if (!item->isHidden()) {
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

    return context.ok ? 0 : 1;
}
