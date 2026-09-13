#pragma once

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/result.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/layer_stack.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::commands::test {

using document::CanonicalGraph;
using document::Composition;
using document::CompositionId;
using document::ConstantValueSource;
using document::Document;
using document::EdgeId;
using document::LayerId;
using document::LayerOutputBoundary;
using document::LayerSlotId;
using document::LayerStackInputRef;
using document::NodeId;
using document::ParameterId;
using document::Project;
using document::ProjectId;
using document::Vec2d;

inline constexpr ProjectId kProjectId = ProjectId::fromRaw(1);
inline constexpr CompositionId kCompositionId = CompositionId::fromRaw(10);
inline constexpr NodeId kLayerStackNodeId = NodeId::fromRaw(20);
inline constexpr NodeId kFirstLayerNodeId = NodeId::fromRaw(21);
inline constexpr NodeId kSecondLayerNodeId = NodeId::fromRaw(22);
inline constexpr LayerId kFirstLayerId = LayerId::fromRaw(30);
inline constexpr LayerId kSecondLayerId = LayerId::fromRaw(31);
inline constexpr LayerSlotId kFirstSlotId = LayerSlotId::fromRaw(40);
inline constexpr LayerSlotId kSecondSlotId = LayerSlotId::fromRaw(41);
inline constexpr EdgeId kFirstEdgeId = EdgeId::fromRaw(50);
inline constexpr EdgeId kSecondEdgeId = EdgeId::fromRaw(51);
inline constexpr ParameterId kOpacityId = ParameterId::fromRaw(60);
inline constexpr ParameterId kSecondOpacityId = ParameterId::fromRaw(61);
inline constexpr ParameterId kFirstPositionId = ParameterId::fromRaw(62);
inline constexpr ParameterId kSecondPositionId = ParameterId::fromRaw(63);
inline constexpr ParameterId kFirstAnchorId = ParameterId::fromRaw(64);
inline constexpr ParameterId kSecondAnchorId = ParameterId::fromRaw(65);
inline constexpr ParameterId kFirstScaleId = ParameterId::fromRaw(66);
inline constexpr ParameterId kSecondScaleId = ParameterId::fromRaw(67);
inline constexpr ParameterId kFirstRotationId = ParameterId::fromRaw(68);
inline constexpr ParameterId kSecondRotationId = ParameterId::fromRaw(69);
// ADAPTED (blend modes): the Layer Output schema now also requires a blendMode binding.
inline constexpr ParameterId kFirstBlendModeId = ParameterId::fromRaw(70);
inline constexpr ParameterId kSecondBlendModeId = ParameterId::fromRaw(71);

class TestContext final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    void fail(const std::string_view message,
              const std::source_location location = std::source_location::current()) {
        expect(false, message, location);
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

inline void requireFixture(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::logic_error(std::string(message));
    }
}

[[nodiscard]] inline Project makeProject() {
    CanonicalGraph graph(kLayerStackNodeId);
    requireFixture(graph.addNode({kLayerStackNodeId,
                                  std::string(document::kLayerStackNodeType),
                                  {},
                                  document::kLayerStackNodeSchemaVersion}),
                   "fixture Layer Stack node must be accepted");
    requireFixture(
        graph.addNode({kFirstLayerNodeId,
                       std::string(document::kLayerOutputNodeType),
                       {
                           {std::string(document::kPositionParameterRole), kFirstPositionId},
                           {std::string(document::kAnchorParameterRole), kFirstAnchorId},
                           {std::string(document::kScaleParameterRole), kFirstScaleId},
                           {std::string(document::kRotationParameterRole), kFirstRotationId},
                           {std::string(document::kOpacityParameterRole), kOpacityId},
                           {std::string(document::kBlendModeParameterRole), kFirstBlendModeId},
                       },
                       document::kLayerOutputNodeSchemaVersion}),
        "fixture first Layer Output node must be accepted");
    requireFixture(
        graph.addNode({kSecondLayerNodeId,
                       std::string(document::kLayerOutputNodeType),
                       {
                           {std::string(document::kPositionParameterRole), kSecondPositionId},
                           {std::string(document::kAnchorParameterRole), kSecondAnchorId},
                           {std::string(document::kScaleParameterRole), kSecondScaleId},
                           {std::string(document::kRotationParameterRole), kSecondRotationId},
                           {std::string(document::kOpacityParameterRole), kSecondOpacityId},
                           {std::string(document::kBlendModeParameterRole), kSecondBlendModeId},
                       },
                       document::kLayerOutputNodeSchemaVersion}),
        "fixture second Layer Output node must be accepted");
    requireFixture(graph.addLayerOutput({kFirstLayerNodeId, kFirstLayerId, "First", "image"}),
                   "fixture first Layer Output boundary must be accepted");
    requireFixture(graph.addLayerOutput({kSecondLayerNodeId, kSecondLayerId, "Second", "image"}),
                   "fixture second Layer Output boundary must be accepted");
    requireFixture(graph.layerStack().append({kFirstSlotId, kFirstLayerId}),
                   "fixture first layer slot must be accepted");
    requireFixture(graph.layerStack().append({kSecondSlotId, kSecondLayerId}),
                   "fixture second layer slot must be accepted");
    requireFixture(
        graph.addEdge({kFirstEdgeId,
                       {kFirstLayerNodeId, "image"},
                       LayerStackInputRef{kLayerStackNodeId, kFirstSlotId,
                                          std::string(document::kLayerStackContentInputRole)}}),
        "fixture first stack edge must be accepted");
    requireFixture(
        graph.addEdge({kSecondEdgeId,
                       {kSecondLayerNodeId, "image"},
                       LayerStackInputRef{kLayerStackNodeId, kSecondSlotId,
                                          std::string(document::kLayerStackContentInputRole)}}),
        "fixture second stack edge must be accepted");
    graph.setCompositionOutput({kLayerStackNodeId, "image"});

    Composition composition(kCompositionId, "Main", core::RationalTime::fromInteger(10),
                            std::move(graph));
    requireFixture(composition.parameters().insert(
                       {kOpacityId, std::string(document::kOpacityParameterSchemaKey),
                        ConstantValueSource{1.0}}),
                   "fixture opacity parameter must be accepted");
    requireFixture(composition.parameters().insert(
                       {kSecondOpacityId, std::string(document::kOpacityParameterSchemaKey),
                        ConstantValueSource{0.75}}),
                   "fixture second opacity parameter must be accepted");
    requireFixture(composition.parameters().insert(
                       {kFirstPositionId, std::string(document::kPositionParameterSchemaKey),
                        ConstantValueSource{Vec2d{0.0, 0.0}}}),
                   "fixture first position parameter must be accepted");
    requireFixture(composition.parameters().insert(
                       {kSecondPositionId, std::string(document::kPositionParameterSchemaKey),
                        ConstantValueSource{Vec2d{0.0, 0.0}}}),
                   "fixture second position parameter must be accepted");
    // The three transform breadth parameters and the blend mode every Layer Output now binds. Both
    // layers carry them at their schema defaults -- the identity transform and Normal blending --
    // so a fixture that says nothing about either behaves exactly as it did before task S4.
    for (const auto& [anchorId, scaleId, rotationId, blendModeId] :
         {std::tuple{kFirstAnchorId, kFirstScaleId, kFirstRotationId, kFirstBlendModeId},
          std::tuple{kSecondAnchorId, kSecondScaleId, kSecondRotationId, kSecondBlendModeId}}) {
        requireFixture(composition.parameters().insert(
                           {anchorId, std::string(document::kAnchorParameterSchemaKey),
                            ConstantValueSource{document::kDefaultAnchor}}),
                       "fixture anchor parameter must be accepted");
        requireFixture(composition.parameters().insert(
                           {scaleId, std::string(document::kScaleParameterSchemaKey),
                            ConstantValueSource{document::kDefaultScale}}),
                       "fixture scale parameter must be accepted");
        requireFixture(composition.parameters().insert(
                           {rotationId, std::string(document::kRotationParameterSchemaKey),
                            ConstantValueSource{document::kDefaultRotationDegrees}}),
                       "fixture rotation parameter must be accepted");
        requireFixture(composition.parameters().insert(
                           {blendModeId, std::string(document::kBlendModeParameterSchemaKey),
                            ConstantValueSource{document::kDefaultBlendModeValue}}),
                       "fixture blend mode parameter must be accepted");
    }

    Project project(kProjectId, "Original Project");
    requireFixture(project.addComposition(std::move(composition)),
                   "fixture composition must be accepted");
    requireFixture(project.validate().ok(), "fixture project must validate");
    return project;
}

[[nodiscard]] inline const Composition& composition(const document::Snapshot& snapshot) {
    const auto* result = snapshot.project().findComposition(kCompositionId);
    requireFixture(result != nullptr, "fixture composition must remain addressable");
    return *result;
}

[[nodiscard]] inline double opacity(const document::Snapshot& snapshot) {
    const auto* parameter = composition(snapshot).parameters().find(kOpacityId);
    requireFixture(parameter != nullptr, "fixture opacity must remain addressable");
    const auto* constant = std::get_if<ConstantValueSource>(&parameter->source);
    requireFixture(constant != nullptr, "fixture opacity must remain constant");
    const auto* value = std::get_if<double>(&constant->value);
    requireFixture(value != nullptr, "fixture opacity must remain a double");
    return *value;
}

[[nodiscard]] inline std::vector<LayerSlotId> layerOrder(const document::Snapshot& snapshot) {
    std::vector<LayerSlotId> result;
    for (const auto& entry : composition(snapshot).graph().layerStack().entries()) {
        result.push_back(entry.slotId);
    }
    return result;
}

} // namespace bloom::commands::test
