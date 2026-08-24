#include <bloom/core/rational_time.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/validation.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bloom::core::RationalTime;
using bloom::document::CompositionId;
using bloom::document::ConstantValueSource;
using bloom::document::EdgeId;
using bloom::document::LayerId;
using bloom::document::LayerStackInputRef;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;
using bloom::document::NodeRecord;
using bloom::document::ParameterId;
using bloom::document::Project;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;

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

[[nodiscard]] std::string invalidUtf8() { return std::string("\xED\xA0\x80", 3); }

[[nodiscard]] Project makeProject() {
    return bloom::document::makeNewProject("Project", "Composition", RationalTime::fromInteger(2))
        .project;
}

void testFieldClassBoundaries(ExpectationContext& expectations) {
    const std::string maximumName(bloom::document::kMaxHumanFacingNameBytes, 'n');
    const std::string oversizedName(bloom::document::kMaxHumanFacingNameBytes + 1, 'n');
    const std::string maximumStructural(bloom::document::kMaxStructuralTextBytes, 's');
    const std::string oversizedStructural(bloom::document::kMaxStructuralTextBytes + 1, 's');
    const std::string maximumIdentifier(bloom::document::kMaxNamespacedIdentifierBytes, 'a');
    const std::string oversizedIdentifier(bloom::document::kMaxNamespacedIdentifierBytes + 1, 'a');

    expectations.expect(
        bloom::document::isValidHumanFacingName(maximumName) &&
            !bloom::document::isValidHumanFacingName(oversizedName) &&
            !bloom::document::isValidHumanFacingName("") &&
            !bloom::document::isValidHumanFacingName(invalidUtf8()),
        "human-facing names enforce nonempty strict UTF-8 and the 4096-byte boundary");
    expectations.expect(bloom::document::isValidStructuralText(maximumStructural) &&
                            !bloom::document::isValidStructuralText(oversizedStructural) &&
                            !bloom::document::isValidStructuralText("") &&
                            !bloom::document::isValidStructuralText(invalidUtf8()),
                        "structural text enforces nonempty strict UTF-8 and the 256-byte boundary");
    expectations.expect(bloom::document::isValidNamespacedIdentifier(maximumIdentifier) &&
                            !bloom::document::isValidNamespacedIdentifier(oversizedIdentifier) &&
                            bloom::document::isValidNamespacedIdentifier("vendor.module_type-1") &&
                            !bloom::document::isValidNamespacedIdentifier("Vendor.Module") &&
                            !bloom::document::isValidNamespacedIdentifier("vendor/module") &&
                            !bloom::document::isValidNamespacedIdentifier("_vendor.module"),
                        "namespaced IDs implement the exact lowercase 128-byte grammar");

    ValidationResult diagnostic;
    bloom::document::validateStructuralText(oversizedStructural, "field.path", "Test field",
                                            diagnostic);
    expectations.expect(hasIssue(diagnostic, ValidationCode::InvalidValue, "field.path"),
                        "central validators retain their caller-owned diagnostic path");
}

void testProjectCompositionAndLayerNames(ExpectationContext& expectations) {
    auto project = makeProject();
    project.setName(invalidUtf8());
    auto* composition = project.findComposition(id<CompositionId>(1));
    if (composition == nullptr) {
        throw std::logic_error("Text validation fixture has no composition");
    }
    composition->setName(std::string(bloom::document::kMaxHumanFacingNameBytes + 1, 'c'));
    const auto validation = project.validate();
    expectations.expect(
        hasIssue(validation, ValidationCode::InvalidValue, "name") &&
            hasIssue(validation, ValidationCode::InvalidValue, "compositions[1].name"),
        "project and composition names validate at stable owning paths");

    auto layerFixture = makeProject();
    auto* layerComposition = layerFixture.findComposition(id<CompositionId>(1));
    if (layerComposition == nullptr) {
        throw std::logic_error("Layer text fixture has no composition");
    }
    auto& graph = layerComposition->graph();
    const auto maximumName = std::string(bloom::document::kMaxHumanFacingNameBytes, 'l');
    expectations.expect(
        graph.addLayerOutput({id<NodeId>(2), id<LayerId>(1), maximumName, "image"}) &&
            !graph.addLayerOutput({id<NodeId>(3), id<LayerId>(2), invalidUtf8(), "image"}) &&
            !graph.addLayerOutput({id<NodeId>(3), id<LayerId>(2), "Layer",
                                   std::string(bloom::document::kMaxStructuralTextBytes + 1, 'p')}),
        "layer names and boundary output ports enforce their respective field classes");

    bool factoryRejected = false;
    try {
        [[maybe_unused]] auto invalid = bloom::document::makeNewProject(
            invalidUtf8(), "Composition", RationalTime::fromInteger(1));
    } catch (const std::invalid_argument&) {
        factoryRejected = true;
    }
    expectations.expect(factoryRejected,
                        "new-project construction rejects invalid persisted names");
}

