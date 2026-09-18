#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include "snapshot_compiler_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace bloom;

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kFirstSolidNode = document::NodeId::fromRaw(10);
constexpr auto kFirstLayerNode = document::NodeId::fromRaw(11);
constexpr auto kSecondSolidNode = document::NodeId::fromRaw(12);
constexpr auto kSecondLayerNode = document::NodeId::fromRaw(13);
constexpr auto kStackNode = document::NodeId::fromRaw(20);
constexpr auto kOutputNode = document::NodeId::fromRaw(21);
constexpr auto kFirstColor = document::ParameterId::fromRaw(30);
constexpr auto kSecondColor = document::ParameterId::fromRaw(31);
constexpr auto kFirstPosition = document::ParameterId::fromRaw(32);
constexpr auto kSecondPosition = document::ParameterId::fromRaw(33);
constexpr auto kFirstOpacity = document::ParameterId::fromRaw(34);
constexpr auto kSecondOpacity = document::ParameterId::fromRaw(35);
constexpr auto kTextSize = document::ParameterId::fromRaw(36);
constexpr auto kTextColor = document::ParameterId::fromRaw(37);
constexpr auto kFirstAnchor = document::ParameterId::fromRaw(38);
constexpr auto kSecondAnchor = document::ParameterId::fromRaw(39);
constexpr auto kFirstScale = document::ParameterId::fromRaw(40);
constexpr auto kSecondScale = document::ParameterId::fromRaw(41);
constexpr auto kFirstRotation = document::ParameterId::fromRaw(42);
constexpr auto kSecondRotation = document::ParameterId::fromRaw(43);
// ADAPTED (blend modes): the Layer Output schema now also requires a blendMode binding.
constexpr auto kFirstBlendMode = document::ParameterId::fromRaw(44);
constexpr auto kSecondBlendMode = document::ParameterId::fromRaw(45);
// Above every id the fixtures below allocate by hand or in bulk, so a Solid's required dimensions
// and a Text's required layout operands can never collide with one of them.
constexpr auto kFirstWidth = document::ParameterId::fromRaw(200);
constexpr auto kFirstHeight = document::ParameterId::fromRaw(201);
constexpr auto kSecondWidth = document::ParameterId::fromRaw(202);
constexpr auto kSecondHeight = document::ParameterId::fromRaw(203);
constexpr auto kTextAlignment = document::ParameterId::fromRaw(204);
constexpr auto kTextLineHeight = document::ParameterId::fromRaw(205);
constexpr auto kTextLetterSpacing = document::ParameterId::fromRaw(206);
constexpr auto kTextFont = document::ParameterId::fromRaw(207);
constexpr auto kFirstSourceEdge = document::EdgeId::fromRaw(40);
constexpr auto kFirstStackEdge = document::EdgeId::fromRaw(41);
constexpr auto kSecondSourceEdge = document::EdgeId::fromRaw(42);
constexpr auto kSecondStackEdge = document::EdgeId::fromRaw(43);
constexpr auto kOutputEdge = document::EdgeId::fromRaw(44);
constexpr auto kFirstLayer = document::LayerId::fromRaw(60);
constexpr auto kSecondLayer = document::LayerId::fromRaw(61);
constexpr auto kFirstSlot = document::LayerSlotId::fromRaw(70);
constexpr auto kSecondSlot = document::LayerSlotId::fromRaw(71);

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::logic_error(std::string(message));
    }
}

template <typename Value>
[[nodiscard]] Value requireValue(std::optional<Value> value, const std::string_view message) {
    if (!value.has_value()) {
        throw std::logic_error(std::string(message));
    }
    return *value;
}

struct ProjectOptions final {
    bool secondLayer = true;
    bool reverseInsertion = false;
    bool omitFirstSourceEdge = false;
    std::string firstSourcePort = std::string(document::kSolidSourceOutputPort);
    document::CompositionFormat format;
};

[[nodiscard]] ProjectOptions singleLayerOptions() {
    ProjectOptions options;
    options.secondLayer = false;
    return options;
}

[[nodiscard]] document::Project makeProject(ProjectOptions options = {}) {
    using namespace document;
    CanonicalGraph graph(kStackNode);
    std::vector<NodeRecord> nodes{
        {kFirstSolidNode,
         std::string(kSolidSourceNodeType),
         {{std::string(kSolidColorParameterRole), kFirstColor},
          {std::string(kSolidWidthParameterRole), kFirstWidth},
          {std::string(kSolidHeightParameterRole), kFirstHeight}},
         kSolidSourceNodeSchemaVersion},
        {kFirstLayerNode,
         std::string(kLayerOutputNodeType),
         {{std::string(kPositionParameterRole), kFirstPosition},
          {std::string(kAnchorParameterRole), kFirstAnchor},
          {std::string(kScaleParameterRole), kFirstScale},
          {std::string(kRotationParameterRole), kFirstRotation},
          {std::string(kOpacityParameterRole), kFirstOpacity},
          {std::string(kBlendModeParameterRole), kFirstBlendMode}},
         kLayerOutputNodeSchemaVersion},
        {kStackNode, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion},
        {kOutputNode,
         std::string(kCompositionOutputNodeType),
         {},
         kCompositionOutputNodeSchemaVersion},
    };
    if (options.secondLayer) {
        nodes.push_back({kSecondSolidNode,
                         std::string(kSolidSourceNodeType),
                         {{std::string(kSolidColorParameterRole), kSecondColor},
                          {std::string(kSolidWidthParameterRole), kSecondWidth},
                          {std::string(kSolidHeightParameterRole), kSecondHeight}},
                         kSolidSourceNodeSchemaVersion});
        nodes.push_back({kSecondLayerNode,
                         std::string(kLayerOutputNodeType),
                         {{std::string(kPositionParameterRole), kSecondPosition},
                          {std::string(kAnchorParameterRole), kSecondAnchor},
                          {std::string(kScaleParameterRole), kSecondScale},
                          {std::string(kRotationParameterRole), kSecondRotation},
                          {std::string(kOpacityParameterRole), kSecondOpacity},
                          {std::string(kBlendModeParameterRole), kSecondBlendMode}},
                         kLayerOutputNodeSchemaVersion});
    }
    if (options.reverseInsertion) {
        std::ranges::reverse(nodes);
    }
    for (auto& node : nodes) {
        require(graph.addNode(std::move(node)), "fixture node must be accepted");
    }

    require(graph.addLayerOutput(
                {kFirstLayerNode, kFirstLayer, "First", std::string(kLayerOutputOutputPort)}),
            "first boundary must be accepted");
    require(graph.layerStack().append({kFirstSlot, kFirstLayer}),
            "first stack slot must be accepted");
    if (options.secondLayer) {
        require(graph.addLayerOutput({kSecondLayerNode, kSecondLayer, "Second",
                                      std::string(kLayerOutputOutputPort)}),
                "second boundary must be accepted");
        require(graph.layerStack().append({kSecondSlot, kSecondLayer}),
                "second stack slot must be accepted");
    }

    std::vector<EdgeRecord> edges;
    if (!options.omitFirstSourceEdge) {
        edges.push_back({kFirstSourceEdge,
                         {kFirstSolidNode, std::move(options.firstSourcePort)},
                         NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}});
    }
    edges.push_back(
        {kFirstStackEdge,
         {kFirstLayerNode, std::string(kLayerOutputOutputPort)},
         LayerStackInputRef{kStackNode, kFirstSlot, std::string(kLayerStackContentInputRole)}});
    if (options.secondLayer) {
        edges.push_back(
            {kSecondSourceEdge,
             {kSecondSolidNode, std::string(kSolidSourceOutputPort)},
             NodeInputRef{kSecondLayerNode, std::string(kLayerOutputContentInputPort)}});
        edges.push_back({kSecondStackEdge,
                         {kSecondLayerNode, std::string(kLayerOutputOutputPort)},
                         LayerStackInputRef{kStackNode, kSecondSlot,
                                            std::string(kLayerStackContentInputRole)}});
    }
    edges.push_back({kOutputEdge,
                     {kStackNode, std::string(kLayerStackOutputPort)},
                     NodeInputRef{kOutputNode, std::string(kCompositionOutputInputPort)}});
    if (options.reverseInsertion) {
        std::ranges::reverse(edges);
    }
    for (auto& edge : edges) {
        require(graph.addEdge(std::move(edge)), "fixture edge must be accepted");
    }
    graph.setCompositionOutput({kOutputNode, std::string(kCompositionOutputOutputPort)});

    Composition composition(kCompositionId, "Main", core::RationalTime::fromInteger(5),
                            std::move(graph), options.format);
    require(
        composition.parameters().insert({kFirstColor, std::string(kSolidColorParameterSchemaKey),
                                         ConstantValueSource{core::Color4d{1.5, 0.25, 0.5, 0.75}}}),
        "first color must be accepted");
    const auto insertSolidDimensions = [&composition](const ParameterId width,
                                                      const ParameterId height) {
        require(composition.parameters().insert(
                    {width, std::string(kSolidWidthParameterSchemaKey),
                     ConstantValueSource{static_cast<double>(composition.format().width())}}),
                "solid width must be accepted");
        require(composition.parameters().insert(
                    {height, std::string(kSolidHeightParameterSchemaKey),
                     ConstantValueSource{static_cast<double>(composition.format().height())}}),
                "solid height must be accepted");
    };
    insertSolidDimensions(kFirstWidth, kFirstHeight);
    require(
        composition.parameters().insert({kFirstPosition, std::string(kPositionParameterSchemaKey),
                                         ConstantValueSource{Vec2d{120.0, 80.0}}}),
        "first position must be accepted");
    require(composition.parameters().insert(
                {kFirstOpacity, std::string(kOpacityParameterSchemaKey), ConstantValueSource{0.8}}),
            "first opacity must be accepted");
    // The identity transform and Normal blending: every fixture here is about topology, parameter
    // sources, and diagnostics, so the three transform breadth parameters and the blend mode stay
    // at their schema defaults unless a case deliberately rewrites one.
    const auto insertIdentityTransform = [&composition](const ParameterId anchor,
                                                        const ParameterId scale,
                                                        const ParameterId rotation,
                                                        const ParameterId blendMode) {
        require(composition.parameters().insert({anchor, std::string(kAnchorParameterSchemaKey),
                                                 ConstantValueSource{kDefaultAnchor}}),
                "anchor must be accepted");
        require(composition.parameters().insert({scale, std::string(kScaleParameterSchemaKey),
                                                 ConstantValueSource{kDefaultScale}}),
                "scale must be accepted");
        require(composition.parameters().insert({rotation, std::string(kRotationParameterSchemaKey),
                                                 ConstantValueSource{kDefaultRotationDegrees}}),
                "rotation must be accepted");
        require(
            composition.parameters().insert({blendMode, std::string(kBlendModeParameterSchemaKey),
                                             ConstantValueSource{kDefaultBlendModeValue}}),
            "blend mode must be accepted");
    };
    insertIdentityTransform(kFirstAnchor, kFirstScale, kFirstRotation, kFirstBlendMode);
    if (options.secondLayer) {
        require(composition.parameters().insert(
                    {kSecondColor, std::string(kSolidColorParameterSchemaKey),
                     ConstantValueSource{core::Color4d{0.1, 0.2, 0.3, 1.0}}}),
                "second color must be accepted");
        require(composition.parameters().insert({kSecondPosition,
                                                 std::string(kPositionParameterSchemaKey),
                                                 ConstantValueSource{Vec2d{-20.0, 30.0}}}),
                "second position must be accepted");
        require(composition.parameters().insert({kSecondOpacity,
                                                 std::string(kOpacityParameterSchemaKey),
                                                 ConstantValueSource{0.6}}),
                "second opacity must be accepted");
        insertSolidDimensions(kSecondWidth, kSecondHeight);
        insertIdentityTransform(kSecondAnchor, kSecondScale, kSecondRotation, kSecondBlendMode);
    }

    Project project(kProjectId, "Project");
    require(project.addComposition(std::move(composition)), "composition must be accepted");
    require(project.validate().ok(), "fixture must be valid document truth");
    return project;
}

