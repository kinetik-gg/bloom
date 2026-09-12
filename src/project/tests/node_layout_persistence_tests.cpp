#include "node_layout_legacy_fixture.hpp"
#include "zip_container_test_support.hpp"

#include <bloom/document/document.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>

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
    expect(opened.schemaMinor == 1 && !opened.roundTrip,
           "migration produces current editable schema");
    auto draft = opened.document->draft(snapshot);
    auto& layout = draft.project().findComposition(composition.id())->nodeLayout();
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
    expect(reopenedSnapshot.ids().highWater() == snapshot.ids().highWater(),
           "layout references never allocate semantic IDs");
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
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
