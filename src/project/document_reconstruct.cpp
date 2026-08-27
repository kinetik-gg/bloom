#include <bloom/project/document_reconstruct.hpp>

#include <bloom/document/animation.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
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