void populateRegistry(runtime::NodeDefinitionRegistry& registry) {
    require(runtime::registerBuiltInNodeDefinitions(registry),
            "built-in registry fixture must initialize");
}

[[nodiscard]] runtime::NodeDefinition unsupportedColorDefinition() {
    return {{"example.unsupported-color", 1},
            runtime::NodeLoweringKind::Unsupported,
            {},
            {{"image", runtime::SocketValueKind::Image}},
            {{"value", "example.color", runtime::ParameterValueKind::Color4d, true}},
            std::nullopt};
}

[[nodiscard]] runtime::NodeDefinition customSolidDefinition() {
    auto definition = *document::builtInNodeDefinitions().find(
        document::kSolidSourceNodeType, document::kSolidSourceNodeSchemaVersion);
    definition.key = {"example.solid", 17};
    return definition;
}

[[nodiscard]] runtime::NodeDefinition bulkUnsupportedDefinition(const std::size_t index) {
    return {{"example.bulk." + std::to_string(index), 1},
            runtime::NodeLoweringKind::Unsupported,
            {},
            {{"image", runtime::SocketValueKind::Image}},
            {},
            std::nullopt};
}

// Retypes the fixture's first source node into a CURRENT Text source. Text v2 requires alignment,
// line height and letter spacing, and carries none of the Solid dimensions the node was built with.
void retypeFirstSourceToText(document::Project& project, const std::string& content,
                             const double size, const core::Color4d color) {
    using namespace document;
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "text fixture composition must exist");
    auto* node = composition->graph().findNode(kFirstSolidNode);
    require(node != nullptr, "text fixture source node must exist");
    node->typeId = std::string(kTextSourceNodeType);
    node->schemaVersion = kTextSourceNodeSchemaVersion;
    node->parameters = {{std::string(kTextParameterRole), kFirstColor},
                        {std::string(kTextSizeParameterRole), kTextSize},
                        {std::string(kTextColorParameterRole), kTextColor},
                        {std::string(kTextAlignmentParameterRole), kTextAlignment},
                        {std::string(kTextLineHeightParameterRole), kTextLineHeight},
                        {std::string(kTextLetterSpacingParameterRole), kTextLetterSpacing},
                        {std::string(kTextFontParameterRole), kTextFont}};
    auto& parameters = composition->parameters();
    require(parameters.erase(kFirstColor) && parameters.erase(kFirstWidth) &&
                parameters.erase(kFirstHeight) &&
                parameters.insert({kFirstColor, std::string(kTextParameterSchemaKey),
                                   ConstantValueSource{content}}) &&
                parameters.insert({kTextSize, std::string(kTextSizeParameterSchemaKey),
                                   ConstantValueSource{size}}) &&
                parameters.insert({kTextColor, std::string(kTextColorParameterSchemaKey),
                                   ConstantValueSource{color}}) &&
                parameters.insert({kTextAlignment, std::string(kTextAlignmentParameterSchemaKey),
                                   ConstantValueSource{std::int64_t{0}}}) &&
                parameters.insert({kTextLineHeight, std::string(kTextLineHeightParameterSchemaKey),
                                   ConstantValueSource{1.0}}) &&
                parameters.insert({kTextLetterSpacing,
                                   std::string(kTextLetterSpacingParameterSchemaKey),
                                   ConstantValueSource{0.0}}) &&
                parameters.insert({kTextFont, std::string(kTextFontParameterSchemaKey),
                                   ConstantValueSource{kDefaultTextFontValue}}),
            "text fixture parameters must be accepted");
}

// Task S7: a driver binding names a value node's output, so a driven fixture needs a value node to
// name. Adds one literal Value node of `typeId` and points `target`'s source at its single output.
void attachValueDriver(document::Project& project, const document::NodeId nodeId,
                       const document::ParameterId valueParameterId, const std::string_view typeId,
                       const std::string_view valueSchemaKey, document::ParameterValue defaultValue,
                       const document::ParameterId target) {
    using namespace document;
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "driver fixture composition must exist");
    require(composition->parameters().insert({valueParameterId, std::string(valueSchemaKey),
                                              ConstantValueSource{std::move(defaultValue)}}),
            "driver fixture value parameter must be accepted");
    require(composition->graph().addNode({nodeId,
                                          std::string(typeId),
                                          {{std::string(kValueParameterRole), valueParameterId}},
                                          kValueNodeSchemaVersion}),
            "driver fixture value node must be accepted");
    require(composition->parameters().setSource(
                target, DriverBindingSource{nodeId, std::string(kValuePortName)}),
            "driver fixture binding must be accepted");
    require(project.validate().ok(), "driver fixture must remain valid document truth");
}

[[nodiscard]] runtime::SnapshotCompileResult
compile(document::Project project, runtime::NodeDefinitionRegistry& registry,
        std::vector<runtime::SnapshotParameterOverride> parameterOverrides = {}) {
    document::Document document(std::move(project));
    runtime::SnapshotCompiler compiler(registry);
    return compiler.compile({document.snapshot(), kCompositionId, std::move(parameterOverrides)},
                            runtime::CancellationToken{});
}

[[nodiscard]] bool hasDiagnostic(const runtime::SnapshotCompileResult& result,
                                 const runtime::CompileDiagnosticCode code,
                                 const document::NodeId nodeId = {}) {
    return std::ranges::any_of(result.diagnostics, [&](const auto& diagnostic) {
        return diagnostic.code == code &&
               (!nodeId.isValid() || diagnostic.subject.nodeId == nodeId);
    });
}

// Task S7: a Time -> Math -> opacity chain, the smallest graph that proves a driver is resolved per
// frame rather than once. Returns the project plus the parameter the chain drives.
struct DrivenChain final {
    document::Project project;
    document::ParameterId drivenParameterId;
};

[[nodiscard]] DrivenChain makeTimeDrivenOpacityChain(const double factor = 0.5) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "chain fixture composition must exist");

    constexpr auto timeNode = NodeId::fromRaw(14);
    constexpr auto mathNode = NodeId::fromRaw(15);
    require(composition->graph().addNode(
                {timeNode, std::string(kTimeValueNodeType), {}, kValueNodeSchemaVersion}),
            "chain fixture Time node must be accepted");

    // Every declared parameter needs a binding, including the operands this operation does not
    // read: a node's parameter set is part of its registered shape, and the compiler lowers only
    // the ones the live operation actually reaches.
    std::vector<ParameterBinding> mathBindings;
    auto nextParameterId = std::uint64_t{50};
    const auto bindParameter = [&](const std::string_view role, const std::string_view schemaKey,
                                   ParameterValue value) {
        const auto id = ParameterId::fromRaw(nextParameterId++);
        require(composition->parameters().insert(
                    {id, std::string(schemaKey), ConstantValueSource{std::move(value)}}),
                "chain fixture parameter must be accepted");
        mathBindings.push_back({std::string(role), id});
        return id;
    };
    const auto firstOperand =
        bindParameter(kFirstOperandPortName, kScalarOperandParameterSchemaKey, 0.0);
    bindParameter(kSecondOperandPortName, kScalarOperandParameterSchemaKey, factor);
    for (std::size_t index = 2; index < kScalarOperandPortNames.size(); ++index) {
        bindParameter(kScalarOperandPortNames[index], kScalarOperandParameterSchemaKey, 0.0);
    }
    bindParameter(kOperationParameterRole, kScalarOperationParameterSchemaKey,
                  scalarOperationStoredValue(core::primitives::ScalarPrimitive::Multiply));
    bindParameter(kClampResultParameterRole, kClampResultParameterSchemaKey, false);
    require(composition->graph().addNode({mathNode, std::string(kScalarMathNodeType),
                                          std::move(mathBindings), kValueNodeSchemaVersion}),
            "chain fixture Math node must be accepted");

    require(composition->parameters().setSource(
                firstOperand, DriverBindingSource{timeNode, std::string(kTimeSecondsPortName)}),
            "chain fixture Time driver must be accepted");
    require(composition->parameters().setSource(
                kFirstOpacity, DriverBindingSource{mathNode, std::string(kResultPortName)}),
            "chain fixture opacity driver must be accepted");
    require(project.validate().ok(), "chain fixture must be valid document truth");
    return {std::move(project), kFirstOpacity};
}

void testValueGraphDriverResolution(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto chain = makeTimeDrivenOpacityChain();
    const auto result = compile(std::move(chain.project), registry);
    expectations.expect(result.status == runtime::SnapshotCompileStatus::Compiled && result.plan,
                        "a Time to Math to opacity chain compiles");
    if (!result.plan) {
        return;
    }
    const auto& plan = *result.plan;
    // Two operations and three outputs: Time's two, the Math result, and nothing else. Reachability
    // pruning is what keeps it there -- no unreferenced value node is compiled.
    expectations.expect(plan.valueOperations().size() == 2 && plan.valueOutputCount() == 3,
                        "the chain compiles to exactly the two value nodes it contains");

    const auto layer = std::ranges::find_if(plan.operations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledLayerOutput>(operation);
    });
    expectations.expect(layer != plan.operations().end(),
                        "the chain's plan contains its Layer Output");
    if (layer == plan.operations().end()) {
        return;
    }
    const auto& opacity = std::get<runtime::CompiledLayerOutput>(*layer).opacity;
    const auto* driven = std::get_if<runtime::ValueOutputIndex>(&opacity.source);
    expectations.expect(driven != nullptr, "the driven opacity resolves to a value-graph output");
    if (driven == nullptr) {
        return;
    }

    // The same plan, sampled at three frames: 24fps, so second 0, 1 and 2 are frames 0, 24 and 48
    // -- and the opacity the evaluator reads is half the elapsed seconds at each of them.
    const std::array<std::pair<std::int64_t, double>, 3> expected{std::pair{std::int64_t{0}, 0.0},
                                                                  std::pair{std::int64_t{1}, 0.5},
                                                                  std::pair{std::int64_t{2}, 1.0}};
    for (const auto& [second, value] : expected) {
        const auto evaluation = runtime::evaluateValueGraph(
            plan.valueOperations(), plan.valueOutputCount(),
            core::RationalTime::fromInteger(second), plan.format().frameRate());
        const auto* resolved = driven->value() < evaluation.outputs.size()
                                   ? std::get_if<double>(&evaluation.outputs[driven->value()])
                                   : nullptr;
        expectations.expect(evaluation.diagnostics.empty() && resolved != nullptr &&
                                *resolved == value,
                            "the driven opacity is re-resolved at each frame");
    }
}

