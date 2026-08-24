#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/validation.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using bloom::core::RationalTime;
using bloom::document::AnimationCurveId;
using bloom::document::CanonicalGraph;
using bloom::document::CommitStatus;
using bloom::document::Composition;
using bloom::document::CompositionId;
using bloom::document::ConstantValueSource;
using bloom::document::Document;
using bloom::document::DriverBindingId;
using bloom::document::DriverBindingSource;
using bloom::document::EdgeId;
using bloom::document::EdgeRecord;
using bloom::document::LayerId;
using bloom::document::LayerOutputBoundary;
using bloom::document::LayerSlotId;
using bloom::document::LayerStackEntry;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;
using bloom::document::NodeRecord;
using bloom::document::OutputPortRef;
using bloom::document::ParameterId;
using bloom::document::ParameterRecord;
using bloom::document::Project;
using bloom::document::ProjectId;
using bloom::document::ValidationCode;
using bloom::document::Vec2d;

class ExpectationContext final {
  public:
    bool expect(const bool condition, std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return true;
        }

        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        return false;
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

template <typename Id> [[nodiscard]] constexpr Id id(const std::uint64_t value) noexcept {
    return Id::fromRaw(value);
}

[[nodiscard]] RationalTime time(const std::int64_t numerator, const std::int64_t denominator) {
    const auto value = RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("Invalid time fixture");
    }
    return *value;
}

[[nodiscard]] bool hasIssue(const bloom::document::ValidationResult& result,
                            const ValidationCode code) {
    return std::any_of(result.issues().begin(), result.issues().end(),
                       [code](const auto& issue) { return issue.code == code; });
}

[[nodiscard]] Project validProject() {
    constexpr auto projectId = id<ProjectId>(1);
    constexpr auto compositionId = id<CompositionId>(4);
    constexpr auto sourceA = id<NodeId>(10);
    constexpr auto layerOutputA = id<NodeId>(11);
    constexpr auto sourceB = id<NodeId>(12);
    constexpr auto layerOutputB = id<NodeId>(13);
    constexpr auto stackNode = id<NodeId>(14);
    constexpr auto outputNode = id<NodeId>(15);
    constexpr auto layerA = id<LayerId>(20);
    constexpr auto layerB = id<LayerId>(21);
    constexpr auto slotA = id<LayerSlotId>(30);
    constexpr auto slotB = id<LayerSlotId>(31);
    constexpr auto opacityA = id<ParameterId>(40);
    constexpr auto opacityB = id<ParameterId>(41);

    CanonicalGraph graph(stackNode);
    NodeRecord sourceNodeA{sourceA, "bloom.solid", {}};
    NodeRecord layerOutputNodeA{layerOutputA,
                                std::string(bloom::document::kLayerOutputNodeType),
                                {{std::string(bloom::document::kOpacityParameterRole), opacityA}}};
    NodeRecord sourceNodeB{sourceB, "bloom.solid", {}};
    NodeRecord layerOutputNodeB{layerOutputB,
                                std::string(bloom::document::kLayerOutputNodeType),
                                {{std::string(bloom::document::kOpacityParameterRole), opacityB}}};
    NodeRecord stackRecord{stackNode, std::string(bloom::document::kLayerStackNodeType), {}};
    NodeRecord outputRecord{
        outputNode, std::string(bloom::document::kCompositionOutputNodeType), {}};
    if (!graph.addNode(std::move(sourceNodeA)) || !graph.addNode(std::move(layerOutputNodeA)) ||
        !graph.addNode(std::move(sourceNodeB)) || !graph.addNode(std::move(layerOutputNodeB)) ||
        !graph.addNode(std::move(stackRecord)) || !graph.addNode(std::move(outputRecord))) {
        throw std::logic_error("Could not create graph nodes");
    }

    if (!graph.addLayerOutput({layerOutputA, layerA, "Foreground",
                               std::string(bloom::document::kLayerOutputOutputPort)}) ||
        !graph.addLayerOutput({layerOutputB, layerB, "Background",
                               std::string(bloom::document::kLayerOutputOutputPort)}) ||
        !graph.layerStack().append({slotA, layerA}) ||
        !graph.layerStack().append({slotB, layerB})) {
        throw std::logic_error("Could not create layer records");
    }

    if (!graph.addEdge({id<EdgeId>(50), {sourceA, "image"}, NodeInputRef{layerOutputA, "image"}}) ||
        !graph.addEdge(
            {id<EdgeId>(51),
             {layerOutputA, "image"},
             LayerStackInputRef{stackNode, slotA,
                                std::string(bloom::document::kLayerStackContentInputRole)}}) ||
        !graph.addEdge({id<EdgeId>(52), {sourceB, "image"}, NodeInputRef{layerOutputB, "image"}}) ||
        !graph.addEdge(
            {id<EdgeId>(53),
             {layerOutputB, "image"},
             LayerStackInputRef{stackNode, slotB,
                                std::string(bloom::document::kLayerStackContentInputRole)}}) ||
        !graph.addEdge({id<EdgeId>(54), {stackNode, "image"}, NodeInputRef{outputNode, "image"}})) {
        throw std::logic_error("Could not create graph edges");
    }
    graph.setCompositionOutput({outputNode, "image"});

    Composition composition(compositionId, "Comp", time(10, 1), std::move(graph));
    if (!composition.parameters().insert({opacityA,
                                          std::string(bloom::document::kOpacityParameterSchemaKey),
                                          ConstantValueSource{1.0}}) ||
        !composition.parameters().insert({opacityB,
                                          std::string(bloom::document::kOpacityParameterSchemaKey),
                                          ConstantValueSource{0.75}})) {
        throw std::logic_error("Could not create parameters");
    }

    Project project(projectId, "Project");
    if (!project.addComposition(std::move(composition))) {
        throw std::logic_error("Could not create composition");
    }
    return project;
}

