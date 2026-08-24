#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::core::RationalTime;
using bloom::document::AnimationCurveId;
using bloom::document::AnimationCurveSource;
using bloom::document::CanonicalGraph;
using bloom::document::CommitStatus;
using bloom::document::Composition;
using bloom::document::CompositionId;
using bloom::document::ConstantValueSource;
using bloom::document::Document;
using bloom::document::DriverBindingId;
using bloom::document::EdgeId;
using bloom::document::ExtensionHostReference;
using bloom::document::ExtensionHostReferenceTable;
using bloom::document::ExtensionOwnerRemapper;
using bloom::document::ExtensionRecord;
using bloom::document::ExtensionRecordId;
using bloom::document::ExtensionTarget;
using bloom::document::IdAllocatorHighWater;
using bloom::document::KeyframeId;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;
using bloom::document::NodeRecord;
using bloom::document::NoExtensionReferences;
using bloom::document::OpaqueExtensionPayload;
using bloom::document::ParameterId;
using bloom::document::Project;
using bloom::document::ProjectId;
using bloom::document::ScalarAnimationCurve;
using bloom::document::ScalarKeyframe;
using bloom::document::SchemaVersion;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::document::Vec2d;

static_assert(!std::constructible_from<ExtensionTarget, DriverBindingId>);

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

[[nodiscard]] bool hasIssue(const ValidationResult& validation, const ValidationCode code,
                            const std::string_view path) {
    return std::ranges::any_of(validation.issues(), [&](const auto& issue) {
        return issue.code == code && issue.path == path;
    });
}

[[nodiscard]] ExtensionRecord record(const std::uint64_t recordId,
                                     std::optional<ExtensionTarget> subject = std::nullopt) {
    return {
        .id = id<ExtensionRecordId>(recordId),
        .ownerId = "vendor.module",
        .typeId = "vendor.module.record-type",
        .schemaVersion = SchemaVersion{1, 0},
        .subject = subject,
        .mediaType = "application/octet-stream",
        .referencePolicy = NoExtensionReferences{},
        .payload = {std::byte{0x00}, std::byte{0x7F}, std::byte{0xFF}},
    };
}

struct FixtureIds final {
    ProjectId project = id<ProjectId>(1);
    CompositionId firstComposition = id<CompositionId>(1);
    CompositionId secondComposition = id<CompositionId>(2);
    NodeId sourceNode = id<NodeId>(10);
    NodeId layerOutputNode = id<NodeId>(11);
    NodeId secondStackNode = id<NodeId>(100);
    EdgeId sourceEdge = id<EdgeId>(10);
    LayerId layer = id<LayerId>(10);
    LayerSlotId layerSlot = id<LayerSlotId>(10);
    ParameterId positionParameter = id<ParameterId>(10);
    ParameterId opacityParameter = id<ParameterId>(11);
    ParameterId animationParameter = id<ParameterId>(12);
    AnimationCurveId animationCurve = id<AnimationCurveId>(20);
    KeyframeId keyframe = id<KeyframeId>(20);
};

inline constexpr FixtureIds kIds;

