#include "node_layout_legacy_fixture.hpp"
#include "zip_container_test_support.hpp"

#include <bloom/document/document.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace bloom;
using namespace bloom::project;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
ProjectIoOperationMemory memory(const std::uint64_t budget = 64U << 20U) {
    auto coordinator = ProjectIoMemoryCoordinator::create(budget);
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation(budget, budget);
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
std::vector<std::byte> legacyArchive() {
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}};
    const auto size = canonicalManifestSize(manifest);
    if (!size)
        throw std::runtime_error("manifest size");
    std::vector<char> encoded(*size.value());
    if (!encodeCanonicalManifest(manifest, encoded))
        throw std::runtime_error("manifest encode");
    return test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes({encoded.data(), encoded.size()})),
        test::makeStoredEntry("document.json", test::toBytes(kNodeLayoutLegacyDocument)));
}
void migrationAndReopen() {
    const auto legacy = legacyArchive();
    auto result = openProjectArchive(legacy, {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "production open migrates 1.0 layout");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    auto snapshot = opened.document->snapshot();
    const auto& composition = snapshot.project().compositions().front();
    expect(composition.nodeLayout() == document::defaultNodeLayout(composition.graph().nodes()),
           "legacy migration assigns all default records");
    expect(opened.schemaMinor == 5 && !opened.roundTrip,
           "migration produces current editable schema");
    auto draft = opened.document->draft(snapshot);
    const auto frameTime = core::RationalTime::create(1, 24);
    if (!frameTime)
        throw std::logic_error("frame time fixture");
    auto* editing = draft.project().findComposition(composition.id());
    for (const auto& boundary : composition.graph().layerOutputs()) {
        auto* layer = editing->graph().findLayer(boundary.layerId);
        layer->labelColor = std::array<std::uint8_t, 3>{12, 34, 56};
        layer->enabled = false;
        layer->solo = true;
        layer->locked = true;
        layer->inPoint = *frameTime;
        layer->outPoint = core::RationalTime::fromInteger(1);
    }
    editing->setWorkArea(document::WorkArea{*frameTime, core::RationalTime::fromInteger(1)});
    auto& layout = editing->nodeLayout();
    const auto first = composition.graph().nodes().front().id;
    layout[first] = {{-123.5, 89.25}, 276.5, true, true};
    layout[document::NodeId::fromRaw(9999)] = {{45, 67}, 120, false, false};
    expect(draft.validate().ok() && !draft.validate().issues().empty(),
           "unknown layout warns without rejecting");
    expect(opened.document->commit(snapshot.revision(), std::move(draft)).committed(),
           "layout edit publishes");
    snapshot = opened.document->snapshot();
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &opened.colorSettings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    expect(static_cast<bool>(saved), "layout archive passes close/reopen verification");
    if (!saved || !saved.archive())
        return;
    auto reopenedResult = openProjectArchive(saved.archive()->bytes(), {}, memory());
    expect(reopenedResult.outcome() == OpenArchiveOutcome::Opened, "layout archive opens");
    if (reopenedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(reopenedResult).takeOpened();
    const auto reopenedSnapshot = reopened.document->snapshot();
    expect(reopenedSnapshot.project().compositions().front().nodeLayout() ==
               snapshot.project().compositions().front().nodeLayout(),
           "all layout fields and unknown-node records round-trip exactly");
    expect(reopenedSnapshot.project().compositions().front().workArea() ==
               snapshot.project().compositions().front().workArea(),
           "work area survives verified reopen");
    expect(
        std::ranges::equal(reopenedSnapshot.project().compositions().front().graph().layerOutputs(),
                           snapshot.project().compositions().front().graph().layerOutputs()),
        "layer ranges survive verified archive reopen");
    expect(reopenedSnapshot.ids().highWater() == snapshot.ids().highWater(),
           "layout references never allocate semantic IDs");
}
void futureLayoutAttachments() {
    auto baseline = openProjectArchive(legacyArchive(), {}, memory());
    if (baseline.outcome() != OpenArchiveOutcome::Opened)
        throw std::logic_error("layout future baseline");
    auto current = std::move(baseline).takeOpened();
    auto snapshot = current.document->snapshot();
    auto saved = buildVerifiedSaveArchive(
        CanonicalManifestV1{},
        CanonicalDocumentV1{.snapshot = &snapshot, .colorSettings = &current.colorSettings}, {},
        memory());
    if (!saved)
        throw std::logic_error("layout future archive");
    auto entries = readZipContainer(saved.archive()->bytes(), {}, memory());
    if (!entries)
        throw std::logic_error("layout future entries");
    const auto bytes = entries.document()->documentBytes();
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto rootMinor = text.find("\"minor\": 5");
    const auto layoutStart = text.find("\"nodeLayout\"");
    if (rootMinor == std::string::npos || layoutStart == std::string::npos)
        throw std::logic_error("layout future anchors");
    text.replace(rootMinor, std::string_view("\"minor\": 5").size(), "\"minor\": 6");
    const auto y = text.find("\"y\": 32.0", layoutStart);
    if (y == std::string::npos)
        throw std::logic_error("layout position anchor");
    text.insert(y + std::string_view("\"y\": 32.0").size(), R"(, "zzzPosition":42)");
    const auto muted = text.find("\"muted\": false", layoutStart);
    if (muted == std::string::npos)
        throw std::logic_error("layout record anchor");
    text.insert(muted + std::string_view("\"muted\": false").size(), R"(, "zzzLayout":"retained")");
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 6}};
    const auto size = canonicalManifestSize(manifest);
    if (!size)
        throw std::logic_error("future manifest size");
    std::vector<char> encoded(*size.value());
    if (!encodeCanonicalManifest(manifest, encoded))
        throw std::logic_error("future manifest encode");
    const auto archive = test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes({encoded.data(), encoded.size()})),
        test::makeStoredEntry("document.json", test::toBytes(text)));
    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened,
           "future layout and position members open editable");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    expect(opened.roundTrip && opened.schemaMinor == 6,
           "future layout retains its schema and attachments");
    if (!opened.roundTrip)
        throw std::logic_error("layout round-trip state");
    snapshot = opened.document->snapshot();
    auto rewritten =
        buildVerifiedSaveArchive(manifest,
                                 CanonicalDocumentV1{.snapshot = &snapshot,
                                                     .colorSettings = &opened.colorSettings,
                                                     .roundTrip = &*opened.roundTrip,
                                                     .schemaMinor = 6},
                                 {}, memory());
    expect(static_cast<bool>(rewritten), "future layout passes verified overlay save");
    if (!rewritten)
        return;
    auto rewrittenEntries = readZipContainer(rewritten.archive()->bytes(), {}, memory());
    if (!rewrittenEntries)
        throw std::logic_error("rewritten layout entries");
    const auto rewrittenBytes = rewrittenEntries.document()->documentBytes();
    const std::string_view retained(reinterpret_cast<const char*>(rewrittenBytes.data()),
                                    rewrittenBytes.size());
    expect(retained.find("\"zzzPosition\": 42") != std::string_view::npos &&
               retained.find("\"zzzLayout\": \"retained\"") != std::string_view::npos,
           "unknown position and NodeId-keyed layout members survive reopen and save");
}

void migrationDeterminismAndBudget() {
    const auto bytes = test::toBytes(kNodeLayoutLegacyDocument);
    auto operation = memory();
    auto parsed = parseStrictJsonDom(bytes, {}, operation);
    expect(static_cast<bool>(parsed), "legacy fixture parses");
    if (!parsed)
        return;
    std::pmr::vector<char> first;
    std::pmr::vector<char> second;
    expect(static_cast<bool>(migrateNodeLayoutV1_0(parsed.document()->root(),
                                                   first.get_allocator().resource(), first)),
           "first migration succeeds");
    expect(static_cast<bool>(migrateNodeLayoutV1_0(parsed.document()->root(),
                                                   second.get_allocator().resource(), second)),
           "second migration succeeds");
    expect(first == second, "migration byte determinism");
    const auto limited = migrateDocumentDom(parsed.document()->root(), {1, 0}, {1, 1},
                                            kProductionDocumentMigrationSteps, {}, memory(1));
    expect(!limited && limited.error() == MigrationError::ResourceExhausted,
           "migration output is budget charged");
}
} // namespace
int main() {
    try {
        migrationAndReopen();
        migrationDeterminismAndBudget();
        futureLayoutAttachments();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