void testGraphIdentifiersRolesAndPorts(ExpectationContext& expectations) {
    auto project = makeProject();
    auto* composition = project.findComposition(id<CompositionId>(1));
    if (composition == nullptr) {
        throw std::logic_error("Graph text fixture has no composition");
    }
    auto& graph = composition->graph();
    auto* stackNode = graph.findNode(id<NodeId>(1));
    if (stackNode == nullptr) {
        throw std::logic_error("Graph text fixture has no stack node");
    }
    stackNode->typeId = "Invalid.Node";
    expectations.expect(hasIssue(project.validate(), ValidationCode::InvalidValue,
                                 "compositions[1].graph.nodes[1].typeId"),
                        "node type IDs validate with a stable typed-node path");
    stackNode->typeId = std::string(bloom::document::kLayerStackNodeType);

    constexpr auto parameterId = id<ParameterId>(10);
    NodeRecord extensionNodeRecord{
        id<NodeId>(10), "vendor.module.node", {{"value", parameterId}}, 1};
    const bool bindingFixtureBuilt =
        composition->parameters().insert(
            {parameterId, "vendor.module.value", ConstantValueSource{std::int64_t{1}}}) &&
        graph.addNode(std::move(extensionNodeRecord));
    auto* extensionNode = graph.findNode(id<NodeId>(10));
    if (!bindingFixtureBuilt || extensionNode == nullptr) {
        throw std::logic_error("Could not build parameter binding text fixture");
    }
    extensionNode->parameters.front().role = invalidUtf8();
    expectations.expect(hasIssue(project.validate(), ValidationCode::InvalidValue,
                                 "compositions[1].graph.nodes[10].parameters[0]"),
                        "invalid binding bytes use a bounded ordinal diagnostic path");

    NodeRecord invalidTypeNode{id<NodeId>(20), "Invalid.Node", {}, 1};
    NodeRecord invalidRoleNode{
        id<NodeId>(20),
        "vendor.module.node",
        {{std::string(bloom::document::kMaxStructuralTextBytes + 1, 'r'), parameterId}},
        1};
    expectations.expect(
        !graph.addNode(std::move(invalidTypeNode)) && !graph.addNode(std::move(invalidRoleNode)) &&
            !graph.addEdge({id<EdgeId>(20),
                            {id<NodeId>(1), invalidUtf8()},
                            NodeInputRef{id<NodeId>(2), "image"}}) &&
            !graph.addEdge({id<EdgeId>(20),
                            {id<NodeId>(1), "image"},
                            NodeInputRef{id<NodeId>(2), invalidUtf8()}}) &&
            !graph.addEdge({id<EdgeId>(20),
                            {id<NodeId>(1), "image"},
                            LayerStackInputRef{id<NodeId>(1), id<bloom::document::LayerSlotId>(1),
                                               invalidUtf8()}}),
        "node insertion and every edge port or role reject invalid persisted text");

    graph.setCompositionOutput({id<NodeId>(2), invalidUtf8()});
    expectations.expect(hasIssue(project.validate(), ValidationCode::InvalidValue,
                                 "compositions[1].graph.compositionOutput"),
                        "composition output port validation retains the established endpoint path");
}

void testParameterSchemaAndStringPayload(ExpectationContext& expectations) {
    bloom::document::ParameterStore parameters;
    expectations.expect(
        !parameters.insert({id<ParameterId>(1), invalidUtf8(), ConstantValueSource{true}}) &&
            !parameters.insert({id<ParameterId>(1),
                                std::string(bloom::document::kMaxStructuralTextBytes + 1, 's'),
                                ConstantValueSource{true}}) &&
            !parameters.insert({id<ParameterId>(1),
                                std::string(bloom::document::kTextParameterSchemaKey),
                                ConstantValueSource{invalidUtf8()}}) &&
            parameters.insert({id<ParameterId>(1),
                               std::string(bloom::document::kTextParameterSchemaKey),
                               ConstantValueSource{std::string("e\xCC\x81")}}),
        "parameter schema keys are structural text and string constants require strict UTF-8");
}

} // namespace

int main() {
    try {
        ExpectationContext expectations;
        testFieldClassBoundaries(expectations);
        testProjectCompositionAndLayerNames(expectations);
        testGraphIdentifiersRolesAndPorts(expectations);
        testParameterSchemaAndStringPayload(expectations);
        return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
