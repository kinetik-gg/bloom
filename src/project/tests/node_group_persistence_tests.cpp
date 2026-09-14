// Durable node groups (document 1.2): the round trip through a real archive, the reopen that must
// return the exact same records, and the 1.1 -> 1.2 migration that gives an older file no groups at
// all. The 1.1 fixture is produced by downgrading a freshly written 1.2 document rather than by
// pasting a second hand-written document literal, so the two versions cannot drift apart here.

#include "zip_container_test_support.hpp"

#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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

document::ColorSettings neutralColorSettings() {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes(bytes));
}

// A project whose one composition carries two groups: one over both live nodes with a deliberately
// non-default padding, and one with no members at all -- which no command produces, but which the
// model permits and the format therefore has to carry without inventing or dropping anything.
struct Authored final {
    std::unique_ptr<document::Document> document;
    document::CompositionId compositionId;
};

Authored authoredProject() {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    auto owned = std::make_unique<document::Document>(std::move(initial.project));
    const auto snapshot = owned->snapshot();
    auto draft = owned->draft(snapshot);
    auto& composition = *draft.project().findComposition(initial.initialCompositionId);
    const auto first = draft.ids().allocateNodeGroup();
    const auto second = draft.ids().allocateNodeGroup();
    if (!first || !second)
        throw std::runtime_error("group ids");
    std::set<document::NodeId> members;
    for (const auto& node : composition.graph().nodes())
        members.insert(node.id);
    composition.nodeGroups()[*first] = {*first, "Lighting", members, {48.0, 12.5}};
    composition.nodeGroups()[*second] = {*second, "Spare", {}, {}};
    if (!owned->commit(snapshot.revision(), std::move(draft)).committed())
        throw std::runtime_error("group commit");
    return {std::move(owned), initial.initialCompositionId};
}

std::vector<std::byte> archiveOf(const document::Snapshot& snapshot,
                                 const document::ColorSettings& settings) {
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    if (!saved || !saved.archive())
        throw std::runtime_error("group archive");
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

void roundTripAndReopen() {
    const auto authored = authoredProject();
    const auto settings = neutralColorSettings();
    auto snapshot = authored.document->snapshot();
    const auto& original = snapshot.project().findComposition(authored.compositionId)->nodeGroups();
    expect(original.size() == 2, "the fixture authored two groups");

    const auto archive = archiveOf(snapshot, settings);
    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened, "a grouped project opens");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    expect(opened.schemaMinor == 6 && !opened.roundTrip,
           "a grouped project is written and read as the current schema minor");
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(authored.compositionId);
    expect(composition != nullptr, "the reopened project keeps its composition");
    if (composition == nullptr)
        return;
    expect(composition->nodeGroups() == original,
           "every group id, name, member and padding component round-trips exactly");
    expect(reopened.ids().highWater().nodeGroup == snapshot.ids().highWater().nodeGroup &&
               reopened.ids().highWater().nodeGroup == 2,
           "the group allocator high water is durable");
    expect(reopened.ids().highWater().node == snapshot.ids().highWater().node,
           "groups never advance the node watermark");

    // Byte determinism: the second write of the reopened document must be the first write again.
    expect(archiveOf(reopened, opened.colorSettings) == archive,
           "a reopened grouped document re-saves byte-identically");
}

// Strips one pretty-printed member line and the comma that joined it to the previous line, which is
// how this fixture turns a canonical 1.2 document back into the 1.1 document it would have been.
void eraseMemberLine(std::string& text, const std::string_view member) {
    const auto key = "\"" + std::string(member) + "\":";
    const auto found = text.find(key);
    if (found == std::string::npos)
        throw std::logic_error("downgrade anchor");
    const auto lineStart = text.rfind('\n', found);
    const auto lineEnd = text.find('\n', found);
    if (lineStart == std::string::npos || lineEnd == std::string::npos)
        throw std::logic_error("downgrade line");
    const auto comma = text.rfind(',', lineStart);
    if (comma == std::string::npos || comma + 1 != lineStart)
        throw std::logic_error("downgrade comma");
    text.erase(comma, lineEnd - comma);
}

// The 1.1 fixture starts from an UNGROUPED project, because a 1.1 file cannot hold a group: both
// members this strips are then the canonical writer's own one-line `[]`/`"0"` spellings.
std::vector<std::byte> legacyArchive(std::string& documentText) {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    const document::Document ungrouped(std::move(initial.project));
    const auto settings = neutralColorSettings();
    const auto snapshot = ungrouped.snapshot();
    const auto current = archiveOf(snapshot, settings);
    auto entries = readZipContainer(current, {}, memory());
    if (!entries)
        throw std::logic_error("legacy entries");
    const auto bytes = entries.document()->documentBytes();
    documentText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto minor = documentText.find("\"minor\": 6");
    if (minor == std::string::npos)
        throw std::logic_error("legacy minor anchor");
    documentText.replace(minor, std::string_view("\"minor\": 6").size(), "\"minor\": 1");
    eraseMemberLine(documentText, "nodeGroups");
    eraseMemberLine(documentText, "nodeGroup");

    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 1}};
    const auto size = canonicalManifestSize(manifest);
    if (!size)
        throw std::logic_error("legacy manifest size");
    std::vector<char> encoded(*size.value());
    if (!encodeCanonicalManifest(manifest, encoded))
        throw std::logic_error("legacy manifest encode");
    return test::buildConformingArchive(
        test::makeStoredEntry("manifest.json", test::toBytes({encoded.data(), encoded.size()})),
        test::makeStoredEntry("document.json", test::toBytes(documentText)));
}

