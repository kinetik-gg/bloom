// Issue #357: an edit validates, reserves and copies what it changed, not the whole project.
#include <bloom/document/document.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <iostream>
#include <string>

namespace {
using namespace bloom::document;
using bloom::core::RationalTime;

int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::logic_error(message);
}

// One valid composition: Output <- Merge, plus `fillers` unconnected Solid nodes. Every ID is
// derived from `base`, so compositions never collide.
[[nodiscard]] Composition makeComposition(const std::uint64_t base, const std::size_t fillers) {
    const auto stack = NodeId::fromRaw(base + 1);
    const auto output = NodeId::fromRaw(base + 2);
    CanonicalGraph graph(stack);
    require(
        graph.addNode({stack, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}),
        "stack");
    require(graph.addNode({output,
                           std::string(kCompositionOutputNodeType),
                           {},
                           kCompositionOutputNodeSchemaVersion}),
            "output");
    require(graph.addEdge({EdgeId::fromRaw(base + 1),
                           {stack, std::string(kLayerStackOutputPort)},
                           NodeInputRef{output, std::string(kCompositionOutputInputPort)}}),
            "edge");
    graph.setCompositionOutput({output, std::string(kCompositionOutputOutputPort)});
    for (std::size_t index = 0; index < fillers; ++index)
        require(graph.addNode({NodeId::fromRaw(base + 100 + index),
                               std::string(kRerouteNodeType),
                               {},
                               kValueNodeSchemaVersion}),
                "filler");
    return {CompositionId::fromRaw(base), "Comp " + std::to_string(base),
            RationalTime::fromInteger(10), std::move(graph)};
}

[[nodiscard]] Project makeProject(const std::size_t compositions, const std::size_t fillers) {
    Project project(ProjectId::fromRaw(1), "Incremental");
    for (std::size_t index = 0; index < compositions; ++index)
        require(project.addComposition(makeComposition(1000 * (index + 1), fillers)),
                "composition");
    require(project.addAssetFolder({AssetFolderId::fromRaw(7), "Plates", std::nullopt}), "folder");
    return project;
}

struct Delta {
    Project::IncrementalValidationWork before = Project::incrementalValidationWork();
    std::uint64_t fullBefore = Project::validationCount();
    std::uint64_t locksBefore = Composition::lockIndexBuilds();
    [[nodiscard]] std::uint64_t validated() const {
        return Project::incrementalValidationWork().compositionsValidated -
               before.compositionsValidated;
    }
    [[nodiscard]] std::uint64_t skipped() const {
        return Project::incrementalValidationWork().compositionsSkipped -
               before.compositionsSkipped;
    }
    [[nodiscard]] std::uint64_t full() const { return Project::validationCount() - fullBefore; }
    [[nodiscard]] std::uint64_t locks() const {
        return Composition::lockIndexBuilds() - locksBefore;
    }
};

// Adds one Solid to the first composition and commits.
[[nodiscard]] Delta editFirstComposition(Document& document) {
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    auto& composition = draft.project().compositions().front();
    auto* editable = draft.project().findComposition(composition.id());
    const auto node = draft.ids().allocateNode();
    require(node.has_value(), "allocate");
    Delta delta;
    require(editable->graph().addNode(
                {*node, std::string(kRerouteNodeType), {}, kValueNodeSchemaVersion}),
            "add");
    const auto result = document.commit(snapshot.revision(), std::move(draft));
    require(result.committed(), "commit");
    return delta;
}

void testCostIndependentOfUnrelatedCompositions() {
    for (const auto [compositions, fillers] :
         {std::pair<std::size_t, std::size_t>{2, 4}, {16, 64}}) {
        Document document(makeProject(compositions, fillers));
        const auto before = document.snapshot();
        const auto delta = editFirstComposition(document);
        expect(delta.validated() == 1, "an edit validates exactly the composition it changed");
        expect(delta.skipped() == compositions - 1,
               "every unrelated composition is skipped, however many there are");
        expect(delta.full() == 0, "a valid edit never runs whole-project validation");
        expect(delta.locks() == 1, "only the changed composition's lock index is rebuilt");
        const auto after = document.snapshot();
        expect(after.project().assetFolders().data() == before.project().assetFolders().data(),
               "unedited root records are shared between snapshots, not copied");
        expect(after.project().compositions()[1].contentIdentity() ==
                   before.project().compositions()[1].contentIdentity(),
               "unedited compositions share their content");
        expect(after.ids().highWater() != before.ids().highWater(),
               "the new node's ID is reserved");
    }
}

[[nodiscard]] std::vector<ValidationIssue> issuesOf(const ValidationResult& result) {
    return {result.issues().begin(), result.issues().end()};
}

// Commits a draft produced by `mutate` and checks the refusal carries exactly the diagnostics of a
// whole-project validation of the same draft.
template <typename Mutate> void expectIdenticalDiagnostics(const char* message, Mutate mutate) {
    Document document(makeProject(3, 2));
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    mutate(draft);
    const auto expected = issuesOf(draft.project().validate());
    expect(!expected.empty(), message);
    const auto result = document.commit(snapshot.revision(), std::move(draft));
    expect(!result.committed() && issuesOf(result.validation) == expected, message);
}

void testInvalidDraftsReportFullDiagnostics() {
    expectIdenticalDiagnostics(
        "a node ID declared by another composition is diagnosed whole", [](Draft& draft) {
            auto* composition = draft.project().findComposition(CompositionId::fromRaw(1000));
            require(composition->graph().addNode({NodeId::fromRaw(2001),
                                                  std::string(kRerouteNodeType),
                                                  {},
                                                  kValueNodeSchemaVersion}),
                    "duplicate");
        });
    expectIdenticalDiagnostics(
        "an invalid composition setting is diagnosed whole", [](Draft& draft) {
            draft.project().findComposition(CompositionId::fromRaw(2000))->setName("");
        });
    expectIdenticalDiagnostics("an invalid root record is diagnosed whole", [](Draft& draft) {
        draft.project().findAssetFolder(AssetFolderId::fromRaw(7))->name = "";
    });
    expectIdenticalDiagnostics("a duplicate composition ID is diagnosed whole", [](Draft& draft) {
        auto copy = *draft.project().findComposition(CompositionId::fromRaw(3000));
        copy.setName("Copy");
        auto* target = draft.project().findComposition(CompositionId::fromRaw(2000));
        *target = copy;
    });
}

void testRetainedDraftPointerCannotReachASnapshot() {
    Document document(makeProject(2, 1));
    const auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    auto* folder = draft.project().findAssetFolder(AssetFolderId::fromRaw(7));
    folder->name = "Renamed";
    require(document.commit(snapshot.revision(), std::move(draft)).committed(), "commit");
    folder->name = "Escaped";
    expect(document.snapshot().project().findAssetFolder(AssetFolderId::fromRaw(7))->name ==
                   "Renamed" &&
               snapshot.project().findAssetFolder(AssetFolderId::fromRaw(7))->name == "Plates",
           "a pointer retained from the draft cannot mutate any published snapshot");
    expect(document.snapshot().project().assetFolders().data() !=
               snapshot.project().assetFolders().data(),
           "an edited root collection is published as its own copy");
}

} // namespace

int main() {
    try {
        testCostIndependentOfUnrelatedCompositions();
        testInvalidDraftsReportFullDiagnostics();
        testRetainedDraftPointerCannotReachASnapshot();
    } catch (const std::exception& error) {
        std::cerr << "fixture failure: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " incremental validation expectation(s) failed\n";
        return 1;
    }
    return 0;
}
