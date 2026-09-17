#include "command_test_support.hpp"
#include <bloom/commands/asset_operations.hpp>
#include <bloom/document/new_project.hpp>
#include <limits>

namespace bloom::commands::test {
namespace {
using document::AssetFolderId;
using document::AssetId;
using Assets = std::vector<AssetId>;
template <class Operation, class... Args>
CommandResult execute(Document& document, CommandStack& stack, Args&&... args) {
    Transaction transaction("Organize Assets", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Args>(args)...);
    return stack.execute(std::move(transaction));
}
void testOrganization(TestContext& test) {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    const auto composition = initial.initialCompositionId;
    const auto a = AssetId::fromRaw(1), b = AssetId::fromRaw(2), c = AssetId::fromRaw(3);
    for (const auto id : {a, b, c}) {
        document::AssetRecord asset;
        asset.id = id;
        asset.locator = {"file", "project-relative", "media/" + std::to_string(id.value()) + ".png",
                         "file:///media/image.png"};
        asset.width = 4;
        asset.height = 3;
        asset.order = id.value() - 1;
        requireFixture(initial.project.addAsset(asset), "asset fixture");
    }
    Document document(std::move(initial.project));
    CommandStack stack(document);
    test.expect(execute<AddImageLayer>(document, stack, composition, a).changed(),
                "asset source fixture");
    const auto original = document.snapshot();
    const auto created = execute<CreateAssetFolder>(document, stack, "Shots");
    const auto folder =
        created.outputId<AssetFolderId>(kCreateAssetFolderOutput).value_or(AssetFolderId{});
    test.expect(created.changed() && folder.isValid(), "create folder returns allocated identity");
    test.expect(stack.undo().changed() && document.snapshot().project().assetFolders().empty(),
                "create undo removes folder");
    test.expect(stack.redo().changed() && document.snapshot().project().findAssetFolder(folder),
                "create redo keeps folder identity");
    test.expect(execute<CreateAssetFolder>(document, stack, "Shots").status ==
                    CommandStatus::Rejected,
                "duplicate root folder refused");
    test.expect(execute<CreateAssetFolder>(document, stack, "", folder).status ==
                    CommandStatus::Rejected,
                "empty folder refused");
    test.expect(
        execute<CreateAssetFolder>(document, stack, "Child", AssetFolderId::fromRaw(999)).status ==
            CommandStatus::Rejected,
        "missing parent refused");
    const auto child = execute<CreateAssetFolder>(document, stack, "Child", folder)
                           .outputId<AssetFolderId>(kCreateAssetFolderOutput)
                           .value_or(AssetFolderId{});
    test.expect(child.isValid(), "nested folder created");
    test.expect(execute<RenameAssetFolder>(document, stack, folder, "Production").changed(),
                "folder rename publishes");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findAssetFolder(folder)->name == "Shots",
                "folder rename undo");
    test.expect(stack.redo().changed(), "folder rename redo");
    test.expect(execute<MoveAssets>(document, stack, Assets{a, c}, folder, 0).changed(),
                "bulk move publishes once");
    auto moved = document.snapshot();
    test.expect(moved.project().findAsset(a)->folder == folder &&
                    moved.project().findAsset(c)->order == 1 &&
                    moved.project().findAsset(b)->order == 0,
                "move preserves input order and compacts source folder");
    const auto& beforeComposition = *original.project().findComposition(composition);
    const auto& afterComposition = *moved.project().findComposition(composition);
    test.expect(
        std::ranges::equal(beforeComposition.graph().nodes(), afterComposition.graph().nodes()) &&
            std::ranges::equal(beforeComposition.parameters().records(),
                               afterComposition.parameters().records()),
        "moving assets preserves referencing node identities and asset parameters");
    test.expect(stack.undo().changed() && !document.snapshot().project().findAsset(a)->folder &&
                    document.snapshot().project().findAsset(c)->order == 2,
                "move undo restores source positions");
    test.expect(stack.redo().changed(), "move redo");
    test.expect(execute<ReorderAssets>(document, stack, folder, Assets{c, a}).changed(),
                "reorder publishes");
    test.expect(document.snapshot().project().findAsset(c)->order == 0,
                "reorder stores stable positions");
    test.expect(stack.undo().changed() && document.snapshot().project().findAsset(a)->order == 0,
                "reorder undo");
    test.expect(stack.redo().changed(), "reorder redo");
    const auto beforeInvalid = document.snapshot();
    for (const auto& ids : {Assets{a, a}, Assets{a}, Assets{a, b}})
        test.expect(execute<ReorderAssets>(document, stack, folder, ids).status ==
                        CommandStatus::Rejected,
                    "invalid permutation rejected");
    test.expect(execute<MoveAssets>(document, stack, Assets{a}, folder, 99).status ==
                    CommandStatus::Rejected,
                "out-of-range move rejected");
    test.expect(execute<MoveAssets>(document, stack, Assets{a, a}, std::nullopt, 0).status ==
                    CommandStatus::Rejected,
                "duplicate move identity rejected");
    test.expect(document.snapshot().revision() == beforeInvalid.revision(),
                "invalid organization leaves project revision unchanged");
    test.expect(execute<RenameAsset>(document, stack, a, "Hero plate").changed(),
                "asset rename publishes");
    test.expect(stack.undo().changed() && document.snapshot().project().findAsset(a)->name == "1",
                "asset rename undo restores lexical default");
    test.expect(stack.redo().changed(), "asset rename redo");
    test.expect(execute<SetAssetTags>(document, stack, Assets{a, c},
                                      std::vector<std::string>{"hero", "approved", "hero"})
                    .changed(),
                "batch tags publish");
    test.expect(document.snapshot().project().findAsset(a)->tags ==
                    std::vector<std::string>{"approved", "hero"},
                "tags normalized to sorted unique strings");
    test.expect(stack.undo().changed() && document.snapshot().project().findAsset(a)->tags.empty(),
                "tag undo");
    test.expect(stack.redo().changed(), "tag redo");
    test.expect(execute<SetAssetTags>(document, stack, a, std::vector<std::string>{""}).status ==
                    CommandStatus::Rejected,
                "empty tag refused");
    test.expect(
        execute<SetAssetTags>(document, stack, a, std::vector<std::string>{std::string(257, 'x')})
                .status == CommandStatus::Rejected,
        "oversized tag refused");
    test.expect(execute<RenameAsset>(document, stack, a, "").status == CommandStatus::Rejected,
                "empty asset name refused");
    test.expect(execute<RemoveAssetFolder>(document, stack, folder).changed(),
                "folder removal publishes");
    auto removed = document.snapshot();
    test.expect(!removed.project().findAssetFolder(folder) &&
                    !removed.project().findAssetFolder(child)->parent &&
                    !removed.project().findAsset(a)->folder &&
                    removed.project().findAsset(b)->order == 0 &&
                    removed.project().findAsset(c)->order == 1 &&
                    removed.project().findAsset(a)->order == 2,
                "folder removal promotes child folders and appends assets in their stored order");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findAssetFolder(child)->parent == folder &&
                    document.snapshot().project().findAsset(a)->folder == folder,
                "folder removal undo restores hierarchy");
    test.expect(stack.redo().changed(), "folder removal redo");
    const auto nextFolder = execute<CreateAssetFolder>(document, stack, "Next")
                                .outputId<AssetFolderId>(kCreateAssetFolderOutput)
                                .value_or(AssetFolderId{});
    test.expect(nextFolder.value() > child.value(), "deleted folder identity is never reused");
    test.expect(execute<CreateAssetFolder>(document, stack, "Child", nextFolder).changed(),
                "collision fixture child");
    const auto beforeCollision = document.snapshot();
    test.expect(execute<RemoveAssetFolder>(document, stack, nextFolder).status ==
                        CommandStatus::Rejected &&
                    document.snapshot().revision() == beforeCollision.revision(),
                "promotion name collision refuses atomically");
}
void testRelinkAndSaturatedOrder(TestContext& test) {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    document::AssetRecord existing;
    existing.id = AssetId::fromRaw(1);
    existing.locator = {"file", "project-relative", "last.png", "file:///last.png"};
    existing.width = 1;
    existing.height = 1;
    existing.order = std::numeric_limits<std::uint64_t>::max();
    requireFixture(initial.project.addAsset(existing), "saturated asset order fixture");
    Document document(std::move(initial.project));
    CommandStack stack(document);
    document::AssetRecord font;
    font.kind = document::AssetKind::Font;
    font.name = "Picked face";
    font.locator = {"font", "builtin", "", "font:fixture"};
    font.fontFamily = "Fixture";
    font.fontStyle = "Regular";
    const auto imported = execute<EnsureFontAsset>(document, stack, font)
                              .outputId<AssetId>("asset")
                              .value_or(AssetId{});
    test.expect(
        imported.value() > existing.id.value() &&
            document.snapshot().project().findAsset(imported)->order == existing.order,
        "import after maximum order uses the stable ID tie-break instead of wrapping to the front");
    const auto folder = execute<CreateAssetFolder>(document, stack, "Fonts")
                            .outputId<AssetFolderId>(kCreateAssetFolderOutput)
                            .value_or(AssetFolderId{});
    test.expect(execute<MoveAssets>(document, stack, Assets{imported}, folder, 0).changed(),
                "move font into folder");
    test.expect(execute<SetAssetTags>(document, stack, imported, std::vector<std::string>{"brand"})
                    .changed(),
                "tag font fixture");
    const auto organized = *document.snapshot().project().findAsset(imported);
    font.name = "Replacement default";
    font.fontStyle = "Italic";
    test.expect(execute<RelinkFontAsset>(document, stack, imported, font).changed(),
                "font relink publishes");
    const auto relinked = *document.snapshot().project().findAsset(imported);
    test.expect(relinked.id == organized.id && relinked.name == organized.name &&
                    relinked.tags == organized.tags && relinked.folder == organized.folder &&
                    relinked.order == organized.order && relinked.fontStyle == "Italic",
                "relink changes source metadata while preserving every organization field");
    test.expect(stack.undo().changed() &&
                    *document.snapshot().project().findAsset(imported) == organized,
                "relink undo restores source and organization");
}
} // namespace
} // namespace bloom::commands::test
int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testOrganization(test);
        bloom::commands::test::testRelinkAndSaturatedOrder(test);
    } catch (const std::exception& error) {
        test.fail(error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