void testRationalTime(ExpectationContext& expectations) {
    const auto half = RationalTime::create(2, 4);
    const auto zero = RationalTime::create(0, 99);
    expectations.expect(half.has_value() && half->numerator() == 1 && half->denominator() == 2,
                        "rational time normalizes by the greatest common divisor");
    expectations.expect(zero.has_value() && zero->numerator() == 0 && zero->denominator() == 1,
                        "zero rational time has a canonical denominator");
    expectations.expect(!RationalTime::create(1, 0).has_value() &&
                            !RationalTime::create(1, -2).has_value(),
                        "nonpositive denominators are rejected");

    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto minimumTime = RationalTime::create(minimum, 1);
    const auto overflowProneLeft = RationalTime::create(maximum, maximum - 1);
    const auto overflowProneRight = RationalTime::create(maximum - 1, maximum - 2);
    expectations.expect(minimumTime.has_value() && minimumTime->numerator() == minimum,
                        "INT64_MIN remains representable");
    expectations.expect(overflowProneLeft.has_value() && overflowProneRight.has_value() &&
                            *overflowProneLeft < *overflowProneRight,
                        "ordering is exact without cross-multiplication overflow");
    expectations.expect(time(-2, 3) < time(-1, 2) && time(-1, 2) < RationalTime{},
                        "negative rational ordering is exact");
}

void testIdsAndParameters(ExpectationContext& expectations) {
    bloom::document::IdAllocator allocator;
    allocator.reserveExisting(id<NodeId>(99));
    const auto firstNode = allocator.allocateNode();
    const auto firstLayer = allocator.allocateLayer();
    expectations.expect(firstNode == id<NodeId>(100) && firstLayer == id<LayerId>(1),
                        "typed project allocator advances independently");
    expectations.expect(firstNode != std::optional<NodeId>{}, "allocated IDs are nonzero");

    bloom::document::ParameterStore parameters;
    constexpr auto parameterId = id<ParameterId>(1);
    expectations.expect(
        parameters.insert({parameterId, std::string(bloom::document::kOpacityParameterSchemaKey),
                           ConstantValueSource{1.0}}),
        "constant parameter inserts");
    expectations.expect(
        !parameters.insert({parameterId, std::string(bloom::document::kOpacityParameterSchemaKey),
                            ConstantValueSource{0.5}}),
        "duplicate parameter ID is rejected");
    expectations.expect(
        !parameters.insert({ParameterId{}, "bloom.invalid", ConstantValueSource{0.0}}),
        "zero parameter ID is rejected");
    expectations.expect(!parameters.setSource(parameterId, DriverBindingSource{DriverBindingId{}}),
                        "invalid driver source is rejected");
    expectations.expect(
        parameters.setSource(parameterId, DriverBindingSource{id<DriverBindingId>(7)}),
        "source changes are explicit and mutually exclusive");
    expectations.expect(
        std::holds_alternative<DriverBindingSource>(parameters.find(parameterId)->source),
        "driver source replaces the prior constant source");

    constexpr auto positionId = id<ParameterId>(2);
    expectations.expect(
        parameters.insert({positionId, std::string(bloom::document::kPositionParameterSchemaKey),
                           ConstantValueSource{Vec2d{960.0, 540.0}}}),
        "a two-dimensional position has one stable parameter identity");
    expectations.expect(
        !parameters.setSource(
            positionId, ConstantValueSource{Vec2d{std::numeric_limits<double>::infinity(), 0.0}}),
        "non-finite vector parameter components are rejected");
}