// Task UTIL-1's readouts. The three that describe the COMPOSITION are lowered to constants, so a
// plan carries the settings it was compiled from and costs nothing per frame to read them back; the
// one that describes the FRAME is a kernel, and is re-resolved at every frame like a Time node.
void testCompositionReadoutsLowerToConstants(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto project = makeProject(singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "readout fixture composition must exist");
    const auto rate = composition->format().frameRate();
    constexpr auto rateNode = NodeId::fromRaw(14);
    require(composition->graph().addNode(
                {rateNode, std::string(kFrameRateNodeType), {}, kValueNodeSchemaVersion}),
            "readout fixture Frame Rate node must be accepted");
    require(composition->parameters().setSource(
                kFirstOpacity, DriverBindingSource{rateNode, std::string(kResultPortName)}),
            "readout fixture opacity driver must be accepted");
    require(project.validate().ok(), "readout fixture must be valid document truth");

    const auto result = compile(std::move(project), registry);
    expectations.expect(result.status == runtime::SnapshotCompileStatus::Compiled && result.plan,
                        "a composition readout compiles");
    if (!result.plan) {
        return;
    }
    const auto& plan = *result.plan;
    expectations.expect(plan.valueOperations().size() == 1 && plan.valueOutputCount() == 1,
                        "and lowers to exactly one value operation with no kernel behind it");
    if (plan.valueOperations().empty()) {
        return;
    }
    const auto* passthrough =
        std::get_if<runtime::CompiledValuePassthrough>(&plan.valueOperations().front().kernel);
    const auto* constant = passthrough == nullptr
                               ? nullptr
                               : std::get_if<runtime::CompiledValue>(&passthrough->value.source);
    const auto* baked = constant == nullptr ? nullptr : std::get_if<double>(constant);
    const double expected =
        static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator());
    expectations.expect(baked != nullptr && *baked == expected,
                        "the composition's own frame rate is baked into the plan as a constant");
}

void testFrameNumberReadoutIsResolvedPerFrame(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto project = makeProject(singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "readout fixture composition must exist");
    constexpr auto frameNode = NodeId::fromRaw(14);
    require(composition->graph().addNode(
                {frameNode, std::string(kFrameNumberNodeType), {}, kValueNodeSchemaVersion}),
            "readout fixture Frame Number node must be accepted");
    require(composition->parameters().setSource(
                kFirstOpacity, DriverBindingSource{frameNode, std::string(kResultPortName)}),
            "readout fixture opacity driver must be accepted");
    require(project.validate().ok(), "readout fixture must be valid document truth");

    const auto result = compile(std::move(project), registry);
    expectations.expect(result.status == runtime::SnapshotCompileStatus::Compiled && result.plan,
                        "a Frame Number readout compiles");
    if (!result.plan) {
        return;
    }
    const auto& plan = *result.plan;
    // Two operations: the readout, and the explicit Integer-to-Scalar promotion the opacity socket
    // needs. A widening that appears in the plan is a widening that can be diagnosed.
    expectations.expect(plan.valueOperations().size() == 2 && plan.valueOutputCount() == 2,
                        "through an explicit promotion into the Scalar the opacity socket carries");
    const auto layer = std::ranges::find_if(plan.operations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledLayerOutput>(operation);
    });
    if (layer == plan.operations().end()) {
        return;
    }
    const auto* driven = std::get_if<runtime::ValueOutputIndex>(
        &std::get<runtime::CompiledLayerOutput>(*layer).opacity.source);
    expectations.expect(driven != nullptr, "and the opacity resolves to a value-graph output");
    if (driven == nullptr) {
        return;
    }
    // 24fps, so second 0, 1 and 2 are frames 0, 24 and 48 -- the same numbers a Time node's own
    // frame output gives, because both come from valueGraphFrameIndex().
    const std::array<std::pair<std::int64_t, double>, 3> expected{std::pair{std::int64_t{0}, 0.0},
                                                                  std::pair{std::int64_t{1}, 24.0},
                                                                  std::pair{std::int64_t{2}, 48.0}};
    for (const auto& [second, value] : expected) {
        const auto evaluation = runtime::evaluateValueGraph(
            plan.valueOperations(), plan.valueOutputCount(),
            core::RationalTime::fromInteger(second), plan.format().frameRate());
        const auto* resolved = driven->value() < evaluation.outputs.size()
                                   ? std::get_if<double>(&evaluation.outputs[driven->value()])
                                   : nullptr;
        expectations.expect(evaluation.diagnostics.empty() && resolved != nullptr &&
                                *resolved == value,
                            "and the frame number is re-resolved at each frame");
    }
}

void testValueGraphCycleRefusal(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    // Two Reroutes pointing at each other through their parameters is the smallest driver cycle
    // there is. It is refused by the document's own acyclic check -- the one the image graph
    // already uses -- rather than by a second rule that could disagree with it.
    auto chain = makeTimeDrivenOpacityChain();
    auto* composition = chain.project.findComposition(kCompositionId);
    require(composition != nullptr, "cycle fixture composition must exist");
    const auto* mathNode = composition->graph().findNode(NodeId::fromRaw(15));
    require(mathNode != nullptr, "cycle fixture Math node must exist");
    const auto operand =
        std::ranges::find(mathNode->parameters, kFirstOperandPortName, &ParameterBinding::role);
    require(operand != mathNode->parameters.end(), "cycle fixture operand must exist");
    require(composition->parameters().setSource(
                operand->parameterId,
                DriverBindingSource{NodeId::fromRaw(15), std::string(kResultPortName)}),
            "a self-driving operand is well-formed as a reference");
    const auto validation = chain.project.validate();
    expectations.expect(std::ranges::any_of(validation.issues(),
                                            [](const auto& issue) {
                                                return issue.code == ValidationCode::GraphCycle;
                                            }),
                        "a parameter driven by its own node is refused as a graph cycle");
}

void testLayerBoundsReadoutAndFeedbackRefusal(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    constexpr auto boundsNode = NodeId::fromRaw(90);
    constexpr auto boundsEdge = EdgeId::fromRaw(90);
    auto project = makeProject();
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "Layer Bounds fixture composition must exist");
    require(composition->graph().addNode(
                {boundsNode, std::string(kLayerBoundsNodeType), {}, kValueNodeSchemaVersion}),
            "Layer Bounds fixture node must be accepted");
    require(composition->graph().addEdge(
                {boundsEdge,
                 {kFirstLayerNode, std::string(kLayerOutputOutputPort)},
                 NodeInputRef{boundsNode, std::string(kLayerBoundsImagePortName)}}),
            "Layer Bounds fixture image edge must be accepted");
    require(project.validate().ok(), "an unconnected Layer Bounds readout is valid document truth");

    const auto result = compile(std::move(project), registry);
    expectations.expect(result.status == runtime::SnapshotCompileStatus::Compiled && result.plan,
                        "a Layer Bounds node connected to a Layer output compiles");
    if (result.plan) {
        const auto& plan = *result.plan;
        const auto value = std::ranges::find_if(plan.valueOperations(), [&](const auto& operation) {
            return operation.sourceNodeId == boundsNode;
        });
        expectations.expect(
            value != plan.valueOperations().end() &&
                std::holds_alternative<runtime::CompiledBoundsReadout>(value->kernel),
            "the compiler emits a CompiledBoundsReadout kernel");
        const auto layer = std::ranges::find_if(plan.operations(), [](const auto& operation) {
            return std::holds_alternative<runtime::CompiledLayerOutput>(operation);
        });
        expectations.expect(
            value != plan.valueOperations().end() && layer != plan.operations().end() &&
                std::get<runtime::CompiledBoundsReadout>(value->kernel).operationIndex ==
                    runtime::OperationIndex::fromRaw(
                        static_cast<std::size_t>(std::distance(plan.operations().begin(), layer))),
            "the readout is patched to its connected image operation");

        runtime::CpuCompositionEvaluator evaluator;
        const auto evaluated =
            evaluator.evaluate(result.plan,
                               {.time = core::RationalTime::fromInteger(0),
                                .output = plan.output(),
                                .resolution = runtime::CompositionFormatResolution{},
                                .quality = runtime::EvaluationQuality::Reference,
                                .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                .pixelStorageByteLimit = 1U << 28U},
                               runtime::CancellationToken{});
        const auto* size = evaluated.frame() && !evaluated.frame()->valueOutputs().empty()
                               ? std::get_if<Vec2d>(&evaluated.frame()->valueOutputs()[0])
                               : nullptr;
        expectations.expect(
            size != nullptr && *size == Vec2d{1920.0, 1080.0},
            "a compiled 1920x1080 Solid Layer readout reaches frame Properties values");
    }

    auto refusedProject = makeProject();
    auto* refusedComposition = refusedProject.findComposition(kCompositionId);
    require(refusedComposition != nullptr, "feedback fixture composition must exist");
    require(refusedComposition->graph().addNode(
                {boundsNode, std::string(kLayerBoundsNodeType), {}, kValueNodeSchemaVersion}),
            "feedback Layer Bounds node must be accepted");
    require(refusedComposition->graph().addEdge(
                {boundsEdge,
                 {kFirstLayerNode, std::string(kLayerOutputOutputPort)},
                 NodeInputRef{boundsNode, std::string(kLayerBoundsImagePortName)}}),
            "feedback Layer Bounds image edge must be accepted");
    require(refusedComposition->parameters().setSource(
                kSecondPosition,
                DriverBindingSource{boundsNode, std::string(kLayerBoundsOriginPortName)}),
            "feedback Layer Bounds driver must be accepted");
    require(refusedProject.validate().ok(),
            "cross-layer Layer Bounds feedback remains valid document truth before compile");
    const auto refused = compile(std::move(refusedProject), registry);
    expectations.expect(
        refused.status != runtime::SnapshotCompileStatus::Compiled &&
            hasDiagnostic(refused,
                          runtime::CompileDiagnosticCode::BoundsReadoutDrivesImageOperation,
                          kSecondLayerNode),
        "Layer Bounds feedback into an image operation is refused with its destination");
}

