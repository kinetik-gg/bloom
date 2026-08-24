#include <bloom/document/new_project.hpp>

#include <bloom/document/graph.hpp>
#include <bloom/document/persisted_text.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace bloom::document {
namespace {

constexpr ProjectId kInitialProjectId = ProjectId::fromRaw(1);
constexpr CompositionId kInitialCompositionId = CompositionId::fromRaw(1);
constexpr NodeId kInitialLayerStackNodeId = NodeId::fromRaw(1);
constexpr NodeId kInitialOutputNodeId = NodeId::fromRaw(2);
constexpr EdgeId kInitialOutputEdgeId = EdgeId::fromRaw(1);

} // namespace

NewProject makeNewProject(std::string projectName, std::string compositionName,
                          const core::RationalTime duration, const CompositionFormat format) {
    if (!isValidHumanFacingName(projectName) || !isValidHumanFacingName(compositionName)) {
        throw std::invalid_argument("New project and composition names are invalid");
    }
    if (duration <= core::RationalTime{}) {
        throw std::invalid_argument("New composition duration must be greater than zero");
    }

    CanonicalGraph graph(kInitialLayerStackNodeId);
    const bool topologyCreated =
        graph.addNode({kInitialLayerStackNodeId,
                       std::string(kLayerStackNodeType),
                       {},
                       kLayerStackNodeSchemaVersion}) &&
        graph.addNode({kInitialOutputNodeId,
                       std::string(kCompositionOutputNodeType),
                       {},
                       kCompositionOutputNodeSchemaVersion}) &&
        graph.addEdge(
            {kInitialOutputEdgeId,
             {kInitialLayerStackNodeId, std::string(kLayerStackOutputPort)},
             NodeInputRef{kInitialOutputNodeId, std::string(kCompositionOutputInputPort)}});
    if (!topologyCreated) {
        throw std::logic_error("Could not create the initial composition topology");
    }
    graph.setCompositionOutput({kInitialOutputNodeId, std::string(kCompositionOutputOutputPort)});

    Project project(kInitialProjectId, std::move(projectName));
    if (!project.addComposition(Composition(kInitialCompositionId, std::move(compositionName),
                                            duration, std::move(graph), format)) ||
        !project.validate().ok()) {
        throw std::logic_error("Could not create a valid initial Bloom project");
    }

    return {std::move(project), kInitialCompositionId};
}

} // namespace bloom::document
