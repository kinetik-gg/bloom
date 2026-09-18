#include "zip_container_test_support.hpp"
#include <array>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace {
using namespace bloom;
using namespace bloom::project;
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::cerr << message << '\n';
    }
}
ProjectIoOperationMemory memory() {
    auto coordinator = ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
document::AssetRecord plate() {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(7);
    asset.locator = {"file", "project-relative", "media/shot.v02.exr",
                     "file:///media/shot.v02.exr"};
    asset.width = 2;
    asset.height = 3;
    asset.name = "Plate";
    return asset;
}
void roundTripAndValidation() {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    const auto root = document::AssetFolderId::fromRaw(3);
    const auto child = document::AssetFolderId::fromRaw(8);
    expect(initial.project.addAssetFolder({child, "Plates", root}),
           "child record accepts later parent declaration");
    expect(initial.project.addAssetFolder({root, "Shots", {}}), "root folder added");
    auto asset = plate();
    asset.folder = child;
    asset.tags = {"approved", "hero"};
    asset.order = 42;
    expect(initial.project.addAsset(asset), "organized asset added");
    document::Document document(std::move(initial.project));
    auto snapshot = document.snapshot();
    expect(snapshot.ids().highWater().assetFolder == 8,
           "folder namespace participates in allocator inventory");
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings},
                                            {}, memory());
    expect(static_cast<bool>(archive), "organized project saves at 1.20");
    if (!archive)
        return;
    auto result = openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "organized project opens");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    auto restored = opened.document->snapshot();
    expect(opened.schemaMinor == 20 && *restored.project().findAsset(asset.id) == asset,
           "name, tags, folder, order and media identity round-trip at 1.20");
    expect(restored.project().assetFolders().size() == 2 &&
               *restored.project().findAssetFolder(child) ==
                   *snapshot.project().findAssetFolder(child),
           "folder hierarchy round-trips");
    auto draft = document.draft(snapshot);
    draft.project().findAssetFolder(root)->parent = child;
    expect(!draft.validate().ok(), "folder cycle rejected");
    expect(!document.commit(snapshot.revision(), std::move(draft)).committed(),
           "cycle cannot publish");
    draft = document.draft(snapshot);
    draft.project().findAsset(asset.id)->folder = document::AssetFolderId::fromRaw(999);
    expect(!draft.validate().ok(), "dangling asset folder rejected");
    draft = document.draft(snapshot);
    draft.project().findAssetFolder(root)->parent = document::AssetFolderId::fromRaw(999);
    expect(!draft.validate().ok(), "dangling parent rejected");
    draft = document.draft(snapshot);
    expect(draft.project().addAssetFolder({document::AssetFolderId::fromRaw(9), "Plates", root}),
           "duplicate name fixture");
    expect(!draft.validate().ok(), "duplicate sibling folder names rejected");
    draft = document.draft(snapshot);
    draft.project().findAsset(asset.id)->tags = {"hero", "approved"};
    expect(!draft.validate().ok(), "unsorted tags rejected");
    draft.project().findAsset(asset.id)->tags = {"hero", "hero"};
    expect(!draft.validate().ok(), "duplicate tags rejected");
}
void lexicalNames() {
    auto asset = plate();
    for (const auto& [path, expected] :
         std::vector<std::pair<std::string, std::string>>{{"media/shot.v02.exr", "shot.v02"},
                                                          {"media/.hidden", ".hidden"},
                                                          {"C:\\media\\frame.png", "frame"},
                                                          {"media/..", ".."},
                                                          {"media/élan.png", "élan"}}) {
        asset.locator.path = path;
        expect(document::defaultAssetName(asset) == expected, "portable lexical file-stem name");
    }
    asset.locator.path.clear();
    asset.fontFamily = "Embedded face";
    expect(document::defaultAssetName(asset) == "Embedded face",
           "font family fallback for empty locator");
    asset.kind = document::AssetKind::Font;
    asset.locator = {"font", "builtin", "embedded/0", "font:fixture"};
    asset.fontStyle = "Book";
    expect(document::defaultAssetName(asset) == "Embedded face Book",
           "builtin font has no file stem and retains its captured face label");
}
void migration() {
    auto initial = document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
    expect(initial.project.addAsset(plate()), "migration plate added");
    document::AssetRecord font;
    font.id = document::AssetId::fromRaw(8);
    font.kind = document::AssetKind::Font;
    font.locator = {"font", "builtin", "embedded/0", "font:fixture"};
    font.fontFamily = "Captured face";
    font.fontStyle = "Book";
    expect(initial.project.addAsset(font), "migration builtin font added");
    document::Document document(std::move(initial.project));
    const auto snapshot = document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<std::size_t, 64> sorting{};
    const CanonicalDocumentV1 input{&snapshot, &settings, {}, sorting};
    const auto size = canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string current(*size.value(), '\0');
    if (!encodeCanonicalDocument(input, current))
        throw std::runtime_error("canonical document");
    auto legacy = std::regex_replace(
        current,
        std::regex(
            R"regex(,\s*"name": "(?:Plate|Captured face Book)",\s*"tags": \[\],\s*"order": "0")regex"),
        "");
    legacy = std::regex_replace(legacy, std::regex(R"(,\s*"inputColorSpaceId": "")"), "");
    legacy = std::regex_replace(legacy, std::regex(R"(,\s*"dataBlocks"\s*:\s*\[\])"), "");
    legacy = std::regex_replace(legacy, std::regex(R"(,\s*"dataBlock"\s*:\s*"0")"), "");
    const auto version = legacy.find("\"minor\": 20");
    legacy.replace(version, std::string_view("\"minor\": 20").size(), "\"minor\": 15");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(legacy)), {}, operation);
    if (!parsed)
        throw std::runtime_error("legacy parse");
    const auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    expect(decoded && decoded.value()->assets.front().name == "shot.v02",
           "1.15 decode derives locator file stem");
    expect(decoded && decoded.value()->assets.back().name == "Captured face Book",
           "builtin font migration uses its captured face instead of a numeric locator");
    auto migrated = migrateDocumentDom(parsed.document()->root(), {1, 15}, {1, 16},
                                       kProductionDocumentMigrationSteps, {}, operation);
    expect(migrated.outcome() == MigrationOutcome::Migrated && migrated.stepsApplied() == 1,
           "production 1.15 to 1.16 migration runs one additive step");
    if (migrated.outcome() != MigrationOutcome::Migrated)
        return;
    const auto upgraded = decodeDocumentEnvelope(*migrated.migratedRoot());
    expect(upgraded && decoded && *upgraded.value() == *decoded.value(),
           "DOM and typed migration agree without changing media identity");
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 15}, .requirements = {}};
    const auto manifestSize = canonicalManifestSize(manifest);
    if (!manifestSize)
        throw std::runtime_error("manifest size");
    std::string manifestText(*manifestSize.value(), '\0');
    if (!encodeCanonicalManifest(manifest, manifestText))
        throw std::runtime_error("manifest encode");
    const auto archive = test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes(manifestText)),
        test::makeStoredEntry("document.json", test::toBytes(legacy)));
    auto opened = openProjectArchive(archive, {}, memory());
    expect(opened.outcome() == OpenArchiveOutcome::Opened,
           "production open migrates a 1.15 archive");
    if (opened.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto result = std::move(opened).takeOpened();
    expect(result.schemaMinor == 20 &&
               result.document->snapshot().project().assets().front().name == "shot.v02",
           "opened archive reports current schema and migrated display name");
}