void testCanonicalGraphAndLayerOrder(ExpectationContext& expectations) {
    auto project = validProject();
    auto* composition = project.findComposition(id<CompositionId>(4));
    if (!expectations.expect(composition != nullptr && project.validate().ok(),
                             "canonical two-layer project validates")) {
        return;
    }

    auto& graph = composition->graph();
    const std::vector<EdgeRecord> edgesBefore(graph.edges().begin(), graph.edges().end());
    expectations.expect(graph.layerStack().moveBefore(id<LayerSlotId>(31), id<LayerSlotId>(30)),
                        "layer stack reorders by stable slot identity");
    expectations.expect(graph.layerStack().entries()[0].slotId == id<LayerSlotId>(31) &&
                            graph.layerStack().entries()[1].slotId == id<LayerSlotId>(30),
                        "only order changes during reorder");
    expectations.expect(std::equal(edgesBefore.begin(), edgesBefore.end(), graph.edges().begin()),
                        "reorder does not rewrite graph edges");
    expectations.expect(project.validate().ok(), "reordered graph remains valid");

    expectations.expect(!graph.layerStack().append({id<LayerSlotId>(31), id<LayerId>(99)}),
                        "duplicate stable slot is rejected");
    expectations.expect(!graph.layerStack().append({id<LayerSlotId>(99), id<LayerId>(20)}),
                        "duplicate layer participation is rejected");
    expectations.expect(!graph.addNode({id<NodeId>(10), "bloom.duplicate", {}}),
                        "duplicate node ID is rejected");
    expectations.expect(
        !graph.addEdge(
            {id<EdgeId>(99), {id<NodeId>(10), "image"}, NodeInputRef{id<NodeId>(11), "image"}}),
        "duplicate input connection is rejected");

    expectations.expect(
        graph.addEdge(
            {id<EdgeId>(99), {id<NodeId>(15), "image"}, NodeInputRef{id<NodeId>(10), "feedback"}}),
        "incomplete draft may add a structurally valid edge");
    expectations.expect(hasIssue(project.validate(), ValidationCode::GraphCycle),
                        "same-time graph cycle is rejected by publication validation");
}

void testDocumentSnapshots(ExpectationContext& expectations) {
    Document document(validProject());
    const auto original = document.snapshot();
    auto draft = document.draft(original);
    draft.project().setName("Changed");
    const auto allocated = draft.ids().allocateNode();
    expectations.expect(allocated == id<NodeId>(16),
                        "document allocator reserves IDs already present in project state");

    const auto committed = document.commit(original.revision(), std::move(draft));
    if (!expectations.expect(committed.committed() && committed.snapshot.has_value(),
                             "valid commit publishes a snapshot")) {
        return;
    }
    expectations.expect(committed.snapshot->revision().value() == 1,
                        "commit publishes one monotonic revision");
    expectations.expect(original.project().name() == "Project" &&
                            committed.snapshot->project().name() == "Changed",
                        "snapshots remain immutable after publication");
    auto committedDraft = document.draft(*committed.snapshot);
    expectations.expect(committedDraft.ids().allocateNode() == id<NodeId>(17),
                        "published document state preserves allocator progress");

    auto staleDraft = document.draft(original);
    staleDraft.project().setName("Stale");
    expectations.expect(document.commit(original.revision(), std::move(staleDraft)).status ==
                            CommitStatus::RevisionConflict,
                        "stale expected revision cannot publish");

    const auto restored = document.restore(committed.snapshot->revision(), original);
    if (!expectations.expect(restored.committed() && restored.snapshot.has_value(),
                             "valid restore publishes a snapshot")) {
        return;
    }
    expectations.expect(restored.snapshot->revision().value() == 2,
                        "restore creates a new monotonic revision");
    expectations.expect(restored.snapshot->project().name() == "Project",
                        "restore preserves historical records exactly");
    const auto* restoredComposition =
        restored.snapshot->project().findComposition(id<CompositionId>(4));
    expectations.expect(restoredComposition != nullptr &&
                            restoredComposition->graph().nodes()[0].id == id<NodeId>(10) &&
                            restoredComposition->graph().layerOutputs()[0].layerId ==
                                id<LayerId>(20) &&
                            restoredComposition->graph().layerOutputs()[0].name == "Foreground",
                        "restore retains exact graph, layer, and human-readable layer identity");
    auto restoredDraft = document.draft(*restored.snapshot);
    expectations.expect(restoredDraft.ids().allocateNode() == id<NodeId>(16),
                        "restore also restores the project-owned ID allocator");

    auto invalidDraft = document.draft(*restored.snapshot);
    auto* composition = invalidDraft.project().findComposition(id<CompositionId>(4));
    composition->graph().findNode(id<NodeId>(10))->typeId.clear();
    const auto invalid = document.commit(restored.snapshot->revision(), std::move(invalidDraft));
    expectations.expect(invalid.status == CommitStatus::InvalidDraft && !invalid.validation.ok(),
                        "invalid draft cannot publish");
    expectations.expect(document.snapshot().revision() == restored.snapshot->revision(),
                        "failed commit leaves revision and state unchanged");
}