void migrationFromSchema11() {
    std::string documentText;
    const auto legacy = legacyArchive(documentText);
    expect(documentText.find("nodeGroup") == std::string::npos,
           "the 1.1 fixture mentions no group at all");
    auto openedResult = openProjectArchive(legacy, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened, "a 1.1 project opens");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    expect(opened.schemaMinor == 6 && !opened.roundTrip,
           "migration lands on the current editable schema");
    const auto snapshot = opened.document->snapshot();
    const auto& composition = snapshot.project().compositions().front();
    expect(composition.nodeGroups().empty(), "a 1.1 file gets no groups");
    expect(snapshot.ids().highWater().nodeGroup == 0, "a 1.1 file has never issued a group id");

    // A group authored after migration allocates from 1, and the saved file is a 1.2 file.
    auto draft = opened.document->draft(snapshot);
    const auto id = draft.ids().allocateNodeGroup();
    expect(id && id->value() == 1, "the first group after migration is id 1");
    if (!id)
        return;
    draft.project().findComposition(composition.id())->nodeGroups()[*id] = {*id, "Group", {}, {}};
    expect(opened.document->commit(snapshot.revision(), std::move(draft)).committed(),
           "a group authored after migration publishes");
    const auto grouped = opened.document->snapshot();
    auto reopenedResult =
        openProjectArchive(archiveOf(grouped, opened.colorSettings), {}, memory());
    expect(reopenedResult.outcome() == OpenArchiveOutcome::Opened, "the regrouped project reopens");
    if (reopenedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(reopenedResult).takeOpened();
    expect(reopened.document->snapshot().project().compositions().front().nodeGroups() ==
               grouped.project().compositions().front().nodeGroups(),
           "the group authored on a migrated document is durable");
}

void migrationDeterminismAndChain() {
    std::string documentText;
    (void)legacyArchive(documentText);
    auto operation = memory();
    const auto bytes = test::toBytes(documentText);
    auto parsed = parseStrictJsonDom(bytes, {}, operation);
    expect(static_cast<bool>(parsed), "the 1.1 fixture parses");
    if (!parsed)
        return;
    std::pmr::vector<char> first;
    std::pmr::vector<char> second;
    expect(static_cast<bool>(migrateNodeGroupsV1_1(parsed.document()->root(),
                                                   first.get_allocator().resource(), first)),
           "first group migration succeeds");
    expect(static_cast<bool>(migrateNodeGroupsV1_1(parsed.document()->root(),
                                                   second.get_allocator().resource(), second)),
           "second group migration succeeds");
    expect(first == second, "group migration byte determinism");
    // The step refuses a document that is not its own source version, so the chain cannot be
    // entered twice or out of order.
    std::pmr::vector<char> again;
    expect(
        !migrateNodeLayoutV1_0(parsed.document()->root(), again.get_allocator().resource(), again),
        "the layout step refuses a 1.1 document");
    std::pmr::vector<char> output;
    auto migrated = parseStrictJsonDom(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(first.data()), first.size()},
        {}, operation);
    expect(static_cast<bool>(migrated), "migrated group bytes reparse");
    if (!migrated)
        return;
    expect(!migrateNodeGroupsV1_1(migrated.document()->root(), output.get_allocator().resource(),
                                  output),
           "the group step refuses its own output");
}
void multipleMergeRoundTrip() {
    auto authored = authoredProject();
    auto snapshot = authored.document->snapshot();
    auto draft = authored.document->draft(snapshot);
    auto& graph = draft.project().findComposition(authored.compositionId)->graph();
    const auto nested = *draft.ids().allocateNode();
    const auto slot = *draft.ids().allocateLayerSlot();
    const auto edge = *draft.ids().allocateEdge();
    expect(graph.addNode({nested, std::string(document::kLayerStackNodeType), {}, 1}),
           "add nested Merge");
    expect(graph.layerStack().append({slot, {}}) &&
               graph.addEdge(
                   {edge,
                    {nested, "image"},
                    document::LayerStackInputRef{graph.layerStack().nodeId(), slot, "content"}}),
           "connect nested Merge");
    expect(authored.document->commit(snapshot.revision(), std::move(draft)).committed(),
           "publish nested Merge");
    snapshot = authored.document->snapshot();
    const auto bytes = archiveOf(snapshot, neutralColorSettings());
    auto result = openProjectArchive(bytes, {}, memory());
    expect(result.outcome() == OpenArchiveOutcome::Opened, "multiple Merges reopen");
    if (result.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto restored = opened.document->snapshot();
    const auto& restoredGraph = restored.project().findComposition(authored.compositionId)->graph();
    expect(restoredGraph.merges().size() == 2 && restoredGraph.merge(nested) &&
               restoredGraph.layerStack().find(slot),
           "Merge ownership and plain-image slot survive");
    expect(archiveOf(restored, opened.colorSettings) == bytes,
           "multiple Merge archive is byte stable");
}
} // namespace

int main() {
    try {
        multipleMergeRoundTrip();
        roundTripAndReopen();
        migrationFromSchema11();
        migrationDeterminismAndChain();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