void inputInterpretationMigration() {
    const std::array expectedIds{"", "srgb_rec709_display", "lin_rec709_scene", ""};
    for (std::uint32_t value = 0; value <= 3; ++value) {
        auto initial =
            document::makeNewProject("Assets", "Main", core::RationalTime::fromInteger(24));
        expect(initial.project.addAsset(plate()), "input migration fixture asset added");
        document::Document document{std::move(initial.project)};
        const auto snapshot = document.snapshot();
        const auto settings = document::makeBloomNeutralColorSettingsV1({});
        std::array<char, 64> payloadScratch{};
        std::array<std::size_t, 64> sortScratch{};
        const CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
        const auto size = canonicalDocumentSize(input);
        if (!size)
            throw std::runtime_error("input migration canonical size");
        std::string text(*size.value(), '\0');
        if (!encodeCanonicalDocument(input, text))
            throw std::runtime_error("input migration canonical encode");
        const auto rootVersion = text.find("\"minor\": 20\n  },\n  \"project\"");
        if (rootVersion == std::string::npos)
            throw std::runtime_error("input migration root version anchor");
        text.replace(rootVersion, std::string_view("\"minor\": 20").size(), "\"minor\": 19");
        const auto interpretation = text.find("\"interpretation\": {");
        if (interpretation == std::string::npos)
            throw std::runtime_error("input migration interpretation anchor");
        const auto interpretationEnd = text.find("\n        }", interpretation);
        if (interpretationEnd == std::string::npos)
            throw std::runtime_error("input migration interpretation end");
        const auto replacement = std::string("\"interpretation\": {\n") +
                                 "          \"colorSpace\": " + std::to_string(value) + ",\n" +
                                 "          \"alphaAssociation\": 0\n        }";
        text.replace(interpretation,
                     interpretationEnd + std::string_view("\n        }").size() - interpretation,
                     replacement);
        auto operation = memory();
        auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
        expect(static_cast<bool>(parsed), "1.19 input interpretation fixture parses");
        if (!parsed)
            continue;
        auto migrated = migrateDocumentDom(parsed.document()->root(), {1, 19}, {1, 20},
                                           kProductionDocumentMigrationSteps, {}, operation);
        expect(migrated.outcome() == MigrationOutcome::Migrated && migrated.stepsApplied() == 1,
               "1.19 input interpretation migrates in one deterministic step");
        if (migrated.outcome() != MigrationOutcome::Migrated)
            continue;
        const auto decoded = decodeDocumentEnvelope(*migrated.migratedRoot());
        expect(decoded && decoded.value()->assets.size() == 1,
               "migrated input interpretation decodes one asset");
        if (!decoded || decoded.value()->assets.empty())
            continue;
        const auto& interpretationValue = decoded.value()->assets.front().interpretation;
        expect(interpretationValue.colorSpace == static_cast<document::AssetColorSpace>(value) &&
                   interpretationValue.inputColorSpaceId == expectedIds[value],
               "1.19 enum migration pins the exact config input id for every enum value");
    }
}
} // namespace
int main() {
    try {
        roundTripAndValidation();
        migration();
        inputInterpretationMigration();
        lexicalNames();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