void testParentOrder(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    for (const bool empty : {false, true}) {
        auto project = makeProject();
        auto& composition = *project.findComposition(kCompositionId);
        auto& graph = composition.graph();
        graph.findLayer(kFirstLayer)->parent = kSecondLayer;
        if (empty) {
            require(graph.eraseEdge(kSecondSourceEdge), "empty parent source removed");
            graph.findLayer(kSecondLayer)->enabled = false;
            composition.nodeLayout()[kSecondLayerNode].muted = true;
        }
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan && result.diagnostics.empty(),
                            "parent later in stack compiles, including an empty disabled parent");
        if (!result.plan)
            continue;
        const auto operations = result.plan->operations();
        bool found = false;
        for (std::size_t index = 0; index < operations.size(); ++index) {
            if (const auto* child = std::get_if<runtime::CompiledLayerOutput>(&operations[index]);
                child && child->layerId == kFirstLayer) {
                found = child->parent && child->parent->value() < index &&
                        std::get<runtime::CompiledLayerOutput>(operations[child->parent->value()])
                                .layerId == kSecondLayer;
            }
            if (const auto* merge = std::get_if<runtime::CompiledMerge>(&operations[index]);
                merge && merge->entries.size() == 2)
                expectations.expect(merge->entries[0].layerId == kFirstLayer &&
                                        merge->entries[1].layerId == kSecondLayer,
                                    "parent ordering preserves stack order");
        }
        expectations.expect(found, "parent operation precedes child despite later stack position");
    }
}

void testNestedMergeCompilation(Expectations& expectations) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    auto& graph = project.findComposition(kCompositionId)->graph();
    const auto nested = NodeId::fromRaw(900);
    const auto slot = LayerSlotId::fromRaw(900);
    require(
        graph.addNode({nested, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}),
        "nested Merge node");
    require(graph.merge(nested)->append({slot, {}}), "plain image slot");
    require(graph.addEdge({EdgeId::fromRaw(900),
                           {kFirstSolidNode, "image"},
                           LayerStackInputRef{nested, slot, "content"}}),
            "plain source edge");
    require(graph.layerStack().append({LayerSlotId::fromRaw(901), {}}), "nested slot");
    require(graph.addEdge({EdgeId::fromRaw(901),
                           {nested, "image"},
                           LayerStackInputRef{kStackNode, LayerSlotId::fromRaw(901), "content"}}),
            "nested edge");
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto direct = project;
    auto& directGraph = direct.findComposition(kCompositionId)->graph();
    require(directGraph.eraseEdge(kOutputEdge) &&
                directGraph.addEdge(
                    {kOutputEdge, {kFirstLayerNode, "image"}, NodeInputRef{kOutputNode, "image"}}),
            "direct Layer output fixture");
    const auto directResult = compile(std::move(direct), registry);
    expectations.expect(
        directResult.plan &&
            std::holds_alternative<runtime::CompiledMerge>(
                directResult.plan->operations()[directResult.plan->output().value() - 1]),
        "direct image Output is normalized to a full composition image");
    const auto result = compile(std::move(project), registry);
    expectations.expect(result.plan && result.diagnostics.empty(),
                        "nested Merges compile without diagnostics");
    if (!result.plan)
        return;
    const auto operations = result.plan->operations();
    std::size_t merges = 0;
    for (std::size_t index = 0; index < operations.size(); ++index)
        if (const auto* merge = std::get_if<runtime::CompiledMerge>(&operations[index])) {
            ++merges;
            for (const auto& input : merge->entries)
                expectations.expect(input.input.value() < index, "Merge inputs are topological");
        }
    expectations.expect(merges == 2, "both reachable Merges lower");
}

void testRegistryMustBeFrozen(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    const auto result = compile(makeProject(), registry);
    expectations.expect(result.status == runtime::SnapshotCompileStatus::Failed && !result.plan,
                        "compiler rejects a mutable registry");
    expectations.expect(hasDiagnostic(result, runtime::CompileDiagnosticCode::RegistryNotFrozen),
                        "mutable registry failure has a stable diagnostic");

    registry.freeze();
    document::Document document(makeProject());
    runtime::SnapshotCompiler compiler(registry);
    const auto missing = compiler.compile(
        {document.snapshot(), document::CompositionId::fromRaw(999)}, runtime::CancellationToken{});
    expectations.expect(
        missing.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(missing, runtime::CompileDiagnosticCode::CompositionNotFound),
        "missing composition fails with a stable typed diagnostic");
}

void testDeterministicTypedPlan(Expectations& expectations) {
    const auto pixelAspect =
        requireValue(core::PixelAspectRatio::create(4, 3), "pixel-aspect fixture must be valid");
    const auto frameRate =
        requireValue(document::FrameRate::create(30000, 1001), "frame-rate fixture must be valid");
    const auto format =
        requireValue(document::CompositionFormat::create(2048, 858, pixelAspect, frameRate),
                     "non-square project format must be valid");

    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    const auto first = compile(makeProject({.format = format}), registry);
    const auto second =
        compile(makeProject({.reverseInsertion = true, .format = format}), registry);

    expectations.expect(first.status == runtime::SnapshotCompileStatus::Compiled && first.plan,
                        "supported reachable graph compiles");
    expectations.expect(second.status == runtime::SnapshotCompileStatus::Compiled && second.plan,
                        "different insertion order compiles");
    if (!first.plan || !second.plan) {
        return;
    }
    expectations.expect(*first.plan == *second.plan,
                        "plan is independent of document insertion and hash order");
    expectations.expect(first.plan->sourceRevision() == document::Revision{} &&
                            first.plan->projectId() == kProjectId &&
                            first.plan->compositionId() == kCompositionId &&
                            first.plan->format() == format &&
                            first.plan->planSemanticsVersion() ==
                                runtime::kCompiledCompositionPlanSemanticsVersion &&
                            first.plan->animationSamplingSemanticsVersion() ==
                                runtime::kAnimationSamplingSemanticsVersion,
                        "revision, identity, and exact composition format carry through");
    expectations.expect(first.plan->operations().size() == 6 &&
                            first.plan->output() == runtime::OperationIndex::fromRaw(5),
                        "plan contains one topological operation per reachable node");

    const auto* solid = std::get_if<runtime::CompiledSolid>(&first.plan->operations()[0]);
    const auto* firstLayer =
        std::get_if<runtime::CompiledLayerOutput>(&first.plan->operations()[1]);
    const auto* stack = std::get_if<runtime::CompiledMerge>(&first.plan->operations()[4]);
    const auto* output =
        std::get_if<runtime::CompiledCompositionOutput>(&first.plan->operations()[5]);
    const auto* firstPosition = firstLayer == nullptr
                                    ? nullptr
                                    : std::get_if<document::Vec2d>(&firstLayer->position.source);
    const auto* firstOpacity =
        firstLayer == nullptr ? nullptr : std::get_if<double>(&firstLayer->opacity.source);
    // Task S5: a solid's colour is a typed operand, so its constant travels inside the operand's
    // own source variant rather than as a bare field.
    const auto* solidColor =
        solid == nullptr ? nullptr : std::get_if<core::Color4d>(&solid->color.source);
    expectations.expect(solid != nullptr && solid->sourceNodeId == kFirstSolidNode &&
                            solid->color.id == kFirstColor && solidColor != nullptr &&
                            *solidColor == core::Color4d{1.5, 0.25, 0.5, 0.75},
                        "solid preserves straight HDR authoring color and typed identity");
    expectations.expect(firstLayer != nullptr && firstLayer->input.value() == 0 &&
                            firstLayer->layerId == kFirstLayer &&
                            firstLayer->position.id == kFirstPosition && firstPosition != nullptr &&
                            *firstPosition == document::Vec2d{120.0, 80.0} &&
                            firstLayer->opacity.id == kFirstOpacity && firstOpacity != nullptr &&
                            *firstOpacity == 0.8,
                        "Layer Output preserves typed input and static properties");
    // The blend mode lowers to a resolved enumerator plus its own parameter identity, never to a
    // curve index: the schema declares it non-animatable.
    expectations.expect(firstLayer != nullptr &&
                            firstLayer->blendModeParameterId == kFirstBlendMode,
                        "Layer Output carries the blend mode's own parameter identity");
    expectations.expect(firstLayer != nullptr &&
                            firstLayer->blendMode == bloom::core::kDefaultBlendMode,
                        "a layer with the schema default lowers to Normal");
    expectations.expect(
        stack != nullptr && stack->entries.size() == 2 &&
            stack->entries[0] == runtime::CompiledMergeInput{kFirstSlot, kFirstLayer,
                                                             runtime::OperationIndex::fromRaw(1)} &&
            stack->entries[1] == runtime::CompiledMergeInput{kSecondSlot, kSecondLayer,
                                                             runtime::OperationIndex::fromRaw(3)},
        "Layer Stack preserves explicit top-to-bottom stable slot order");
    expectations.expect(output != nullptr && output->input.value() == 4,
                        "Composition Output names the final dependency explicitly");

    const auto single = compile(makeProject(singleLayerOptions()), registry);
    expectations.expect(single.status == runtime::SnapshotCompileStatus::Compiled && single.plan &&
                            single.plan->operations().size() == 4 &&
                            single.plan->output() == runtime::OperationIndex::fromRaw(3),
                        "one-solid topology lowers to the minimal four-operation plan");
    if (single.plan) {
        const auto* singleStack =
            std::get_if<runtime::CompiledMerge>(&single.plan->operations()[2]);
        expectations.expect(singleStack != nullptr && singleStack->entries.size() == 1 &&
                                singleStack->entries.front().input ==
                                    runtime::OperationIndex::fromRaw(1),
                            "one-solid stack dependency is exact and topologically prior");
    }

    document::Document revisedDocument(makeProject({.format = format}));
    const auto base = revisedDocument.snapshot();
    auto publication = revisedDocument.commit(base.revision(), revisedDocument.draft(base));
    require(publication.committed(), "revision fixture must publish");
    const auto publicationSnapshot =
        requireValue(publication.snapshot, "revision fixture must publish a snapshot");
    runtime::SnapshotCompiler compiler(registry);
    const auto revised =
        compiler.compile({publicationSnapshot, kCompositionId}, runtime::CancellationToken{});
    expectations.expect(revised.plan &&
                            revised.plan->sourceRevision() == document::Revision::fromRaw(1),
                        "compiler carries the exact published source revision");
}

void testCustomSolidLoweringRemainsSupported(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    require(registry.registerDefinition(customSolidDefinition()) ==
                runtime::NodeRegistrationStatus::Registered,
            "custom Solid definition must register");
    registry.freeze();

    auto project = makeProject(singleLayerOptions());
    auto* node = project.findComposition(kCompositionId)->graph().findNode(kFirstSolidNode);
    node->typeId = "example.solid";
    node->schemaVersion = 17;
    require(project.validate().ok(), "custom Solid remains valid extension project truth");
    const auto result = compile(std::move(project), registry);
    expectations.expect(
        result.status == runtime::SnapshotCompileStatus::Compiled && result.plan &&
            std::holds_alternative<runtime::CompiledSolid>(result.plan->operations().front()),
        "custom Solid type lowers through the supported closed Solid operation");
}

