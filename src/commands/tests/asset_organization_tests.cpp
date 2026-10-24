#include "command_test_support.hpp"
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>

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
void testLutImport([[maybe_unused]] TestContext& test) {
#ifdef __linux__
    const auto directory = std::filesystem::current_path() / "color3-import-fixtures";
    std::filesystem::create_directories(directory);
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    const auto path = directory / "show.cube";
    {
        std::ofstream out(path);
        out << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    }
    auto initial =
        document::makeNewProject("LUT import", "Main", core::RationalTime::fromInteger(1));
    Document document(std::move(initial.project));
    CommandStack stack(document);
    auto prepared =
        std::make_unique<ImportAssets>(std::vector<std::filesystem::path>{path}, directory);
    Transaction transaction("Import LUT", document.snapshot().revision());
    requireFixture(transaction.add(std::move(prepared)), "prepared LUT operation");
    const auto result = stack.execute(std::move(transaction));
    test.expect(result.changed() && document.snapshot().project().assets().size() == 1,
                "LUT import publishes one asset");
    const auto snapshot = document.snapshot();
    if (!snapshot.project().assets().empty()) {
        const auto& asset = snapshot.project().assets().front();
        test.expect(asset.kind == document::AssetKind::Lut && asset.locator.path == "show.cube" &&
                        asset.contentDigest != core::Sha256Digest{},
                    "LUT import retains locator and exact content digest");
    }
    test.expect(stack.undo().changed() && document.snapshot().project().assets().empty(),
                "LUT import undo");
    test.expect(stack.redo().changed(), "LUT import redo");
    const auto brokenAudio = directory / "broken.wav";
    {
        std::ofstream out(brokenAudio, std::ios::binary);
        out << "not audio";
    }
    ImportAssets invalidAudio({brokenAudio}, directory);
    test.expect(invalidAudio.prepared().empty() &&
                    invalidAudio.diagnostic().find("supported WAV or MP3") != std::string::npos,
                "audio import retains the typed probe reason for the UI");
#endif
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
void testWideImageAssetCommand(TestContext& test) {
    auto initial =
        document::makeNewProject("Wide source", "Main", core::RationalTime::fromInteger(24));
    const auto source = AssetId::fromRaw(1);
    document::AssetRecord image;
    image.id = source;
    image.locator = {"file", "project-relative", "plate.exr", "file:///plate.exr"};
    image.width = 6000;
    image.height = 4000;
    test.expect(initial.project.addAsset(image), "24 Mpixel metadata enters project truth");
    Document document(std::move(initial.project));
    CommandStack stack(document);
    test.expect(
        execute<AddImageLayer>(document, stack, initial.initialCompositionId, source).changed(),
        "image layer command accepts large source metadata without decoding pixels");
}
void testAssetInputColor(TestContext& test) {
    auto initial =
        document::makeNewProject("Input colour", "Main", core::RationalTime::fromInteger(1));
    document::AssetRecord asset;
    asset.id = AssetId::fromRaw(1);
    asset.kind = document::AssetKind::Image;
    asset.locator = {"file", "project-relative", "media/test.png", "file:///media/test.png"};
    asset.width = 4;
    asset.height = 3;
    requireFixture(initial.project.addAsset(asset), "input colour asset fixture");
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto original = document.snapshot();
    test.expect(execute<SetAssetInputColor>(document, stack, asset.id,
                                            document::AssetColorSpace::Auto, std::string{"ACEScg"})
                    .changed(),
                "asset-level config input choice is an undoable command");
    const auto selected = document.snapshot().project().findAsset(asset.id)->interpretation;
    test.expect(selected.inputColorSpaceId == "ACEScg" &&
                    selected.colorSpace == document::AssetColorSpace::Auto,
                "explicit asset id clears the legacy interpretation");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().findAsset(asset.id)->interpretation ==
                        original.project().findAsset(asset.id)->interpretation,
                "undo restores asset input interpretation");
    test.expect(stack.redo().changed() &&
                    document.snapshot().project().findAsset(asset.id)->interpretation == selected,
                "redo restores exact config input id");
    test.expect(execute<SetAssetInputColor>(document, stack, asset.id,
                                            document::AssetColorSpace::Raw, std::string{})
                        .changed() &&
                    document.snapshot()
                        .project()
                        .findAsset(asset.id)
                        ->interpretation.inputColorSpaceId.empty(),
                "Raw choice replaces config input id rather than leaving a hidden override");
    test.expect(execute<SetAssetInputColor>(document, stack, asset.id,
                                            document::AssetColorSpace::Raw, std::string{"ACEScg"})
                        .status == CommandStatus::Rejected,
                "contradictory legacy and config choices are rejected");
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
// #378: folders hold compositions and other folders, move without cycles or name clashes, give
// their contents back to their parent on removal, and "Delete with contents" is one transaction.
void testFolderStructure(TestContext& test) {
    auto initial = document::makeNewProject("Folders", "Main", core::RationalTime::fromInteger(24));
    const auto main = initial.initialCompositionId;
    const auto a = AssetId::fromRaw(1);
    document::AssetRecord asset;
    asset.id = a;
    asset.locator = {"file", "project-relative", "media/a.png", "file:///media/a.png"};
    asset.width = 4;
    asset.height = 3;
    requireFixture(initial.project.addAsset(asset), "asset fixture");
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto folder = [&](const std::string& name, std::optional<AssetFolderId> parent = {}) {
        return execute<CreateAssetFolder>(document, stack, name, parent)
            .outputId<AssetFolderId>(kCreateAssetFolderOutput)
            .value_or(AssetFolderId{});
    };
    const auto comps = folder("Comps");
    const auto footage = folder("Footage");
    const auto plates = folder("Plates", footage);
    test.expect(comps.isValid() && footage.isValid() && plates.isValid(), "folder fixtures");
    // The live project; each call refreshes the held snapshot.
    auto held = document.snapshot();
    const auto project = [&]() -> const document::Project& {
        held = document.snapshot();
        return held.project();
    };

    using Compositions = std::vector<document::CompositionId>;
    test.expect(execute<MoveCompositions>(document, stack, Compositions{main}, comps).changed() &&
                    project().findComposition(main)->folder() == comps,
                "a composition moves into a folder");
    test.expect(stack.undo().changed() && !project().findComposition(main)->folder(),
                "composition move undo returns it to the root");
    test.expect(stack.redo().changed() && project().findComposition(main)->folder() == comps,
                "composition move redo");
    test.expect(execute<MoveCompositions>(document, stack, Compositions{main}, comps).status ==
                    CommandStatus::NoChange,
                "moving a composition to its own folder is no change");
    const auto beforeRefusals = document.snapshot().revision();
    test.expect(
        execute<MoveCompositions>(document, stack, Compositions{main, main}, std::nullopt).status ==
            CommandStatus::Rejected,
        "a repeated composition is refused");
    test.expect(
        execute<MoveCompositions>(document, stack, Compositions{main}, AssetFolderId::fromRaw(999))
                .status == CommandStatus::Rejected,
        "a missing destination folder is refused");
    test.expect(execute<MoveCompositions>(
                    document, stack, Compositions{document::CompositionId::fromRaw(999)}, comps)
                        .status == CommandStatus::Rejected,
                "a missing composition is refused");

    using Folders = std::vector<AssetFolderId>;
    test.expect(execute<MoveAssetFolders>(document, stack, Folders{footage}, footage).status ==
                    CommandStatus::Rejected,
                "a folder cannot move into itself");
    test.expect(execute<MoveAssetFolders>(document, stack, Folders{footage}, plates).status ==
                    CommandStatus::Rejected,
                "a folder cannot move into its own subfolder");
    const auto clash = folder("Plates");
    test.expect(execute<MoveAssetFolders>(document, stack, Folders{clash}, footage).status ==
                    CommandStatus::Rejected,
                "a destination that already holds the name refuses the move");
    test.expect(stack.undo().changed(), "clash fixture undone");
    test.expect(document.snapshot().revision() != beforeRefusals,
                "fixture bookkeeping moved the revision");
    test.expect(execute<MoveAssetFolders>(document, stack, Folders{plates}, comps).changed() &&
                    project().findAssetFolder(plates)->parent == comps,
                "a folder moves under another folder");
    test.expect(
        execute<MoveAssetFolders>(document, stack, Folders{plates}, std::nullopt).changed() &&
            !project().findAssetFolder(plates)->parent,
        "a folder moves to the root");
    test.expect(stack.undo().changed() && stack.undo().changed() &&
                    project().findAssetFolder(plates)->parent == footage,
                "folder moves undo back to the original parent");
    test.expect(execute<MoveAssetFolders>(document, stack, Folders{plates}, footage).status ==
                    CommandStatus::NoChange,
                "moving a folder to its own parent is no change");

    test.expect(execute<MoveAssets>(document, stack, Assets{a}, plates, 0).changed(),
                "asset fixture moved into the nested folder");
    test.expect(execute<MoveCompositions>(document, stack, Compositions{main}, plates).changed(),
                "composition fixture moved into the nested folder");
    test.expect(execute<RemoveAssetFolder>(document, stack, plates).changed() &&
                    project().findComposition(main)->folder() == footage &&
                    project().findAsset(a)->folder == footage,
                "removing a folder moves its compositions and assets up to the parent");
    test.expect(stack.undo().changed() && project().findComposition(main)->folder() == plates &&
                    project().findAsset(a)->folder == plates,
                "folder removal undo puts the composition back");

    // "Delete with contents": every item, then the folders bottom-up, as one undoable step.
    const auto before = document.snapshot();
    Transaction transaction("Delete Folder", before.revision());
    transaction.emplace<RemoveAsset>(a);
    transaction.emplace<DeleteComposition>(main);
    transaction.emplace<RemoveAssetFolder>(plates);
    transaction.emplace<RemoveAssetFolder>(footage);
    test.expect(stack.execute(std::move(transaction)).changed() && !project().findAsset(a) &&
                    !project().findComposition(main) && !project().findAssetFolder(plates) &&
                    !project().findAssetFolder(footage),
                "delete with contents removes the folder, its subfolders and their items");
    test.expect(stack.undo().changed() && project().findAsset(a) &&
                    project().findComposition(main) &&
                    project().findComposition(main)->folder() == plates &&
                    project().findAssetFolder(plates)->parent == footage,
                "one undo restores the whole deleted folder");
}

// #378: an import prepared for a folder lands there after the folder's existing assets; a folder
// removed while the import was prepared sends the assets to the root instead of losing them.
void testImportIntoFolder(TestContext& test) {
    const auto directory = std::filesystem::current_path() / "assets-folder-import-fixtures";
    std::filesystem::create_directories(directory);
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    const auto lut = [&](const std::string& name) {
        const auto path = directory / name;
        std::ofstream out(path);
        out << "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
        return path;
    };
    const auto first = lut("first.cube");
    const auto second = lut("second.cube");
    auto initial =
        document::makeNewProject("Folder import", "Main", core::RationalTime::fromInteger(1));
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto looks = execute<CreateAssetFolder>(document, stack, "Looks")
                           .outputId<AssetFolderId>(kCreateAssetFolderOutput)
                           .value_or(AssetFolderId{});
    const auto import = [&](const std::filesystem::path& path,
                            std::optional<AssetFolderId> folder) {
        auto prepared =
            std::make_unique<ImportAssets>(std::vector<std::filesystem::path>{path}, directory);
        prepared->setFolder(folder);
        Transaction transaction("Import", document.snapshot().revision());
        requireFixture(transaction.add(std::move(prepared)), "prepared import");
        return stack.execute(std::move(transaction));
    };
    test.expect(import(first, looks).changed(), "an import into a folder publishes");
    test.expect(import(second, looks).changed(), "a second import into the folder publishes");
    const auto snapshot = document.snapshot();
    const auto find = [&](const std::string& path) {
        return std::ranges::find_if(snapshot.project().assets(),
                                    [&](const auto& asset) { return asset.locator.path == path; });
    };
    const auto one = find("first.cube");
    const auto two = find("second.cube");
    test.expect(one != snapshot.project().assets().end() &&
                    two != snapshot.project().assets().end() && one->folder == looks &&
                    two->folder == looks && one->order == 0 && two->order == 1,
                "imported assets land in the folder after its existing assets");
    test.expect(stack.undo().changed() && stack.undo().changed() &&
                    document.snapshot().project().assets().empty(),
                "each folder import undoes as one step");
    test.expect(execute<RemoveAssetFolder>(document, stack, looks).changed(), "folder removed");
    test.expect(import(first, looks).changed() &&
                    !document.snapshot().project().assets().front().folder,
                "a vanished folder sends the import to the root");
}
// #378 New Composition from Selection: sized and timed from the footage, filed in a folder, one
// layer per footage item with the first on top, and one undo removes it all.
void testCompositionFromAssets(TestContext& test) {
    auto initial =
        document::makeNewProject("From footage", "Main", core::RationalTime::fromInteger(10));
    document::AssetRecord clip;
    clip.id = AssetId::fromRaw(1);
    clip.name = "Clip";
    clip.kind = document::AssetKind::Video;
    clip.locator = {"file", "project-relative", "clip.mov", "file:///media/clip.mov"};
    clip.width = 1280;
    clip.height = 720;
    clip.frames = 48;
    clip.duration = core::RationalTime::fromInteger(2);
    document::AssetVideoStream stream;
    stream.codec = "h264";
    stream.timebase = *core::RationalTime::create(1, 25);
    stream.framePeriod = *core::RationalTime::create(1, 25);
    stream.width = 1280;
    stream.height = 720;
    stream.duration = clip.duration;
    clip.videoStreams.push_back(stream);
    requireFixture(initial.project.addAsset(clip), "video fixture");
    document::AssetRecord still;
    still.id = AssetId::fromRaw(2);
    still.name = "Still";
    still.locator = {"file", "project-relative", "still.png", "file:///media/still.png"};
    still.width = 640;
    still.height = 480;
    requireFixture(initial.project.addAsset(still), "image fixture");
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto base = compositionSettingsOf(
        *document.snapshot().project().findComposition(initial.initialCompositionId));
    const auto settings = compositionSettingsForAsset(clip, base);
    test.expect(settings.name == "Clip" && settings.format.width() == 1280 &&
                    settings.format.height() == 720 &&
                    settings.format.frameRate().numerator() == 25 &&
                    settings.format.frameRate().denominator() == 1 &&
                    settings.duration == core::RationalTime::fromInteger(2),
                "a video composition takes the clip's size, rate and duration");
    const auto stillSettings = compositionSettingsForAsset(still, base);
    test.expect(stillSettings.format.width() == 640 && stillSettings.duration == base.duration &&
                    stillSettings.format.frameRate() == base.format.frameRate(),
                "a still keeps the base rate and duration");
    const auto folder = execute<CreateAssetFolder>(document, stack, "Comps")
                            .outputId<AssetFolderId>(kCreateAssetFolderOutput)
                            .value_or(AssetFolderId{});
    const auto before = document.snapshot().project().compositions().size();
    const auto result = execute<AddCompositionFromAssets>(document, stack, settings,
                                                          Assets{clip.id, still.id}, folder);
    const auto composition = result.outputId<document::CompositionId>(kAddCompositionOutput);
    const auto snapshot = document.snapshot();
    const auto* created = composition ? snapshot.project().findComposition(*composition) : nullptr;
    test.expect(result.changed() && created && created->folder() == folder &&
                    created->name() == "Clip" && created->format().width() == 1280,
                "the composition is created in the folder with the derived settings");
    test.expect(created && created->graph().layerOutputs().size() == 2,
                "one layer per footage item");
    test.expect(stack.undo().changed() &&
                    document.snapshot().project().compositions().size() == before,
                "one undo removes the composition and its layers");
    test.expect(execute<AddCompositionFromAssets>(document, stack, settings, Assets{clip.id},
                                                  AssetFolderId::fromRaw(999))
                        .status == CommandStatus::Rejected,
                "a missing folder refuses the new composition");
}
} // namespace
} // namespace bloom::commands::test
int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testLutImport(test);
        bloom::commands::test::testOrganization(test);
        bloom::commands::test::testFolderStructure(test);
        bloom::commands::test::testImportIntoFolder(test);
        bloom::commands::test::testCompositionFromAssets(test);
        bloom::commands::test::testWideImageAssetCommand(test);
        bloom::commands::test::testAssetInputColor(test);
        bloom::commands::test::testRelinkAndSaturatedOrder(test);
    } catch (const std::exception& error) {
        test.fail(error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
