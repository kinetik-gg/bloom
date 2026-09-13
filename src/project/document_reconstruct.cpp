#include <bloom/project/document_reconstruct.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace bloom::project {

namespace {

// Internal signal for the per-composition assembly helpers below: nullopt means "keep going",
// otherwise the exact typed rejection to return from reconstructDocument(). Kept distinct from
// ReconstructDocumentResult itself because these helpers never produce a real ReconstructedDocument
// on success -- only reconstructDocument() constructs the final Document.
using StepResult = std::optional<ReconstructionRejected>;

[[nodiscard]] StepResult compositionRejection(const ReconstructionStage stage,
                                              const document::CompositionId compositionId,
                                              const std::uint64_t recordId) noexcept {
    return ReconstructionRejected{
        .stage = stage, .compositionId = compositionId, .recordId = recordId};
}

[[nodiscard]] ReconstructionRejected projectRejection(const ReconstructionStage stage) noexcept {
    return {.stage = stage, .compositionId = {}, .recordId = 0};
}

// One parameter the current Layer Output schema requires but an older version did not persist,
// paired with the default an upgraded node must receive. The defaults are exactly
// document::kDefaultAnchor, kDefaultScale, kDefaultRotationDegrees, and kDefaultBlendModeValue --
// the identity transform and Normal blending -- so an older file evaluates after the upgrade to the
// pixels the build that wrote it produced. They are read from the document module rather than
// restated here, so a default can never drift between the registry, the creation command, and this
// upgrade.
//
// The table spans every version below the current one at once rather than one table per version
// step, because the injection rule is already per-ROLE: a node that binds a role keeps its own
// binding. A version-1 node therefore receives anchor, scale, rotation, and blendMode, and a
// version-2 node receives only blendMode, from this one list.
struct InjectedLayerOutputParameter final {
    std::string_view role;
    std::string_view schemaKey;
    document::ParameterValue defaultValue;
};

[[nodiscard]] std::array<InjectedLayerOutputParameter, 4> injectedLayerOutputParameters() {
    return {{
        {document::kAnchorParameterRole, document::kAnchorParameterSchemaKey,
         document::kDefaultAnchor},
        {document::kScaleParameterRole, document::kScaleParameterSchemaKey,
         document::kDefaultScale},
        {document::kRotationParameterRole, document::kRotationParameterSchemaKey,
         document::kDefaultRotationDegrees},
        {document::kBlendModeParameterRole, document::kBlendModeParameterSchemaKey,
         document::kDefaultBlendModeValue},
    }};
}

// Brings decoded nodes forward to the schema version the build registers, in memory, before any
// record is installed. Only the Layer Output type has a version to upgrade from; every other
// foundation type is still at version 1, so this is deliberately a per-type rule rather than a
// generic "inject whatever the registry declares" loop -- a future type's upgrade may need to
// derive a value rather than take a default, and that decision belongs to the type.
//
// Injected ids come from one counter seeded at the document's persisted parameter high water, which
// is raised to match, so a new id can collide with nothing the file declares and the
// inclusive-watermark rule Document's constructor enforces still holds. A node that somehow already
// binds one of the new roles keeps its own binding.
[[nodiscard]] StepResult upgradeDecodedNodeSchemas(DecodedDocumentEnvelope& envelope) {
    const auto injected = injectedLayerOutputParameters();
    auto& highWater = envelope.highWater.parameter;
    for (auto& composition : envelope.compositions) {
        for (auto& node : composition.graph.nodes) {
            // Task FIX1, item I: the eight per-kind Reroute types became ONE, whose kind comes from
            // the link it sits on. A node written as one of them becomes that one type, and nothing
            // else changes -- same ports, same pass-through, same pixels -- because the kind it
            // used to name is exactly the kind its own incoming link already carries.
            if (document::isLegacyRerouteNodeType(node.typeId)) {
                node.typeId = std::string(document::kRerouteNodeType);
            }
            if (node.typeId != document::kLayerOutputNodeType ||
                node.schemaVersion >= document::kLayerOutputNodeSchemaVersion) {
                continue;
            }
            for (const auto& parameter : injected) {
                const auto bound =
                    std::ranges::any_of(node.parameters, [&parameter](const auto& binding) {
                        return binding.role == parameter.role;
                    });
                if (bound) {
                    continue;
                }
                if (highWater == std::numeric_limits<std::uint64_t>::max()) {
                    return compositionRejection(ReconstructionStage::NodeSchemaUpgrade,
                                                composition.id, node.id.value());
                }
                ++highWater;
                const auto parameterId = document::ParameterId::fromRaw(highWater);
                composition.parameters.push_back(
                    {parameterId, std::string(parameter.schemaKey),
                     document::ConstantValueSource{parameter.defaultValue}});
                node.parameters.push_back({std::string(parameter.role), parameterId});
            }
            // Canonical binding order -- UTF-8 by role, then numeric id -- so an upgraded node is
            // indistinguishable in ordering from one the canonical writer emitted.
            std::ranges::sort(node.parameters, [](const auto& left, const auto& right) {
                return left.role != right.role ? left.role < right.role
                                               : left.parameterId < right.parameterId;
            });
            node.schemaVersion = document::kLayerOutputNodeSchemaVersion;
        }
    }
    return std::nullopt;
}

// Assembles one composition's canonical graph through CanonicalGraph's own checked adders, moving
// every decoded record in. Every rejection is reported with the owning composition id and the
// specific node/edge/layer/slot id the offending adder was given.
[[nodiscard]] StepResult buildGraph(const document::CompositionId compositionId,
                                    DecodedGraph& decodedGraph, document::CanonicalGraph& graph) {
    for (auto& node : decodedGraph.nodes) {
        const auto nodeId = node.id;
        if (!graph.addNode(std::move(node))) {
            return compositionRejection(ReconstructionStage::GraphNode, compositionId,
                                        nodeId.value());
        }
    }
    for (auto& edge : decodedGraph.edges) {
        const auto edgeId = edge.id;
        // An edge whose SOURCE is a sink is dropped rather than refused (task FIX1, item H). The
        // composition Output declares no output port now, and a document written before that could
        // carry an edge from it -- one the compiler never followed, because reachability walks
        // BACKWARDS from the output and nothing downstream of it exists. Refusing the archive over
        // a link that never meant anything would lose the artist's whole project to a record that
        // was already inert. It is dropped silently: this reconstruction path reports rejections,
        // not warnings, and inventing a warning channel for it belongs with the one the open
        // pipeline will need for every other advisory rather than here.
        const auto* source = graph.findNode(edge.source.nodeId);
        const auto* definition =
            source == nullptr
                ? nullptr
                : document::builtInNodeDefinitions().find(source->typeId, source->schemaVersion);
        if (definition != nullptr && definition->outputs.empty()) {
            continue;
        }
        if (!graph.addEdge(std::move(edge))) {
            return compositionRejection(ReconstructionStage::GraphEdge, compositionId,
                                        edgeId.value());
        }
    }
    for (auto& boundary : decodedGraph.layerOutputs) {
        const auto layerId = boundary.layerId;
        if (!graph.addLayerOutput(std::move(boundary))) {
            return compositionRejection(ReconstructionStage::LayerOutput, compositionId,
                                        layerId.value());
        }
    }
    for (const auto& entry : decodedGraph.layerStack.entries) {
        if (!graph.layerStack().append(entry)) {
            return compositionRejection(ReconstructionStage::LayerStackEntry, compositionId,
                                        entry.slotId.value());
        }
    }
    graph.setCompositionOutput(std::move(decodedGraph.compositionOutput));
    return std::nullopt;
}

// Assembles one composition's parameters and animation curves through ParameterStore::insert() and
// AnimationCurveStore::insert() -- the stores' own checked adders, applying curve ownership and
// schema/value agreement that this function does not duplicate.
[[nodiscard]] StepResult buildStores(const document::CompositionId compositionId,
                                     DecodedComposition& decodedComposition,
                                     document::Composition& composition) {
    for (auto& parameter : decodedComposition.parameters) {
        const auto parameterId = parameter.id;
        if (!composition.parameters().insert(std::move(parameter))) {
            return compositionRejection(ReconstructionStage::ParameterStore, compositionId,
                                        parameterId.value());
        }
    }
    for (auto& curve : decodedComposition.animationCurves) {
        const auto curveId = document::animationCurveId(curve);
        if (!composition.animationCurves().insert(std::move(curve))) {
            return compositionRejection(ReconstructionStage::AnimationCurveStore, compositionId,
                                        curveId.value());
        }
    }
    return std::nullopt;
}

} // namespace