void testReachabilityAndUnsupportedNodes(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    auto unreachable = makeProject();
    auto* composition = unreachable.findComposition(kCompositionId);
    require(composition != nullptr &&
                composition->graph().addNode(
                    {document::NodeId::fromRaw(99), "example.unavailable", {}, 1}),
            "unreachable optional node must be accepted");
    require(unreachable.validate().ok(), "unreachable optional node remains valid project truth");
    const auto unreachableResult = compile(std::move(unreachable), registry);
    expectations.expect(unreachableResult.status == runtime::SnapshotCompileStatus::Compiled &&
                            unreachableResult.diagnostics.empty(),
                        "unreachable unknown nodes do not invalidate the output path");

    auto unknown = makeProject(singleLayerOptions());
    unknown.findComposition(kCompositionId)->graph().findNode(kFirstSolidNode)->typeId =
        "example.unavailable";
    require(unknown.validate().ok(), "unknown reachable type is valid extension document truth");
    const auto unknownResult = compile(std::move(unknown), registry);
    expectations.expect(unknownResult.status == runtime::SnapshotCompileStatus::Unsupported &&
                            !unknownResult.plan &&
                            hasDiagnostic(unknownResult,
                                          runtime::CompileDiagnosticCode::UnknownNodeType,
                                          kFirstSolidNode),
                        "reachable unknown node blocks only compilation with typed subject");

    auto version = makeProject(singleLayerOptions());
    version.findComposition(kCompositionId)->graph().findNode(kFirstSolidNode)->schemaVersion = 99;
    require(version.validate().ok(), "future node version remains valid document truth");
    const auto versionResult = compile(std::move(version), registry);
    expectations.expect(versionResult.status == runtime::SnapshotCompileStatus::Unsupported &&
                            hasDiagnostic(versionResult,
                                          runtime::CompileDiagnosticCode::UnsupportedNodeVersion,
                                          kFirstSolidNode),
                        "known type with unavailable version is distinguished from unknown type");

    // ADAPTED (task S3): this case previously asserted that a recognized text source reports
    // UnsupportedNode, because no portable CPU glyph rasterizer existed. Text now has its own
    // lowering, so what is pinned here is the lowered operation -- including that the three
    // parameter identities travel with it for diagnostics -- and the ABSENCE of the diagnostic this
    // case used to require. UnsupportedNode itself stays covered by
    // unsupportedColorDefinition()'s own case above.
    auto text = makeProject(singleLayerOptions());
    const auto textColorValue = core::Color4d{0.25, 0.5, 0.75, 1.0};
    retypeFirstSourceToText(text, "Title", 48.0, textColorValue);
    require(text.validate().ok(), "recognized Text fixture must remain valid");
    const auto textResult = compile(std::move(text), registry);
    expectations.expect(textResult.status == runtime::SnapshotCompileStatus::Compiled &&
                            textResult.plan && textResult.diagnostics.empty(),
                        "a recognized text source compiles with no diagnostics at all");
    const auto* compiledText =
        textResult.plan && !textResult.plan->operations().empty()
            ? std::get_if<runtime::CompiledText>(&textResult.plan->operations().front())
            : nullptr;
    const auto* textSize =
        compiledText == nullptr ? nullptr : std::get_if<double>(&compiledText->size.source);
    const auto* textColor =
        compiledText == nullptr ? nullptr : std::get_if<core::Color4d>(&compiledText->color.source);
    expectations.expect(compiledText != nullptr && compiledText->sourceNodeId == kFirstSolidNode &&
                            compiledText->content == "Title" && textSize != nullptr &&
                            *textSize == 48.0 && textColor != nullptr &&
                            *textColor == textColorValue &&
                            compiledText->face == render::EmbeddedFace::DejaVuSans,
                        "the lowered text operation carries the exact authored content, size, and "
                        "color and default face");
    expectations.expect(
        compiledText != nullptr && compiledText->contentParameterId == kFirstColor &&
            compiledText->size.id == kTextSize && compiledText->color.id == kTextColor,
        "and each parameter identity, so an evaluation diagnostic can name the "
        "exact parameter that failed");

    auto interText = makeProject(singleLayerOptions());
    retypeFirstSourceToText(interText, "Title", 48.0, textColorValue);
    auto* interComposition = interText.findComposition(kCompositionId);
    expectations.expect(
        interComposition != nullptr &&
            interComposition->parameters().setSource(
                kTextFont, document::ConstantValueSource{document::kTextFontInterSemiBold}),
        "the text font selector accepts Inter SemiBold");
    const auto interResult = compile(std::move(interText), registry);
    const auto* interCompiledText =
        interResult.plan && !interResult.plan->operations().empty()
            ? std::get_if<runtime::CompiledText>(&interResult.plan->operations().front())
            : nullptr;
    expectations.expect(interResult.status == runtime::SnapshotCompileStatus::Compiled &&
                            interCompiledText != nullptr &&
                            interCompiledText->face == render::EmbeddedFace::InterSemiBold,
                        "lowering passes the selected Inter SemiBold face into CompiledText");

    auto legacyText = makeProject(singleLayerOptions());
    retypeFirstSourceToText(legacyText, "Title", 48.0, textColorValue);
    auto* legacyComposition = legacyText.findComposition(kCompositionId);
    auto* legacyNode = legacyComposition == nullptr
                           ? nullptr
                           : legacyComposition->graph().findNode(kFirstSolidNode);
    expectations.expect(legacyComposition != nullptr && legacyNode != nullptr &&
                            legacyComposition->parameters().erase(kTextFont) &&
                            std::erase_if(legacyNode->parameters,
                                          [](const auto& binding) {
                                              return binding.role ==
                                                     document::kTextFontParameterRole;
                                          }) == 1,
                        "an older Text v2 fixture can omit the new font binding");
    const auto legacyResult = compile(std::move(legacyText), registry);
    const auto* legacyCompiledText =
        legacyResult.plan && !legacyResult.plan->operations().empty()
            ? std::get_if<runtime::CompiledText>(&legacyResult.plan->operations().front())
            : nullptr;
    expectations.expect(legacyResult.status == runtime::SnapshotCompileStatus::Compiled &&
                            legacyCompiledText != nullptr &&
                            legacyCompiledText->face == render::EmbeddedFace::DejaVuSans,
                        "an older Text v2 document defaults the missing face to DejaVu Sans");

    // A size the schema refuses never reaches the evaluator: the document rejects the value at
    // insertion, so there is no "valid document, unrenderable plan" state to lower.
    auto oversized = makeProject(singleLayerOptions());
    auto* oversizedComposition = oversized.findComposition(kCompositionId);
    expectations.expect(
        !oversizedComposition->parameters().insert(
            {kTextSize, std::string(document::kTextSizeParameterSchemaKey),
             document::ConstantValueSource{document::kMaximumTextSizePixels + 1.0}}) &&
            !oversizedComposition->parameters().insert(
                {kTextSize, std::string(document::kTextSizeParameterSchemaKey),
                 document::ConstantValueSource{0.0}}),
        "the text size schema refuses an out-of-range size at the document boundary");
}

void testReachableSchemaDiagnostics(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    // ADAPTED (task FIX1, item B): a Layer Output's content input is OPTIONAL now, because an
    // artist wires a Layer node up by hand and "added but not yet fed" is an ordinary intermediate
    // state. The layer is classified as an empty image instead of failing the compile, so the
    // composition still produces a frame -- one in which that layer draws nothing.
    auto missingInputOptions = singleLayerOptions();
    missingInputOptions.omitFirstSourceEdge = true;
    const auto missingInput = compile(makeProject(std::move(missingInputOptions)), registry);
    expectations.expect(
        missingInput.status == runtime::SnapshotCompileStatus::Compiled &&
            !hasDiagnostic(missingInput, runtime::CompileDiagnosticCode::MissingInput,
                           kFirstLayerNode),
        "an unfed Layer Output compiles to nothing rather than failing the compile");

    auto wrongPortOptions = singleLayerOptions();
    wrongPortOptions.firstSourcePort = "pixels";
    const auto wrongPort = compile(makeProject(std::move(wrongPortOptions)), registry);
    expectations.expect(
        wrongPort.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(wrongPort, runtime::CompileDiagnosticCode::UnknownPort, kFirstSolidNode),
        "unknown reachable output port reports its source node and edge");

    auto unexpected = makeProject(singleLayerOptions());
    auto* composition = unexpected.findComposition(kCompositionId);
    const auto extraParameter = document::ParameterId::fromRaw(90);
    require(composition->parameters().insert(
                {extraParameter, "example.extra", document::ConstantValueSource{1.0}}),
            "extra parameter fixture must be accepted");
    composition->graph().findNode(kFirstSolidNode)->parameters.push_back({"extra", extraParameter});
    require(unexpected.validate().ok(), "extra extension binding remains valid document truth");
    const auto unexpectedResult = compile(std::move(unexpected), registry);
    expectations.expect(unexpectedResult.status == runtime::SnapshotCompileStatus::Failed &&
                            hasDiagnostic(unexpectedResult,
                                          runtime::CompileDiagnosticCode::UnexpectedParameter,
                                          kFirstSolidNode),
                        "unexpected binding is rejected by the registered evaluator schema");
}

void testTypedParameterDiagnostics(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    require(registry.registerDefinition(unsupportedColorDefinition()) ==
                runtime::NodeRegistrationStatus::Registered,
            "custom diagnostic schema must register");
    registry.freeze();

    auto makeCustom = [] {
        auto project = makeProject(singleLayerOptions());
        auto* node = project.findComposition(kCompositionId)->graph().findNode(kFirstSolidNode);
        node->typeId = "example.unsupported-color";
        node->schemaVersion = 1;
        node->parameters = {{"value", kFirstColor}};
        require(project.validate().ok(), "custom extension node must remain valid project truth");
        return project;
    };

    auto schemaMismatch = makeCustom();
    const auto schemaResult = compile(std::move(schemaMismatch), registry);
    expectations.expect(schemaResult.status == runtime::SnapshotCompileStatus::Failed &&
                            hasDiagnostic(schemaResult,
                                          runtime::CompileDiagnosticCode::ParameterSchemaMismatch,
                                          kFirstSolidNode),
                        "registered evaluator schema rejects a different parameter schema key");

    auto valueMismatch = makeCustom();
    auto& valueParameters = valueMismatch.findComposition(kCompositionId)->parameters();
    require(valueParameters.erase(kFirstColor) &&
                valueParameters.insert(
                    {kFirstColor, "example.color", document::ConstantValueSource{2.0}}) &&
                valueMismatch.validate().ok(),
            "generic wrong-value-kind fixture must remain valid document truth");
    const auto valueResult = compile(std::move(valueMismatch), registry);
    expectations.expect(
        valueResult.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(valueResult, runtime::CompileDiagnosticCode::ParameterValueKindMismatch,
                          kFirstSolidNode),
        "registered evaluator schema rejects a constant of the wrong typed kind");

    auto missing = makeCustom();
    missing.findComposition(kCompositionId)->graph().findNode(kFirstSolidNode)->parameters.clear();
    require(missing.validate().ok(), "missing extension binding remains valid document truth");
    const auto missingResult = compile(std::move(missing), registry);
    expectations.expect(missingResult.status == runtime::SnapshotCompileStatus::Failed &&
                            hasDiagnostic(missingResult,
                                          runtime::CompileDiagnosticCode::MissingParameter,
                                          kFirstSolidNode),
                        "registered evaluator schema requires its declared parameter role");
}