[[nodiscard]] Project makeProjectWithTargets() {
    auto created =
        bloom::document::makeNewProject("Project", "First", RationalTime::fromInteger(2));
    auto* first = created.project.findComposition(kIds.firstComposition);
    if (first == nullptr) {
        throw std::logic_error("Initial composition is missing");
    }

    NodeRecord source{kIds.sourceNode, "vendor.module.source", {}, 1};
    NodeRecord layerOutput{
        kIds.layerOutputNode,
        std::string(bloom::document::kLayerOutputNodeType),
        {
            {std::string(bloom::document::kPositionParameterRole), kIds.positionParameter},
            {std::string(bloom::document::kOpacityParameterRole), kIds.opacityParameter},
        },
        bloom::document::kLayerOutputNodeSchemaVersion,
    };
    const bool firstExtended =
        first->animationCurves().insert(ScalarAnimationCurve{
            kIds.animationCurve,
            {ScalarKeyframe{kIds.keyframe, RationalTime::fromInteger(0), 0.5}},
        }) &&
        first->parameters().insert({kIds.positionParameter,
                                    std::string(bloom::document::kPositionParameterSchemaKey),
                                    ConstantValueSource{Vec2d{0.0, 0.0}}}) &&
        first->parameters().insert({kIds.opacityParameter,
                                    std::string(bloom::document::kOpacityParameterSchemaKey),
                                    ConstantValueSource{1.0}}) &&
        first->parameters().insert({kIds.animationParameter,
                                    std::string(bloom::document::kOpacityParameterSchemaKey),
                                    AnimationCurveSource{kIds.animationCurve}}) &&
        first->graph().addNode(std::move(source)) &&
        first->graph().addNode(std::move(layerOutput)) &&
        first->graph().addLayerOutput({kIds.layerOutputNode, kIds.layer, "Layer",
                                       std::string(bloom::document::kLayerOutputOutputPort)}) &&
        first->graph().layerStack().append({kIds.layerSlot, kIds.layer}) &&
        first->graph().addEdge(
            {kIds.sourceEdge,
             {kIds.sourceNode, "image"},
             NodeInputRef{kIds.layerOutputNode,
                          std::string(bloom::document::kLayerOutputContentInputPort)}}) &&
        first->graph().addEdge(
            {id<EdgeId>(11),
             {kIds.layerOutputNode, std::string(bloom::document::kLayerOutputOutputPort)},
             LayerStackInputRef{first->graph().layerStack().nodeId(), kIds.layerSlot,
                                std::string(bloom::document::kLayerStackContentInputRole)}});

    CanonicalGraph secondGraph(kIds.secondStackNode);
    const auto secondOutputNode = id<NodeId>(101);
    const bool secondBuilt =
        secondGraph.addNode({kIds.secondStackNode,
                             std::string(bloom::document::kLayerStackNodeType),
                             {},
                             bloom::document::kLayerStackNodeSchemaVersion}) &&
        secondGraph.addNode({secondOutputNode,
                             std::string(bloom::document::kCompositionOutputNodeType),
                             {},
                             bloom::document::kCompositionOutputNodeSchemaVersion}) &&
        secondGraph.addEdge(
            {id<EdgeId>(100),
             {kIds.secondStackNode, std::string(bloom::document::kLayerStackOutputPort)},
             NodeInputRef{secondOutputNode,
                          std::string(bloom::document::kCompositionOutputInputPort)}});
    secondGraph.setCompositionOutput(
        {secondOutputNode, std::string(bloom::document::kCompositionOutputOutputPort)});
    const bool secondAdded =
        secondBuilt && created.project.addComposition(Composition(kIds.secondComposition, "Second",
                                                                  RationalTime::fromInteger(2),
                                                                  std::move(secondGraph)));
    if (!firstExtended || !secondAdded || !created.project.validate().ok()) {
        throw std::logic_error("Could not build extension target fixture");
    }
    return std::move(created.project);
}