ReconstructDocumentResult ReconstructDocumentResult::success(ReconstructedDocument result) {
    ReconstructDocumentResult out;
    out.succeeded_ = true;
    out.result_ = std::move(result);
    return out;
}

ReconstructDocumentResult ReconstructDocumentResult::failure(ReconstructionRejected rejection) {
    ReconstructDocumentResult out;
    out.succeeded_ = false;
    out.rejection_ = rejection;
    return out;
}

ReconstructDocumentResult reconstructDocument(DecodedDocumentEnvelope envelope) {
    if (const auto rejection = upgradeDecodedNodeSchemas(envelope); rejection.has_value()) {
        return ReconstructDocumentResult::failure(*rejection);
    }

    document::Project project(envelope.projectId, std::move(envelope.projectName));

    for (auto& decodedComposition : envelope.compositions) {
        const auto compositionId = decodedComposition.id;

        document::CanonicalGraph graph(decodedComposition.graph.layerStack.nodeId);
        if (const auto rejection = buildGraph(compositionId, decodedComposition.graph, graph);
            rejection.has_value()) {
            return ReconstructDocumentResult::failure(*rejection);
        }

        document::Composition composition(compositionId, std::move(decodedComposition.name),
                                          decodedComposition.duration, std::move(graph),
                                          decodedComposition.format);

        if (const auto rejection = buildStores(compositionId, decodedComposition, composition);
            rejection.has_value()) {
            return ReconstructDocumentResult::failure(*rejection);
        }

        composition.nodeLayout() = std::move(decodedComposition.nodeLayout);
        composition.nodeGroups() = std::move(decodedComposition.nodeGroups);

        if (!project.addComposition(std::move(composition))) {
            return ReconstructDocumentResult::failure({.stage = ReconstructionStage::CompositionAdd,
                                                       .compositionId = compositionId,
                                                       .recordId = 0});
        }
    }

    for (auto& record : envelope.extensionRecords) {
        const auto recordId = record.id;
        if (!project.addExtensionRecord(std::move(record))) {
            return ReconstructDocumentResult::failure(
                {.stage = ReconstructionStage::ExtensionRecordAdd,
                 .compositionId = {},
                 .recordId = recordId.value()});
        }
    }

    const auto validation = project.validate();
    if (!validation.ok()) {
        return ReconstructDocumentResult::failure(
            projectRejection(ReconstructionStage::ProjectValidate));
    }

    // Document(Project, IdAllocatorHighWater) re-validates the project (already known-valid above)
    // and then checks the contract's inclusive-watermark rule: every declared id must already be
    // covered by its namespace's persisted high water (see docs/architecture/project-format.md,
    // "Inclusive Allocator State"). Both failure paths throw plain std::invalid_argument --
    // DocumentProvenanceError (thrown only by Document::draft() for a foreign snapshot) derives
    // from it but is not itself thrown here; catching the base class is still the correct, and
    // only, truthful boundary so no document-model exception ever escapes reconstruction.
    try {
        auto document =
            std::make_unique<document::Document>(std::move(project), envelope.highWater);
        return ReconstructDocumentResult::success(
            {std::move(document), std::move(envelope.colorSettings)});
    } catch (const std::invalid_argument&) {
        return ReconstructDocumentResult::failure(
            projectRejection(ReconstructionStage::DocumentConstruct));
    }
}

} // namespace bloom::project