void testParameterSourcesAndDiagnosticIds(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    auto animated = makeProject(singleLayerOptions());
    auto* animatedComposition = animated.findComposition(kCompositionId);
    require(animatedComposition != nullptr, "animation fixture composition must exist");
    const auto animatedFormat = requireValue(document::CompositionFormat::create(4, 2),
                                             "animation evaluation format must be valid");
    animatedComposition->setFormat(animatedFormat);
    auto* parameters = &animatedComposition->parameters();
    require(parameters->setSource(kFirstWidth, document::ConstantValueSource{static_cast<double>(
                                                   animatedFormat.width())}) &&
                parameters->setSource(
                    kFirstHeight,
                    document::ConstantValueSource{static_cast<double>(animatedFormat.height())}),
            "animation fixture solid must fill its re-declared format");
    constexpr auto curveId = document::AnimationCurveId::fromRaw(100);
    constexpr auto positionCurveId = document::AnimationCurveId::fromRaw(103);
    require(animatedComposition->animationCurves().insert(document::ScalarAnimationCurve{
                curveId,
                {{document::KeyframeId::fromRaw(101), core::RationalTime::fromInteger(0), 0.2,
                  document::KeyframeInterpolation::Hold},
                 {document::KeyframeId::fromRaw(102), core::RationalTime::fromInteger(1), 0.8,
                  document::KeyframeInterpolation::Linear}},
            }),
            "typed scalar curve must be publishable");
    require(animatedComposition->animationCurves().insert(document::Vec2AnimationCurve{
                positionCurveId,
                {document::ComponentAnimationCurve{
                     {{document::KeyframeId::fromRaw(104), core::RationalTime::fromInteger(0), 2.0,
                       document::KeyframeInterpolation::Linear},
                      {document::KeyframeId::fromRaw(105), core::RationalTime::fromInteger(1), 3.0,
                       document::KeyframeInterpolation::Linear}}},
                 document::ComponentAnimationCurve{
                     {{document::KeyframeId::fromRaw(106), core::RationalTime::fromInteger(0), 1.0,
                       document::KeyframeInterpolation::Linear},
                      {document::KeyframeId::fromRaw(107), core::RationalTime::fromInteger(1), 1.0,
                       document::KeyframeInterpolation::Linear}}}},
            }),
            "typed Vec2 curve must be publishable");
    require(parameters->setSource(kFirstOpacity, document::AnimationCurveSource{curveId}),
            "typed curve reference must be publishable");
    require(parameters->setSource(kFirstPosition, document::AnimationCurveSource{positionCurveId}),
            "typed position curve reference must be publishable");
    require(animated.validate().ok(), "curve source fixture must remain valid document truth");
    const auto result = compile(std::move(animated), registry);
    expectations.expect(result.status == runtime::SnapshotCompileStatus::Compiled && result.plan &&
                            result.diagnostics.empty(),
                        "a supported typed animation curve lowers into the runtime plan");
    if (result.plan) {
        const auto* layer =
            std::get_if<runtime::CompiledLayerOutput>(&result.plan->operations()[1]);
        const auto* curveIndex =
            layer == nullptr ? nullptr
                             : std::get_if<runtime::ScalarCurveIndex>(&layer->opacity.source);
        const auto* positionCurveIndex =
            layer == nullptr ? nullptr
                             : std::get_if<runtime::Vec2CurveIndex>(&layer->position.source);
        expectations.expect(
            result.plan->scalarCurves().size() == 1 && result.plan->vec2Curves().size() == 1 &&
                result.plan->scalarCurves().front() ==
                    runtime::CompiledScalarCurve{
                        curveId,
                        {{document::KeyframeId::fromRaw(101), core::RationalTime::fromInteger(0),
                          0.2, runtime::CompiledKeyframeInterpolation::Hold},
                         {document::KeyframeId::fromRaw(102), core::RationalTime::fromInteger(1),
                          0.8, runtime::CompiledKeyframeInterpolation::Linear}}} &&
                layer != nullptr && layer->opacity.id == kFirstOpacity && curveIndex != nullptr &&
                curveIndex->value() == 0 && layer->position.id == kFirstPosition &&
                positionCurveIndex != nullptr && positionCurveIndex->value() == 0 &&
                result.plan->vec2Curves().front().id == positionCurveId &&
                result.plan->vec2Curves().front().components[0].size() == 2 &&
                result.plan->vec2Curves().front().components[1].size() == 2,
            "compiled curve tables and parameter references preserve canonical typed identity");

        const auto halfway = requireValue(core::RationalTime::create(1, 2),
                                          "animation evaluation time must be valid");
        const runtime::CpuCompositionEvaluator evaluator;
        const auto evaluated =
            evaluator.evaluate(result.plan,
                               {.time = halfway,
                                .output = result.plan->output(),
                                .resolution = runtime::CompositionFormatResolution{},
                                .quality = runtime::EvaluationQuality::Reference,
                                .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                .pixelStorageByteLimit = 1U << 20U},
                               runtime::CancellationToken{});
        expectations.expect(evaluated.status() == runtime::EvaluationStatus::Evaluated &&
                                evaluated.frame() != nullptr &&
                                evaluated.frame()->identity().plan == result.plan,
                            "compiler output evaluates animated position and opacity end to end");
    }
    expectations.expect(runtime::compileDiagnosticCodeId(
                            runtime::CompileDiagnosticCode::UnsupportedParameterSource) ==
                            "bloom.runtime.compile.unsupported-parameter-source",
                        "compiler diagnostics expose stable machine-readable identifiers");

    // ADAPTED (task S7): a driver binding on a Colour parameter is evaluable now, so what used to
    // be an unsupported-source assertion is a positive one -- the solid's colour compiles to a
    // value-graph output, and the value node that supplies it becomes a compiled value operation.
    auto driven = makeProject(singleLayerOptions());
    attachValueDriver(driven, document::NodeId::fromRaw(14), document::ParameterId::fromRaw(46),
                      document::kColorValueNodeType, document::kColorValueParameterSchemaKey,
                      core::Color4d{0.25, 0.5, 0.75, 1.0}, kFirstColor);
    const auto drivenResult = compile(std::move(driven), registry);
    expectations.expect(drivenResult.status == runtime::SnapshotCompileStatus::Compiled &&
                            drivenResult.plan,
                        "a Colour parameter driven by a Colour value node compiles");
    if (drivenResult.plan) {
        const auto& plan = *drivenResult.plan;
        expectations.expect(plan.valueOperations().size() == 1 && plan.valueOutputCount() == 1,
                            "the driving value node compiles to exactly one value operation");
        const auto solid = std::ranges::find_if(plan.operations(), [](const auto& operation) {
            return std::holds_alternative<runtime::CompiledSolid>(operation);
        });
        expectations.expect(solid != plan.operations().end() &&
                                std::holds_alternative<runtime::ValueOutputIndex>(
                                    std::get<runtime::CompiledSolid>(*solid).color.source),
                            "the driven colour operand resolves to a value-graph output");
    }

    // ADAPTED (task DRIVE-1): the other half of the same rule used to be that only Scalar, Vector2
    // and Colour had a value-graph arm, so an Integer one -- the blend mode -- stayed an
    // unsupported source. Every kind has an arm now, so the assertion is the positive one: the link
    // compiles, and the layer carries the value output rather than only its authored constant.
    auto drivenBlendMode = makeProject(singleLayerOptions());
    attachValueDriver(drivenBlendMode, document::NodeId::fromRaw(14),
                      document::ParameterId::fromRaw(46), document::kIntegerValueNodeType,
                      document::kIntegerValueParameterSchemaKey, std::int64_t{0}, kFirstBlendMode);
    const auto blendModeResult = compile(std::move(drivenBlendMode), registry);
    expectations.expect(blendModeResult.status == runtime::SnapshotCompileStatus::Compiled &&
                            blendModeResult.plan,
                        "an Integer parameter driven by an Integer value node compiles");
    if (blendModeResult.plan) {
        const auto& plan = *blendModeResult.plan;
        const auto layer = std::ranges::find_if(plan.operations(), [](const auto& operation) {
            return std::holds_alternative<runtime::CompiledLayerOutput>(operation);
        });
        expectations.expect(
            layer != plan.operations().end() &&
                std::get<runtime::CompiledLayerOutput>(*layer).drivenBlendMode.has_value(),
            "the driven blend mode resolves to a value-graph output");
    }

    // Task DRIVE-1's headline case, at the compiler: a text layer whose WORDS come from a String
    // node. The layer's source node is retyped to a Text source first, so the parameter the driver
    // lands on is a real content parameter rather than a colour that happens to take a String.
    auto drivenContent = makeProject(singleLayerOptions());
    retypeFirstSourceToText(drivenContent, "Title", 48.0, core::Color4d{1.0, 1.0, 1.0, 1.0});
    attachValueDriver(drivenContent, document::NodeId::fromRaw(14),
                      document::ParameterId::fromRaw(46), document::kStringValueNodeType,
                      document::kStringValueParameterSchemaKey, std::string("Driven"), kFirstColor);
    const auto contentResult = compile(std::move(drivenContent), registry);
    expectations.expect(contentResult.status == runtime::SnapshotCompileStatus::Compiled &&
                            contentResult.plan,
                        "a String parameter driven by a String value node compiles");
    if (contentResult.plan) {
        const auto& plan = *contentResult.plan;
        const auto text = std::ranges::find_if(plan.operations(), [](const auto& operation) {
            return std::holds_alternative<runtime::CompiledText>(operation);
        });
        expectations.expect(text != plan.operations().end() &&
                                std::get<runtime::CompiledText>(*text).drivenContent.has_value(),
                            "the driven text content resolves to a value-graph output");
    }
}

// An authored blend mode lowers from its stored integer through core::BlendMode's one mapping, and
// an integer naming no implemented mode never reaches lowering at all: ParameterStore refuses it on
// write, so the lowering path has no "unknown mode" branch to guess in.
void testBlendModeLowersFromItsStoredInteger(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    auto project = makeProject(singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "blend-mode fixture composition must exist");
    auto& parameters = composition->parameters();
    expectations.expect(
        !parameters.setSource(
            kFirstBlendMode,
            document::ConstantValueSource{
                bloom::core::blendModeStoredValue(bloom::core::BlendMode::Difference) + 1}),
        "an integer naming no implemented blend mode is refused by the document layer");
    expectations.expect(
        !parameters.setSource(kFirstBlendMode, document::ConstantValueSource{std::int64_t{-1}}),
        "a negative stored blend mode is refused by the document layer");
    require(parameters.setSource(kFirstBlendMode,
                                 document::ConstantValueSource{bloom::core::blendModeStoredValue(
                                     bloom::core::BlendMode::Overlay)}),
            "an implemented blend mode must be publishable");
    require(project.validate().ok(), "blend-mode fixture must remain valid document truth");

    const auto result = compile(std::move(project), registry);
    const auto* layer =
        result.plan == nullptr
            ? nullptr
            : std::get_if<runtime::CompiledLayerOutput>(&result.plan->operations()[1]);
    expectations.expect(result.status == runtime::SnapshotCompileStatus::Compiled &&
                            layer != nullptr && layer->blendMode == bloom::core::BlendMode::Overlay,
                        "an authored blend mode lowers to its own enumerator");
}