void testStorageAndAllocatorPublication(ExpectationContext& expectations) {
    auto project = makeProjectWithTargets();
    expectations.expect(project.addExtensionRecord(record(9, ExtensionTarget{kIds.project})) &&
                            project.findExtensionRecord(id<ExtensionRecordId>(9)) != nullptr &&
                            project.validate().ok(),
                        "project-global extension records support add and lookup");

    Document inventoried(std::move(project));
    const auto initial = inventoried.snapshot();
    constexpr std::array expectedPayload{std::byte{0x00}, std::byte{0x7F}, std::byte{0xFF}};
    const auto* initialRecord = initial.project().findExtensionRecord(id<ExtensionRecordId>(9));
    expectations.expect(
        initial.ids().highWater().extensionRecord == 9 && initialRecord != nullptr &&
            std::ranges::equal(initialRecord->payload.bytes(), expectedPayload),
        "construction inventories extension IDs and preserves exact opaque payload bytes");

    auto removal = inventoried.draft(initial);
    const bool removed = removal.project().removeExtensionRecord(id<ExtensionRecordId>(9));
    const auto withoutRecord = inventoried.commit(initial.revision(), std::move(removal));
    if (!expectations.expect(removed && withoutRecord.committed() && withoutRecord.snapshot &&
                                 withoutRecord.snapshot->project().extensionRecords().empty() &&
                                 withoutRecord.snapshot->ids().highWater().extensionRecord == 9,
                             "committing removal preserves the allocator high-water gap")) {
        return;
    }
    if (!withoutRecord.snapshot.has_value()) {
        return;
    }

    auto next = inventoried.draft(*withoutRecord.snapshot);
    const auto nextId = next.ids().allocateExtensionRecord();
    const bool nextAdded =
        nextId.has_value() && next.project().addExtensionRecord(record(nextId->value()));
    const auto readded = inventoried.commit(withoutRecord.snapshot->revision(), std::move(next));
    if (!expectations.expect(nextAdded && readded.committed() && readded.snapshot &&
                                 nextId == id<ExtensionRecordId>(10) &&
                                 readded.snapshot->ids().highWater().extensionRecord == 10,
                             "a deleted extension ID is never reused by a later draft")) {
        return;
    }
    if (!readded.snapshot.has_value()) {
        return;
    }

    const auto restored = inventoried.restore(readded.snapshot->revision(), initial);
    expectations.expect(
        restored.committed() && restored.snapshot &&
            restored.snapshot->project().findExtensionRecord(id<ExtensionRecordId>(9)) != nullptr &&
            restored.snapshot->ids().highWater().extensionRecord == 10,
        "historical restore recovers record truth without lowering high water");

    auto persisted = initial.ids().highWater();
    persisted.extensionRecord = 8;
    bool rejected = false;
    try {
        auto invalidProject = makeProjectWithTargets();
        const bool inserted = invalidProject.addExtensionRecord(record(9));
        if (!inserted) {
            throw std::logic_error("Could not add persisted allocator fixture");
        }
        [[maybe_unused]] Document invalid(std::move(invalidProject), persisted);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expectations.expect(rejected,
                        "persisted construction rejects extension high water below a declaration");
}

void testTypedSubjectsAndCrossCompositionReferences(ExpectationContext& expectations) {
    auto project = makeProjectWithTargets();
    const std::array<ExtensionTarget, 9> subjects{
        ExtensionTarget{kIds.project},
        ExtensionTarget{kIds.firstComposition},
        ExtensionTarget{kIds.sourceNode},
        ExtensionTarget{kIds.sourceEdge},
        ExtensionTarget{kIds.layer},
        ExtensionTarget{kIds.layerSlot},
        ExtensionTarget{kIds.positionParameter},
        ExtensionTarget{kIds.animationCurve},
        ExtensionTarget{kIds.keyframe},
    };
    for (std::size_t index = 0; index < subjects.size(); ++index) {
        if (!project.addExtensionRecord(record(index + 1, subjects[index]))) {
            throw std::logic_error("Could not add typed subject fixture");
        }
    }

    auto crossComposition = record(10, ExtensionTarget{kIds.secondComposition});
    crossComposition.referencePolicy = ExtensionHostReferenceTable{{
        ExtensionHostReference{"first-node", ExtensionTarget{kIds.sourceNode}},
        ExtensionHostReference{"second-node", ExtensionTarget{kIds.secondStackNode}},
    }};
    const bool crossCompositionAdded = project.addExtensionRecord(std::move(crossComposition));
    expectations.expect(crossCompositionAdded && project.validate().ok(),
                        "every subject kind and cross-composition host targets resolve globally");

    auto orphanSubject = makeProjectWithTargets();
    const bool orphanAdded = orphanSubject.addExtensionRecord(
        record(1, ExtensionTarget{id<NodeId>(std::numeric_limits<std::uint64_t>::max())}));
    expectations.expect(orphanAdded &&
                            hasIssue(orphanSubject.validate(), ValidationCode::MissingReference,
                                     "extensionRecords[1].subject"),
                        "an orphan typed subject is rejected");

    auto orphanTable = makeProjectWithTargets();
    auto orphanRecord = record(1);
    orphanRecord.referencePolicy = ExtensionHostReferenceTable{{
        ExtensionHostReference{"missing", ExtensionTarget{id<LayerId>(999)}},
    }};
    const bool orphanTableAdded = orphanTable.addExtensionRecord(std::move(orphanRecord));
    expectations.expect(orphanTableAdded &&
                            hasIssue(orphanTable.validate(), ValidationCode::MissingReference,
                                     "extensionRecords[1].referencePolicy.references[0].target"),
                        "an orphan host-table target is rejected");
}

void testEnvelopeAndReferencePolicyValidation(ExpectationContext& expectations) {
    auto valid = makeProjectWithTargets();
    auto remapped = record(1);
    remapped.referencePolicy =
        ExtensionOwnerRemapper{"unavailable.owner.record-remapper", SchemaVersion{7, 3}};
    const auto originalPayload = remapped.payload;
    const bool remappedAdded = valid.addExtensionRecord(std::move(remapped));
    if (!expectations.expect(remappedAdded && valid.validate().ok(),
                             "an unavailable owner remapper does not invalidate the project")) {
        return;
    }
    Document retained(std::move(valid));
    const auto retainedSnapshot = retained.snapshot();
    const auto* retainedRecord =
        retainedSnapshot.project().findExtensionRecord(id<ExtensionRecordId>(1));
    expectations.expect(retainedRecord != nullptr && retainedRecord->payload == originalPayload,
                        "unavailable owner handling is neutral to exact opaque bytes");

    auto unordered = makeProjectWithTargets();
    auto unorderedRecord = record(1);
    unorderedRecord.referencePolicy = ExtensionHostReferenceTable{{
        ExtensionHostReference{"zeta", ExtensionTarget{kIds.project}},
        ExtensionHostReference{"alpha", ExtensionTarget{kIds.project}},
    }};
    const bool unorderedAdded = unordered.addExtensionRecord(std::move(unorderedRecord));
    expectations.expect(unorderedAdded &&
                            hasIssue(unordered.validate(), ValidationCode::InvalidOrder,
                                     "extensionRecords[1].referencePolicy.references[1].key"),
                        "host-table keys must be sorted by UTF-8 bytes");

    auto duplicate = makeProjectWithTargets();
    auto duplicateRecord = record(1);
    duplicateRecord.referencePolicy = ExtensionHostReferenceTable{{
        ExtensionHostReference{"same", ExtensionTarget{kIds.project}},
        ExtensionHostReference{"same", ExtensionTarget{kIds.project}},
    }};
    const bool duplicateAdded = duplicate.addExtensionRecord(std::move(duplicateRecord));
    expectations.expect(duplicateAdded &&
                            hasIssue(duplicate.validate(), ValidationCode::InvalidOrder,
                                     "extensionRecords[1].referencePolicy.references[1].key"),
                        "host-table keys must be unique");

    auto oversizedKey = makeProjectWithTargets();
    auto oversizedKeyRecord = record(1);
    oversizedKeyRecord.referencePolicy = ExtensionHostReferenceTable{{
        ExtensionHostReference{std::string(bloom::document::kMaxStructuralTextBytes + 1, 'k'),
                               ExtensionTarget{kIds.project}},
    }};
    const bool oversizedKeyAdded = oversizedKey.addExtensionRecord(std::move(oversizedKeyRecord));
    expectations.expect(oversizedKeyAdded &&
                            hasIssue(oversizedKey.validate(), ValidationCode::InvalidValue,
                                     "extensionRecords[1].referencePolicy.references[0].key"),
                        "host-table keys enforce their 256-byte field ceiling");

    auto invalidFields = makeProjectWithTargets();
    auto invalidRecord = record(1);
    invalidRecord.ownerId = "Vendor.Module";
    invalidRecord.typeId = std::string(bloom::document::kMaxNamespacedIdentifierBytes + 1, 'a');
    invalidRecord.schemaVersion = {};
    invalidRecord.mediaType = std::string("\xC0\x80", 2);
    invalidRecord.referencePolicy = ExtensionOwnerRemapper{"invalid/remapper", {}};
    const bool invalidAdded = invalidFields.addExtensionRecord(std::move(invalidRecord));
    const auto invalidValidation = invalidFields.validate();
    expectations.expect(invalidAdded &&
                            hasIssue(invalidValidation, ValidationCode::InvalidValue,
                                     "extensionRecords[1].ownerId") &&
                            hasIssue(invalidValidation, ValidationCode::InvalidValue,
                                     "extensionRecords[1].typeId") &&
                            hasIssue(invalidValidation, ValidationCode::InvalidValue,
                                     "extensionRecords[1].schemaVersion") &&
                            hasIssue(invalidValidation, ValidationCode::InvalidValue,
                                     "extensionRecords[1].mediaType") &&
                            hasIssue(invalidValidation, ValidationCode::InvalidValue,
                                     "extensionRecords[1].referencePolicy.remapperId") &&
                            hasIssue(invalidValidation, ValidationCode::InvalidValue,
                                     "extensionRecords[1].referencePolicy.version"),
                        "envelope identifiers, versions, and UTF-8 field ceilings are validated");
}

void testRecordIdentityAndPayloadLimits(ExpectationContext& expectations) {
    auto duplicates = makeProjectWithTargets();
    const bool zeroRejected = !duplicates.addExtensionRecord(record(0));
    const bool firstAdded = duplicates.addExtensionRecord(record(1));
    const bool duplicateRejected = !duplicates.addExtensionRecord(record(1));
    const bool secondAdded = duplicates.addExtensionRecord(record(2));
    auto* second = duplicates.findExtensionRecord(id<ExtensionRecordId>(2));
    if (second != nullptr) {
        second->id = id<ExtensionRecordId>(1);
    }
    const auto duplicateValidation = duplicates.validate();
    expectations.expect(
        zeroRejected && firstAdded && duplicateRejected && secondAdded && second != nullptr &&
            hasIssue(duplicateValidation, ValidationCode::DuplicateId, "extensionRecords[1].id"),
        "add rejects zero and duplicate IDs while validation catches mutated duplicates");

    auto oversized = makeProjectWithTargets();
    auto oversizedRecord = record(1);
    oversizedRecord.payload = OpaqueExtensionPayload(
        std::vector<std::byte>(bloom::document::kMaxOpaqueExtensionPayloadBytes + 1));
    const bool oversizedAdded = oversized.addExtensionRecord(std::move(oversizedRecord));
    expectations.expect(oversizedAdded &&
                            hasIssue(oversized.validate(), ValidationCode::InvalidValue,
                                     "extensionRecords[1].payload"),
                        "one opaque payload cannot exceed 64 MiB");

    auto aggregate = makeProjectWithTargets();
    auto first = record(1);
    auto secondPayload = record(2);
    auto finalByte = record(3);
    const OpaqueExtensionPayload maximumPayload{
        std::vector<std::byte>(bloom::document::kMaxOpaqueExtensionPayloadBytes)};
    first.payload = maximumPayload;
    secondPayload.payload = maximumPayload;
    finalByte.payload = OpaqueExtensionPayload{std::byte{0x00}};
    const bool boundaryAdded = aggregate.addExtensionRecord(std::move(first)) &&
                               aggregate.addExtensionRecord(std::move(secondPayload));
    const bool exactBoundaryIsValid = boundaryAdded && aggregate.validate().ok();
    const bool finalByteAdded = aggregate.addExtensionRecord(std::move(finalByte));
    expectations.expect(
        exactBoundaryIsValid && finalByteAdded &&
            hasIssue(aggregate.validate(), ValidationCode::InvalidValue, "extensionRecords"),
        "128 MiB of payload is valid and one further byte exceeds the aggregate limit");
}

} // namespace

int main() {
    try {
        ExpectationContext expectations;
        testStorageAndAllocatorPublication(expectations);
        testTypedSubjectsAndCrossCompositionReferences(expectations);
        testEnvelopeAndReferencePolicyValidation(expectations);
        testRecordIdentityAndPayloadLimits(expectations);
        return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
