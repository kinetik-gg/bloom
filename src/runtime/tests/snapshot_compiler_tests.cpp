#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include "snapshot_compiler_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
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
         {{std::string(kSolidColorParameterRole), kFirstColor}},
         kSolidSourceNodeSchemaVersion},
        {kFirstLayerNode,
         std::string(kLayerOutputNodeType),
         {{std::string(kPositionParameterRole), kFirstPosition},
          {std::string(kOpacityParameterRole), kFirstOpacity}},
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
                         {{std::string(kSolidColorParameterRole), kSecondColor}},
                         kSolidSourceNodeSchemaVersion});
        nodes.push_back({kSecondLayerNode,
                         std::string(kLayerOutputNodeType),
                         {{std::string(kPositionParameterRole), kSecondPosition},
                          {std::string(kOpacityParameterRole), kSecondOpacity}},
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
    require(
        composition.parameters().insert({kFirstPosition, std::string(kPositionParameterSchemaKey),
                                         ConstantValueSource{Vec2d{120.0, 80.0}}}),
        "first position must be accepted");
    require(composition.parameters().insert(
                {kFirstOpacity, std::string(kOpacityParameterSchemaKey), ConstantValueSource{0.8}}),
            "first opacity must be accepted");
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
    return {{"example.solid", 17},
            runtime::NodeLoweringKind::Solid,
            {},
            {{std::string(document::kSolidSourceOutputPort), runtime::SocketValueKind::Image}},
            {{std::string(document::kSolidColorParameterRole),
              std::string(document::kSolidColorParameterSchemaKey),
              runtime::ParameterValueKind::Color4d, true}},
            std::nullopt};
}

[[nodiscard]] runtime::NodeDefinition bulkUnsupportedDefinition(const std::size_t index) {
    return {{"example.bulk." + std::to_string(index), 1},
            runtime::NodeLoweringKind::Unsupported,
            {},
            {{"image", runtime::SocketValueKind::Image}},
            {},
            std::nullopt};
}

[[nodiscard]] runtime::SnapshotCompileResult
compile(document::Project project, runtime::NodeDefinitionRegistry& registry,
        std::optional<runtime::SnapshotParameterOverride> parameterOverride = std::nullopt) {
    document::Document document(std::move(project));
    runtime::SnapshotCompiler compiler(registry);
    return compiler.compile({document.snapshot(), kCompositionId, std::move(parameterOverride)},
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
    const auto pixelAspect = core::PixelAspectRatio::create(4, 3);
    const auto frameRate = document::FrameRate::create(30000, 1001);
    require(pixelAspect.has_value() && frameRate.has_value(), "format fixture must be valid");
    const auto format = document::CompositionFormat::create(2048, 858, *pixelAspect, *frameRate);
    require(format.has_value(), "non-square project format must be valid");

    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    const auto first = compile(makeProject({.format = *format}), registry);
    const auto second =
        compile(makeProject({.reverseInsertion = true, .format = *format}), registry);

    expectations.expect(first.status == runtime::SnapshotCompileStatus::Compiled && first.plan,
                        "supported reachable graph compiles");
    expectations.expect(second.status == runtime::SnapshotCompileStatus::Compiled && second.plan,
                        "different insertion order compiles");
    if (!first.plan || !second.plan) {
        return;
    }
    expectations.expect(*first.plan == *second.plan,
                        "plan is independent of document insertion and hash order");
    expectations.expect(
        first.plan->sourceRevision == document::Revision{} && first.plan->projectId == kProjectId &&
            first.plan->compositionId == kCompositionId && first.plan->format == *format &&
            first.plan->planSemanticsVersion == runtime::kCompiledCompositionPlanSemanticsVersion &&
            first.plan->animationSamplingSemanticsVersion ==
                runtime::kAnimationSamplingSemanticsVersion,
        "revision, identity, and exact composition format carry through");
    expectations.expect(first.plan->operations.size() == 6 &&
                            first.plan->output == runtime::OperationIndex::fromRaw(5),
                        "plan contains one topological operation per reachable node");

    const auto* solid = std::get_if<runtime::CompiledSolid>(&first.plan->operations[0]);
    const auto* firstLayer = std::get_if<runtime::CompiledLayerOutput>(&first.plan->operations[1]);
    const auto* stack = std::get_if<runtime::CompiledLayerStack>(&first.plan->operations[4]);
    const auto* output =
        std::get_if<runtime::CompiledCompositionOutput>(&first.plan->operations[5]);
    const auto* firstPosition = firstLayer == nullptr
                                    ? nullptr
                                    : std::get_if<document::Vec2d>(&firstLayer->position.source);
    const auto* firstOpacity =
        firstLayer == nullptr ? nullptr : std::get_if<double>(&firstLayer->opacity.source);
    expectations.expect(solid != nullptr && solid->sourceNodeId == kFirstSolidNode &&
                            solid->colorParameterId == kFirstColor &&
                            solid->color == core::Color4d{1.5, 0.25, 0.5, 0.75},
                        "solid preserves straight HDR authoring color and typed identity");
    expectations.expect(firstLayer != nullptr && firstLayer->input.value() == 0 &&
                            firstLayer->layerId == kFirstLayer &&
                            firstLayer->position.id == kFirstPosition && firstPosition != nullptr &&
                            *firstPosition == document::Vec2d{120.0, 80.0} &&
                            firstLayer->opacity.id == kFirstOpacity && firstOpacity != nullptr &&
                            *firstOpacity == 0.8,
                        "Layer Output preserves typed input and static properties");
    expectations.expect(stack != nullptr && stack->entries.size() == 2 &&
                            stack->entries[0] ==
                                runtime::CompiledLayerStackEntry{
                                    kFirstSlot, kFirstLayer, runtime::OperationIndex::fromRaw(1)} &&
                            stack->entries[1] ==
                                runtime::CompiledLayerStackEntry{
                                    kSecondSlot, kSecondLayer, runtime::OperationIndex::fromRaw(3)},
                        "Layer Stack preserves explicit top-to-bottom stable slot order");
    expectations.expect(output != nullptr && output->input.value() == 4,
                        "Composition Output names the final dependency explicitly");

    const auto single = compile(makeProject(singleLayerOptions()), registry);
    expectations.expect(single.status == runtime::SnapshotCompileStatus::Compiled && single.plan &&
                            single.plan->operations.size() == 4 &&
                            single.plan->output == runtime::OperationIndex::fromRaw(3),
                        "one-solid topology lowers to the minimal four-operation plan");
    if (single.plan) {
        const auto* singleStack =
            std::get_if<runtime::CompiledLayerStack>(&single.plan->operations[2]);
        expectations.expect(singleStack != nullptr && singleStack->entries.size() == 1 &&
                                singleStack->entries.front().input ==
                                    runtime::OperationIndex::fromRaw(1),
                            "one-solid stack dependency is exact and topologically prior");
    }

    document::Document revisedDocument(makeProject({.format = *format}));
    const auto base = revisedDocument.snapshot();
    auto publication = revisedDocument.commit(base.revision(), revisedDocument.draft(base));
    require(publication.committed() && publication.snapshot.has_value(),
            "revision fixture must publish");
    runtime::SnapshotCompiler compiler(registry);
    const auto revised =
        compiler.compile({*publication.snapshot, kCompositionId}, runtime::CancellationToken{});
    expectations.expect(revised.plan &&
                            revised.plan->sourceRevision == document::Revision::fromRaw(1),
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
            std::holds_alternative<runtime::CompiledSolid>(result.plan->operations.front()),
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

    auto text = makeProject(singleLayerOptions());
    auto* textComposition = text.findComposition(kCompositionId);
    auto* textNode = textComposition->graph().findNode(kFirstSolidNode);
    textNode->typeId = std::string(document::kTextSourceNodeType);
    textNode->schemaVersion = document::kTextSourceNodeSchemaVersion;
    textNode->parameters = {{std::string(document::kTextParameterRole), kFirstColor}};
    require(textComposition->parameters().erase(kFirstColor) &&
                textComposition->parameters().insert(
                    {kFirstColor, std::string(document::kTextParameterSchemaKey),
                     document::ConstantValueSource{std::string("Title")}}) &&
                text.validate().ok(),
            "recognized Text fixture must remain valid");
    const auto textResult = compile(std::move(text), registry);
    expectations.expect(textResult.status == runtime::SnapshotCompileStatus::Unsupported &&
                            hasDiagnostic(textResult,
                                          runtime::CompileDiagnosticCode::UnsupportedNode,
                                          kFirstSolidNode),
                        "recognized Text reports an unavailable capability, not an unknown node");
}

void testReachableSchemaDiagnostics(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    auto missingInputOptions = singleLayerOptions();
    missingInputOptions.omitFirstSourceEdge = true;
    const auto missingInput = compile(makeProject(std::move(missingInputOptions)), registry);
    expectations.expect(missingInput.status == runtime::SnapshotCompileStatus::Failed &&
                            hasDiagnostic(missingInput,
                                          runtime::CompileDiagnosticCode::MissingInput,
                                          kFirstLayerNode),
                        "required fixed inputs are validated by the compiler schema");

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
    const auto animatedFormat = document::CompositionFormat::create(4, 2);
    require(animatedFormat.has_value(), "animation evaluation format must be valid");
    animatedComposition->setFormat(*animatedFormat);
    auto* parameters = &animatedComposition->parameters();
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
                {{document::KeyframeId::fromRaw(104),
                  core::RationalTime::fromInteger(0),
                  {2.0, 1.0},
                  document::KeyframeInterpolation::Linear},
                 {document::KeyframeId::fromRaw(105),
                  core::RationalTime::fromInteger(1),
                  {3.0, 1.0},
                  document::KeyframeInterpolation::Linear}},
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
        const auto* layer = std::get_if<runtime::CompiledLayerOutput>(&result.plan->operations[1]);
        const auto* curveIndex =
            layer == nullptr ? nullptr
                             : std::get_if<runtime::ScalarCurveIndex>(&layer->opacity.source);
        const auto* positionCurveIndex =
            layer == nullptr ? nullptr
                             : std::get_if<runtime::Vec2CurveIndex>(&layer->position.source);
        expectations.expect(
            result.plan->scalarCurves.size() == 1 && result.plan->vec2Curves.size() == 1 &&
                result.plan->scalarCurves.front() ==
                    runtime::CompiledScalarCurve{
                        curveId,
                        {{document::KeyframeId::fromRaw(101), core::RationalTime::fromInteger(0),
                          0.2, runtime::CompiledKeyframeInterpolation::Hold},
                         {document::KeyframeId::fromRaw(102), core::RationalTime::fromInteger(1),
                          0.8, runtime::CompiledKeyframeInterpolation::Linear}}} &&
                layer != nullptr && layer->opacity.id == kFirstOpacity && curveIndex != nullptr &&
                curveIndex->value() == 0 && layer->position.id == kFirstPosition &&
                positionCurveIndex != nullptr && positionCurveIndex->value() == 0 &&
                result.plan->vec2Curves.front().id == positionCurveId &&
                result.plan->vec2Curves.front().keyframes.size() == 2,
            "compiled curve tables and parameter references preserve canonical typed identity");

        const auto halfway = core::RationalTime::create(1, 2);
        require(halfway.has_value(), "animation evaluation time must be valid");
        const runtime::CpuCompositionEvaluator evaluator;
        const auto evaluated =
            evaluator.evaluate(result.plan,
                               {.time = *halfway,
                                .output = result.plan->output,
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

    auto driven = makeProject(singleLayerOptions());
    auto& drivenParameters = driven.findComposition(kCompositionId)->parameters();
    require(
        drivenParameters.setSource(
            kFirstColor, document::DriverBindingSource{document::DriverBindingId::fromRaw(101)}) &&
            driven.validate().ok(),
        "driver reference fixture must remain valid document truth");
    const auto drivenResult = compile(std::move(driven), registry);
    expectations.expect(
        drivenResult.status == runtime::SnapshotCompileStatus::Unsupported &&
            hasDiagnostic(drivenResult, runtime::CompileDiagnosticCode::UnsupportedParameterSource,
                          kFirstSolidNode),
        "driver source stays unsupported until its Batch 4 typed output contract");
}

void testRequestScopedParameterOverrides(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const auto base = compile(makeProject(singleLayerOptions()), registry);
    const runtime::SnapshotParameterOverride firstOverride{document::Revision{}, kFirstPosition,
                                                           document::Vec2d{12.5, -4.0}};
    const auto first = compile(makeProject(singleLayerOptions()), registry, firstOverride);
    const auto second =
        compile(makeProject(singleLayerOptions()), registry,
                runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition,
                                                   document::Vec2d{12.5, -3.0}});
    const auto* layer = first.plan == nullptr
                            ? nullptr
                            : std::get_if<runtime::CompiledLayerOutput>(&first.plan->operations[1]);
    const auto* position =
        layer == nullptr ? nullptr : std::get_if<document::Vec2d>(&layer->position.source);
    expectations.expect(
        first.status == runtime::SnapshotCompileStatus::Compiled && first.plan &&
            position != nullptr && *position == document::Vec2d{12.5, -4.0} && base.plan &&
            *base.plan != *first.plan && second.plan && *first.plan != *second.plan,
        "a typed request override enters the immutable plan without document edits");

    const auto wrongRevision =
        compile(makeProject(singleLayerOptions()), registry,
                runtime::SnapshotParameterOverride{document::Revision::fromRaw(1), kFirstPosition,
                                                   document::Vec2d{0.0, 0.0}});
    expectations.expect(
        wrongRevision.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(wrongRevision, runtime::CompileDiagnosticCode::InvalidParameterOverride),
        "override admission rejects a captured revision mismatch first");

    const auto wrongKind =
        compile(makeProject(singleLayerOptions()), registry,
                runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition, 0.5});
    const auto wrongDomain =
        compile(makeProject(singleLayerOptions()), registry,
                runtime::SnapshotParameterOverride{document::Revision{}, kFirstOpacity, 1.5});
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
                runtime::SnapshotParameterOverride{document::Revision{}, unreachableParameter,
                                                   document::Vec2d{3.0, 4.0}});
    expectations.expect(
        unreachable.status == runtime::SnapshotCompileStatus::Failed &&
            hasDiagnostic(unreachable, runtime::CompileDiagnosticCode::InvalidParameterOverride),
        "override targets must participate in the requested output path");

    auto drivenProject = makeProject(singleLayerOptions());
    require(
        drivenProject.findComposition(kCompositionId)
                ->parameters()
                .setSource(kFirstPosition,
                           document::DriverBindingSource{document::DriverBindingId::fromRaw(92)}) &&
            drivenProject.validate().ok(),
        "driven override fixture must remain valid document truth");
    const auto driven =
        compile(std::move(drivenProject), registry,
                runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition,
                                                   document::Vec2d{3.0, 4.0}});
    expectations.expect(
        driven.status == runtime::SnapshotCompileStatus::Unsupported &&
            hasDiagnostic(driven, runtime::CompileDiagnosticCode::UnsupportedParameterOverride),
        "an override never hides or disconnects a driver source");

    auto animatedProject = makeProject(singleLayerOptions());
    auto* animatedComposition = animatedProject.findComposition(kCompositionId);
    constexpr auto curveId = document::AnimationCurveId::fromRaw(93);
    require(animatedComposition->animationCurves().insert(document::Vec2AnimationCurve{
                curveId,
                {{document::KeyframeId::fromRaw(94),
                  core::RationalTime::fromInteger(0),
                  {120.0, 80.0},
                  document::KeyframeInterpolation::Linear}},
            }) &&
                animatedComposition->parameters().setSource(
                    kFirstPosition, document::AnimationCurveSource{curveId}) &&
                animatedProject.validate().ok(),
            "animated override fixture must remain valid document truth");
    const auto animated =
        compile(std::move(animatedProject), registry,
                runtime::SnapshotParameterOverride{document::Revision{}, kFirstPosition,
                                                   document::Vec2d{7.0, 8.0}});
    expectations.expect(animated.status == runtime::SnapshotCompileStatus::Compiled &&
                            animated.plan && animated.plan->vec2Curves.empty(),
                        "an accepted override lowers as a constant and omits its dormant curve");
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

} // namespace

int main() {
    Expectations expectations;
    try {
        testRegistryMustBeFrozen(expectations);
        testDeterministicTypedPlan(expectations);
        testCustomSolidLoweringRemainsSupported(expectations);
        testReachabilityAndUnsupportedNodes(expectations);
        testReachableSchemaDiagnostics(expectations);
        testTypedParameterDiagnostics(expectations);
        testParameterSourcesAndDiagnosticIds(expectations);
        testRequestScopedParameterOverrides(expectations);
        testMidWorkCancellationIsBounded(expectations);
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test fixture failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