void testRequestScopedParameterOverrides(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const auto base = compile(makeProject(singleLayerOptions()), registry);
    const runtime::SnapshotParameterOverride firstOverride{document::Revision{}, kFirstPosition,
                                                           document::Vec2d{12.5, -4.0}};
    const auto first = compile(makeProject(singleLayerOptions()), registry, {firstOverride});
    const auto second =
        compile(makeProject(singleLayerOptions()), registry,
                {runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition,
                                                    document::Vec2d{12.5, -3.0}}});
    const auto* layer =
        first.plan == nullptr
            ? nullptr
            : std::get_if<runtime::CompiledLayerOutput>(&first.plan->operations()[1]);
    const auto* position =
        layer == nullptr ? nullptr : std::get_if<document::Vec2d>(&layer->position.source);
    expectations.expect(
        first.status == runtime::SnapshotCompileStatus::Compiled && first.plan &&
            position != nullptr && *position == document::Vec2d{12.5, -4.0} && base.plan &&
            *base.plan != *first.plan && second.plan && *first.plan != *second.plan,
        "a typed request override enters the immutable plan without document edits");

    const auto wrongRevision =
        compile(makeProject(singleLayerOptions()), registry,
                {runtime::SnapshotParameterOverride{document::Revision::fromRaw(1), kFirstPosition,
                                                    document::Vec2d{0.0, 0.0}}});
    expectations.expect(
        wrongRevision.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(wrongRevision, runtime::CompileDiagnosticCode::InvalidParameterOverride),
        "override admission rejects a captured revision mismatch first");

    const auto wrongKind =
        compile(makeProject(singleLayerOptions()), registry,
                {runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition, 0.5}});
    const auto wrongDomain =
        compile(makeProject(singleLayerOptions()), registry,
                {runtime::SnapshotParameterOverride{document::Revision{}, kFirstOpacity, 1.5}});
    expectations.expect(
        wrongKind.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(wrongKind, runtime::CompileDiagnosticCode::InvalidParameterOverride) &&
            wrongDomain.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(wrongDomain, runtime::CompileDiagnosticCode::InvalidParameterOverride),
        "override value kind and parameter domain are validated before lowering");

    auto unreachableProject = makeProject(singleLayerOptions());
    auto* unreachableComposition = unreachableProject.findComposition(kCompositionId);
    constexpr auto unreachableParameter = document::ParameterId::fromRaw(90);
    document::NodeRecord unreachableNode{document::NodeId::fromRaw(91),
                                         "example.unreachable",
                                         {{"position", unreachableParameter}},
                                         1};
    require(unreachableComposition != nullptr &&
                unreachableComposition->parameters().insert(
                    {unreachableParameter, std::string(document::kPositionParameterSchemaKey),
                     document::ConstantValueSource{document::Vec2d{1.0, 2.0}}}) &&
                unreachableComposition->graph().addNode(std::move(unreachableNode)) &&
                unreachableProject.validate().ok(),
            "unreachable override fixture must remain valid document truth");
    const auto unreachable =
        compile(std::move(unreachableProject), registry,
                {runtime::SnapshotParameterOverride{document::Revision{}, unreachableParameter,
                                                    document::Vec2d{3.0, 4.0}}});
    expectations.expect(
        unreachable.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(unreachable, runtime::CompileDiagnosticCode::InvalidParameterOverride),
        "override targets must participate in the requested output path");

    auto drivenProject = makeProject(singleLayerOptions());
    attachValueDriver(drivenProject, document::NodeId::fromRaw(14),
                      document::ParameterId::fromRaw(46), document::kVector2ValueNodeType,
                      document::kVector2ValueParameterSchemaKey, document::Vec2d{7.0, 9.0},
                      kFirstPosition);
    const auto driven =
        compile(std::move(drivenProject), registry,
                {runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition,
                                                    document::Vec2d{3.0, 4.0}}});
    expectations.expect(
        driven.status == runtime::SnapshotCompileStatus::Unsupported &&
            hasDiagnostic(driven, runtime::CompileDiagnosticCode::UnsupportedParameterOverride),
        "an override never hides or disconnects a driver source");

    auto animatedProject = makeProject(singleLayerOptions());
    auto* animatedComposition = animatedProject.findComposition(kCompositionId);
    constexpr auto curveId = document::AnimationCurveId::fromRaw(93);
    require(animatedComposition->animationCurves().insert(document::Vec2AnimationCurve{
                curveId,
                {document::ComponentAnimationCurve{
                     {{document::KeyframeId::fromRaw(94), core::RationalTime::fromInteger(0), 120.0,
                       document::KeyframeInterpolation::Linear}}},
                 document::ComponentAnimationCurve{
                     {{document::KeyframeId::fromRaw(95), core::RationalTime::fromInteger(0), 80.0,
                       document::KeyframeInterpolation::Linear}}}},
            }) &&
                animatedComposition->parameters().setSource(
                    kFirstPosition, document::AnimationCurveSource{curveId}) &&
                animatedProject.validate().ok(),
            "animated override fixture must remain valid document truth");
    const auto animated =
        compile(std::move(animatedProject), registry,
                {runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition,
                                                    document::Vec2d{7.0, 8.0}}});
    expectations.expect(animated.status == runtime::SnapshotCompileStatus::Compiled &&
                            animated.plan && animated.plan->vec2Curves().empty(),
                        "an accepted override lowers as a constant and omits its dormant curve");
}

void testLiveValueOverrideOwners(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto project = makeProject(singleLayerOptions());
    retypeFirstSourceToText(project, "Before", 24, {1, 1, 1, 1});
    auto* composition = project.findComposition(kCompositionId);
    std::uint64_t nextNode = 300, nextParameter = 600;
    const auto add = [&](const std::string_view type) {
        const auto* definition = registry.find(type, 1);
        require(definition != nullptr, "live override fixture definition exists");
        NodeRecord node{NodeId::fromRaw(nextNode++), std::string(type), {}, 1};
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(nextParameter++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                    "live override fixture parameter inserts");
            node.parameters.push_back({parameter.role, id});
        }
        require(composition->graph().addNode(node), "live override fixture node inserts");
        return node;
    };
    const auto parameter = [](const NodeRecord& node, const std::string_view role) {
        const auto found = std::ranges::find(node.parameters, role, &ParameterBinding::role);
        require(found != node.parameters.end(), "live override fixture role exists");
        return found->parameterId;
    };
    const auto drive = [&](const ParameterId id, const NodeRecord& node, std::string port) {
        require(
            composition->parameters().setSource(id, DriverBindingSource{node.id, std::move(port)}),
            "live override fixture driver installs");
    };
    const auto scalar = add(kScalarValueNodeType);
    const auto vector = add(kVector2ValueNodeType);
    const auto color = add(kColorValueNodeType);
    const auto integer = add(kIntegerValueNodeType);
    const auto text = add(kStringValueNodeType);
    const auto boolean = add(kBooleanValueNodeType);
    const auto vector3 = add(kVector3ValueNodeType);
    const auto separate = add(kSeparateXyzNodeType);
    const auto choice = add(kScalarSwitchNodeType);
    drive(kFirstOpacity, choice, "result");
    drive(parameter(choice, "condition"), boolean, "value");
    drive(parameter(choice, "ifFalse"), scalar, "value");
    drive(parameter(choice, "ifTrue"), separate, "x");
    drive(parameter(separate, "vector"), vector3, "value");
    drive(kFirstPosition, vector, "value");
    drive(kTextColor, color, "value");
    drive(kFirstBlendMode, integer, "value");
    drive(kFirstColor, text, "value");
    require(project.validate().ok(), "all live override kinds form valid document truth");

    const auto check = [&](const Project& before, const ParameterId id,
                           const runtime::SnapshotParameterOverride& override) {
        const auto preview = compile(before, registry, {override});
        auto authored = before;
        const auto value =
            std::visit([](const auto& held) -> ParameterValue { return held; }, override.value);
        require(authored.findComposition(kCompositionId)
                    ->parameters()
                    .setSource(id, ConstantValueSource{value}),
                "reference edit installs");
        const auto committed = compile(authored, registry);
        expectations.expect(preview.plan && committed.plan,
                            "every reachable owner and live value kind admits an override");
        if (!preview.plan || !committed.plan) {
            for (const auto& diagnostic : preview.diagnostics)
                std::cerr << "Preview: " << diagnostic.summary << " " << diagnostic.detail << '\n';
            for (const auto& diagnostic : committed.diagnostics)
                std::cerr << "Authored: " << diagnostic.summary << " " << diagnostic.detail << '\n';
            return;
        }
        const auto sample = [](const auto& plan) {
            return runtime::evaluateValueGraph(plan->valueOperations(), plan->valueOutputCount(),
                                               core::RationalTime::fromInteger(1),
                                               plan->format().frameRate());
        };
        const auto live = sample(preview.plan), durable = sample(committed.plan);
        expectations.expect(live.diagnostics.empty() && durable.diagnostics.empty() &&
                                live.outputs == durable.outputs,
                            "request-local values lower identically to authored constants");
    };
    const std::array<runtime::SnapshotParameterOverride, 7> overrides{{
        {{}, parameter(scalar, "value"), 0.7},
        {{}, parameter(vector, "value"), Vec2d{9, 11}},
        {{}, parameter(vector3, "value"), Vec3d{0.6, 0.4, 0.2}},
        {{}, parameter(color, "value"), core::Color4d{0.2, 0.4, 0.6, 1}},
        {{}, parameter(integer, "value"), std::int64_t{1}},
        {{}, parameter(boolean, "value"), true},
        {{}, parameter(text, "value"), std::string("Live")},
    }};
    for (const auto& override : overrides)
        check(project, override.parameterId, override);
    auto math = makeTimeDrivenOpacityChain();
    check(math.project, ParameterId::fromRaw(51), {{}, ParameterId::fromRaw(51), 0.7});
    const auto utility = add("bloom.scalar-to-string");
    drive(kFirstColor, utility, "result");
    const auto operand = utility.parameters.front().parameterId;
    check(project, operand, {{}, operand, 42.0});
    const auto driven = compile(project, registry, {{{}, parameter(choice, "ifFalse"), 0.5}});
    expectations.expect(
        driven.status == runtime::SnapshotCompileStatus::Unsupported &&
            hasDiagnostic(driven, runtime::CompileDiagnosticCode::UnsupportedParameterOverride),
        "a reachable utility's driven source cannot be overridden");
}