void testInvalidConstruction(ExpectationContext& expectations) {
    bool threw = false;
    try {
        Project invalid(ProjectId{}, "Invalid");
        [[maybe_unused]] Document document(std::move(invalid));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expectations.expect(threw, "document rejects a zero project ID");
}

void testNewProjectFactory(ExpectationContext& expectations) {
    auto newProject =
        bloom::document::makeNewProject("Untitled", "Composition 1", RationalTime::fromInteger(10));
    const auto* composition = newProject.project.findComposition(newProject.initialCompositionId);
    if (!expectations.expect(newProject.project.validate().ok() && composition != nullptr,
                             "new project factory creates a valid initial composition")) {
        return;
    }

    expectations.expect(newProject.project.name() == "Untitled" &&
                            composition->name() == "Composition 1" &&
                            composition->duration() == RationalTime::fromInteger(10),
                        "new project factory preserves requested project settings");
    expectations.expect(composition->graph().nodes().size() == 2 &&
                            composition->graph().edges().size() == 1 &&
                            composition->graph().layerStack().entries().empty(),
                        "initial composition contains only stack and output topology");

    Document document(std::move(newProject.project));
    auto draft = document.draft(document.snapshot());
    expectations.expect(draft.ids().allocateComposition() == id<CompositionId>(2) &&
                            draft.ids().allocateNode() == id<NodeId>(3) &&
                            draft.ids().allocateEdge() == id<EdgeId>(2),
                        "initial topology reserves its durable IDs in the document allocator");

    bool rejectedInvalidSettings = false;
    try {
        [[maybe_unused]] auto invalid =
            bloom::document::makeNewProject("", "Composition 1", RationalTime::fromInteger(10));
    } catch (const std::invalid_argument&) {
        rejectedInvalidSettings = true;
    }
    expectations.expect(rejectedInvalidSettings,
                        "new project factory rejects incomplete project settings");
}

void testDocumentProvenance(ExpectationContext& expectations) {
    Document first(validProject());
    Document second(validProject());
    const auto firstSnapshot = first.snapshot();
    const auto secondSnapshot = second.snapshot();

    bool draftRejected = false;
    try {
        [[maybe_unused]] auto foreignDraft = first.draft(secondSnapshot);
    } catch (const bloom::document::DocumentProvenanceError&) {
        draftRejected = true;
    }
    expectations.expect(draftRejected, "cross-document snapshot cannot create a draft");
    expectations.expect(first.restore(firstSnapshot.revision(), secondSnapshot).status ==
                            CommitStatus::ForeignDocument,
                        "cross-document restore has a typed failure");

    auto secondDraft = second.draft(secondSnapshot);
    expectations.expect(first.commit(firstSnapshot.revision(), std::move(secondDraft)).status ==
                            CommitStatus::ForeignDocument,
                        "cross-document draft cannot commit");

    auto firstDraft = first.draft(firstSnapshot);
    const auto unrelatedCommit = [&]() {
        auto draft = first.draft(firstSnapshot);
        draft.project().setName("Current");
        return first.commit(firstSnapshot.revision(), std::move(draft));
    }();
    if (!expectations.expect(unrelatedCommit.committed() && unrelatedCommit.snapshot.has_value(),
                             "fixture advances current revision")) {
        return;
    }
    expectations.expect(
        first.commit(unrelatedCommit.snapshot->revision(), std::move(firstDraft)).status ==
            CommitStatus::DraftBaseMismatch,
        "draft base revision must match caller expected revision");
}

} // namespace

int main() {
    ExpectationContext expectations;
    testRationalTime(expectations);
    testIdsAndParameters(expectations);
    testCanonicalGraphAndLayerOrder(expectations);
    testDocumentSnapshots(expectations);
    testInvalidConstruction(expectations);
    testNewProjectFactory(expectations);
    testDocumentProvenance(expectations);

    return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