void testOverrideVectorKindsAndLimits(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto textProject = makeProject(singleLayerOptions());
    retypeFirstSourceToText(textProject, "Before", 24.0, {1, 1, 1, 1});
    const std::vector<runtime::SnapshotParameterOverride> overrides{
        {{}, kFirstColor, std::string("After")},
        {{}, kTextSize, 42.0},
        {{}, kTextColor, core::Color4d{0.2, 0.3, 0.4, 0.5}},
        {{}, kTextAlignment, std::int64_t{2}},
        {{}, kFirstPosition, document::Vec2d{7, 9}}};
    const auto result = compile(textProject, registry, overrides);
    const auto* text =
        result.plan ? std::get_if<runtime::CompiledText>(&result.plan->operations()[0]) : nullptr;
    expectations.expect(text && text->content == "After" && text->layout.alignment == 2 &&
                            std::get<double>(text->size.source) == 42.0 &&
                            std::get<core::Color4d>(text->color.source) ==
                                core::Color4d{0.2, 0.3, 0.4, 0.5},
                        "one request lowers string, scalar, integer, colour and vector overrides");
    auto full = overrides;
    full.push_back({{}, kTextLineHeight, 1.25});
    full.push_back({{}, kTextLetterSpacing, 2.0});
    full.push_back({{}, kFirstOpacity, 0.5});
    expectations.expect(compile(textProject, registry, full).status ==
                            runtime::SnapshotCompileStatus::Compiled,
                        "exactly eight distinct overrides are accepted");
    full.back().sourceRevision = document::Revision::fromRaw(99);
    expectations.expect(compile(textProject, registry, full).status ==
                            runtime::SnapshotCompileStatus::Failed,
                        "a stale entry rejects the whole override vector");
    auto duplicate = overrides;
    duplicate.push_back(overrides.front());
    expectations.expect(compile(textProject, registry, duplicate).status ==
                            runtime::SnapshotCompileStatus::Failed,
                        "duplicate parameter overrides are refused");
    duplicate.resize(9, overrides.front());
    expectations.expect(compile(textProject, registry, duplicate).status ==
                            runtime::SnapshotCompileStatus::Failed,
                        "more than eight overrides are refused");
    expectations.expect(
        compile(textProject, registry, {{{}, kTextAlignment, std::int64_t{99}}}).status ==
            runtime::SnapshotCompileStatus::Failed,
        "integer enum domain is checked");
    auto imageProject = makeProject(singleLayerOptions());
    auto* composition = imageProject.findComposition(kCompositionId);
    auto* node = composition->graph().findNode(kFirstSolidNode);
    const auto* definition = registry.find("bloom.image-source", 1);
    require(definition != nullptr, "image definition exists");
    node->typeId = definition->key.typeId;
    node->schemaVersion = definition->key.schemaVersion;
    node->parameters.clear();
    document::ParameterId booleanId;
    std::uint64_t raw = 500;
    for (const auto& parameter : definition->parameters) {
        const auto id = document::ParameterId::fromRaw(raw++);
        require(
            composition->parameters().insert(
                {id, parameter.schemaKey, document::ConstantValueSource{parameter.defaultValue}}),
            "insert image parameter");
        node->parameters.push_back({parameter.role, id});
        if (parameter.role == "premultiply")
            booleanId = id;
    }
    const auto imageResult = compile(imageProject, registry, {{{}, booleanId, std::int64_t{0}}});
    const auto* image =
        imageResult.plan
            ? std::get_if<runtime::CompiledImageSource>(&imageResult.plan->operations()[0])
            : nullptr;
    expectations.expect(image && !image->premultiply,
                        "Boolean override lowers integer zero to false");
    expectations.expect(
        compile(imageProject, registry, {{{}, booleanId, std::int64_t{2}}}).status ==
            runtime::SnapshotCompileStatus::Failed,
        "Boolean overrides accept only zero or one");
}

[[nodiscard]] document::Project makeCancellationStressProject(const std::size_t edgeCount) {
    auto project = makeProject();
    auto& graph = project.findComposition(kCompositionId)->graph();
    constexpr std::uint64_t kBase = 1'000;
    for (std::size_t index = 0; index <= edgeCount; ++index) {
        require(
            graph.addNode(
                {document::NodeId::fromRaw(kBase + index), "example.unreachable-stress", {}, 1}),
            "stress node must be accepted");
    }
    for (std::size_t index = 0; index < edgeCount; ++index) {
        require(graph.addEdge({document::EdgeId::fromRaw(kBase + index),
                               {document::NodeId::fromRaw(kBase + index), "image"},
                               document::NodeInputRef{document::NodeId::fromRaw(kBase + index + 1),
                                                      "input"}}),
                "stress edge must be accepted");
    }
    require(project.validate().ok(), "cancellation stress graph must be valid document truth");
    return project;
}

class BlockingCheckpointObserver final : public runtime::detail::CompileCheckpointObserver {
  public:
    BlockingCheckpointObserver(const runtime::detail::CompileCheckpointPhase targetPhase,
                               const std::size_t targetCheckpoint)
        : targetPhase_(targetPhase), targetCheckpoint_(targetCheckpoint) {}

    void checkpoint(const runtime::detail::CompileCheckpointPhase phase) override {
        if (phase != targetPhase_) {
            return;
        }
        std::unique_lock lock(mutex_);
        ++checkpointCount_;
        if (checkpointCount_ != targetCheckpoint_) {
            return;
        }
        reached_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_; });
    }

    void complete(const runtime::SnapshotCompileResult& result) {
        std::lock_guard lock(mutex_);
        completed_ = true;
        status_ = result.status;
        publishedPlan_ = static_cast<bool>(result.plan);
        condition_.notify_all();
    }

    [[nodiscard]] bool waitUntilReached() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [&] { return reached_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] bool waitUntilComplete() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [&] { return completed_; });
    }

    [[nodiscard]] runtime::SnapshotCompileStatus status() const {
        std::lock_guard lock(mutex_);
        return status_;
    }

    [[nodiscard]] bool publishedPlan() const {
        std::lock_guard lock(mutex_);
        return publishedPlan_;
    }

  private:
    const runtime::detail::CompileCheckpointPhase targetPhase_;
    const std::size_t targetCheckpoint_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t checkpointCount_ = 0;
    bool reached_ = false;
    bool released_ = false;
    bool completed_ = false;
    runtime::SnapshotCompileStatus status_ = runtime::SnapshotCompileStatus::Failed;
    bool publishedPlan_ = false;
};

void testMidWorkCancellationIsBounded(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    constexpr std::size_t bulkDefinitionCount = 4'096;
    for (std::size_t index = 0; index < bulkDefinitionCount; ++index) {
        require(registry.registerDefinition(bulkUnsupportedDefinition(index)) ==
                    runtime::NodeRegistrationStatus::Registered,
                "large cancellation registry definition must register");
    }
    registry.freeze();
    document::Document document(makeCancellationStressProject(256));
    const runtime::SnapshotCompileRequest request{document.snapshot(), kCompositionId};
    BlockingCheckpointObserver observer(runtime::detail::CompileCheckpointPhase::IncomingEdgeIndex,
                                        32);
    runtime::TaskScheduler scheduler({.cpuWorkerCount = 1,
                                      .blockingIoWorkerCount = 1,
                                      .cpuQueueCapacity = 4,
                                      .blockingIoQueueCapacity = 4,
                                      .terminalHistoryCapacity = 4,
                                      .diagnosticsPerTask = 4,
                                      .groupRegistryCapacity = 4});
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Cancelled compile", {.kind = runtime::TaskOwnerKind::Composition,
                                                   .id = runtime::TaskOwnerId::fromRaw(1)}),
        [&](runtime::TaskContext& context) {
            const auto result = runtime::detail::compileSnapshot(registry, request,
                                                                 context.cancellation(), &observer);
            observer.complete(result);
            return runtime::TaskResult<void>::cancelled();
        });
    const bool reached = submission.accepted() && observer.waitUntilReached();
    expectations.expect(
        reached, "stress compile deterministically reaches the in-flight incoming-edge index pass");
    submission.handle.cancel();
    observer.release();
    expectations.expect(observer.waitUntilComplete(),
                        "mid-work cancellation completes without scanning the remaining graph");
    expectations.expect(observer.status() == runtime::SnapshotCompileStatus::Cancelled,
                        "mid-work cancellation produces the compiler cancellation status");
    expectations.expect(!observer.publishedPlan(),
                        "mid-work cancellation never publishes a partial plan");

    BlockingCheckpointObserver definitionObserver(
        runtime::detail::CompileCheckpointPhase::DefinitionResolution, 2);
    auto definitionSubmission = scheduler.submit<void>(
        runtime::TaskRequest(
            "Cancelled large-registry lookup",
            {.kind = runtime::TaskOwnerKind::Composition, .id = runtime::TaskOwnerId::fromRaw(2)}),
        [&](runtime::TaskContext& context) {
            const auto result = runtime::detail::compileSnapshot(
                registry, request, context.cancellation(), &definitionObserver);
            definitionObserver.complete(result);
            return runtime::TaskResult<void>::cancelled();
        });
    const bool definitionReached =
        definitionSubmission.accepted() && definitionObserver.waitUntilReached();
    expectations.expect(definitionReached,
                        "large-registry compile reaches the post-lookup cancellation boundary");
    definitionSubmission.handle.cancel();
    definitionObserver.release();
    expectations.expect(definitionObserver.waitUntilComplete() &&
                            definitionObserver.status() ==
                                runtime::SnapshotCompileStatus::Cancelled &&
                            !definitionObserver.publishedPlan(),
                        "large-registry cancellation stops before further definition resolution");
}

#include "snapshot_compiler_effect_tests.ipp"
#include "snapshot_compiler_mute_tests.ipp"

} // namespace

int main() {
    Expectations expectations;
    try {
        testImageEffectLowering(expectations);
        testLiveValueOverrideOwners(expectations);
        testOverrideVectorKindsAndLimits(expectations);
        testLayerFlagsAndRangeLowering(expectations);
        testMuteKindsAndPixels(expectations);
        testMuteFirstImageInput(expectations);
        testParentOrder(expectations);
        testNestedMergeCompilation(expectations);
        testRegistryMustBeFrozen(expectations);
        testValueGraphDriverResolution(expectations);
        testCompositionReadoutsLowerToConstants(expectations);
        testFrameNumberReadoutIsResolvedPerFrame(expectations);
        testValueGraphCycleRefusal(expectations);
        testLayerBoundsReadoutAndFeedbackRefusal(expectations);
        testDeterministicTypedPlan(expectations);
        testCustomSolidLoweringRemainsSupported(expectations);
        testReachabilityAndUnsupportedNodes(expectations);
        testReachableSchemaDiagnostics(expectations);
        testTypedParameterDiagnostics(expectations);
        testParameterSourcesAndDiagnosticIds(expectations);
        testBlendModeLowersFromItsStoredInteger(expectations);
        testRequestScopedParameterOverrides(expectations);
        testMidWorkCancellationIsBounded(expectations);
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test fixture failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
