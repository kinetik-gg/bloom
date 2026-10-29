// ADR 0031 end to end: the scripting ops measure layers with the headless CPU reference query and
// queue one AlignLayers command; re-measuring afterwards proves the layers are visually aligned,
// including parented layers under rotated and non-uniformly scaled parents. The headless query also
// matches the GPU scene preparation's bounds (the delivered GPU-resident frame's geometry) for the
// same plan and time, and a save/reopen keeps the result.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/layer_alignment.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using scripting::StableId;
using scripting::Value;
using scripting::ValueArray;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

struct Layer final {
    document::LayerId id;
    document::ParameterId position;
    document::ParameterId scale;
    document::ParameterId rotation;
};

struct Fixture final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId composition;

    Fixture() {
        auto created = scripting::Session::createNew(
            "Align", "Main", core::RationalTime::fromInteger(10),
            document::CompositionFormat::create(1000, 500).value_or(document::CompositionFormat{}));
        if (!created)
            std::abort();
        session = std::move(created).takeSession();
        composition = session->snapshot().project().compositions().front().id();
    }

    template <typename Operation, typename... Arguments>
    scripting::SessionCommandResult run(Arguments&&... args) {
        commands::Transaction transaction("Fixture", session->snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(args)...);
        return session->execute(std::move(transaction));
    }

    // A solid of the given size whose centre is at `position` (an unparented solid's anchor is its
    // centre).
    Layer solid(const char* name, const document::Vec2d position, const document::Vec2d size) {
        const auto added =
            run<commands::AddSolidLayer>(composition, name, core::Color4d{1, 1, 1, 1}, position);
        const auto& result = *added.command;
        const Layer layer{
            result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput).value(),
            result.outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput)
                .value(),
            result.outputId<document::ParameterId>(commands::kAddSolidLayerScaleParameterOutput)
                .value(),
            result.outputId<document::ParameterId>(commands::kAddSolidLayerRotationParameterOutput)
                .value()};
        const auto solidNode =
            result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput).value();
        const auto snapshot = session->snapshot();
        const auto* node =
            snapshot.project().findComposition(composition)->graph().findNode(solidNode);
        for (const auto& binding : node->parameters) {
            if (binding.role == document::kSolidWidthParameterRole)
                expect(run<commands::SetParameterSource>(composition, binding.parameterId,
                                                         document::ConstantValueSource{size.x})
                           .succeeded(),
                       "the solid width is set");
            if (binding.role == document::kSolidHeightParameterRole)
                expect(run<commands::SetParameterSource>(composition, binding.parameterId,
                                                         document::ConstantValueSource{size.y})
                           .succeeded(),
                       "the solid height is set");
        }
        return layer;
    }

    void set(const document::ParameterId parameter, document::ParameterValue value) {
        expect(run<commands::SetParameterSource>(composition, parameter,
                                                 document::ConstantValueSource{std::move(value)})
                   .succeeded(),
               "a transform parameter is set");
    }

    [[nodiscard]] std::vector<commands::LayerAlignmentGeometry>
    measure(const core::RationalTime time = {}) const {
        auto measured = session->layerGeometryProvider()(composition, time);
        expect(measured.succeeded, "the headless measurement succeeds: " + measured.diagnostic);
        return measured.geometry;
    }

    [[nodiscard]] static const commands::AlignmentRect&
    boundsOf(const std::vector<commands::LayerAlignmentGeometry>& geometry, const Layer& layer) {
        static const commands::AlignmentRect none{};
        const auto found =
            std::ranges::find(geometry, layer.id, &commands::LayerAlignmentGeometry::layer);
        expect(found != geometry.end(), "the layer has visible bounds");
        return found == geometry.end() ? none : found->bounds;
    }

    scripting::SessionCommandResult op(const std::string& id, scripting::Arguments arguments) {
        return session->executeJsonOperation(id, arguments);
    }
};

[[nodiscard]] Value layerList(std::initializer_list<Layer> layers) {
    ValueArray entries;
    for (const auto& layer : layers)
        entries.emplace_back(StableId{layer.id.value()});
    return Value(std::move(entries));
}

void testParentedAndTransformedLayersAlignVisually() {
    Fixture fixture;
    const auto parent = fixture.solid("Parent", {300, 250}, {120, 80});
    const auto child = fixture.solid("Child", {40, -30}, {60, 40});
    const auto free = fixture.solid("Free", {800, 120}, {90, 140});
    fixture.set(parent.rotation, 30.0);
    fixture.set(parent.scale, document::Vec2d{2.0, 0.5});
    fixture.set(child.rotation, 15.0);
    fixture.set(free.rotation, -20.0);
    expect(
        fixture.run<commands::SetLayerParent>(fixture.composition, child.id, parent.id).succeeded(),
        "the child is parented to a rotated, non-uniformly scaled parent");

    struct Case final {
        const char* edge;
        const char* target;
    };
    for (const auto& [edge, target] :
         {Case{"left", "composition"}, Case{"right", "selection"},
          Case{"horizontal-centre", "key-object"}, Case{"top", "selection"},
          Case{"vertical-middle", "key-object"}, Case{"bottom", "composition"}}) {
        const std::string label = std::string(edge) + " to " + target;
        const auto before = fixture.measure();
        const auto key = Fixture::boundsOf(before, free);
        const auto selection = [&] {
            auto rect = Fixture::boundsOf(before, child);
            const auto& other = Fixture::boundsOf(before, free);
            rect.left = std::min(rect.left, other.left);
            rect.top = std::min(rect.top, other.top);
            rect.right = std::max(rect.right, other.right);
            rect.bottom = std::max(rect.bottom, other.bottom);
            return rect;
        }();
        scripting::Arguments arguments{
            {"edge", Value(edge)}, {"target", Value(target)}, {"layers", layerList({child, free})}};
        if (std::string(target) == "key-object")
            arguments.emplace("key", Value(StableId{free.id.value()}));
        const auto result = fixture.op("bloom.layer.align-layers", arguments);
        expect(result.succeeded(), label + " succeeds through the JSON operation path");
        const auto after = fixture.measure();
        const auto& moved = Fixture::boundsOf(after, child);
        const auto& other = Fixture::boundsOf(after, free);
        const std::string name(edge);
        const commands::AlignmentRect frame{0, 0, 1000, 500};
        const auto& rect = std::string(target) == "composition" ? frame
                           : std::string(target) == "selection" ? selection
                                                                : key;
        if (name == "left")
            expect(near(moved.left, rect.left) && near(other.left, rect.left), label);
        if (name == "right")
            expect(near(moved.right, rect.right) && near(other.right, rect.right), label);
        if (name == "horizontal-centre")
            expect(near(moved.centreX(), rect.centreX()) && other == key,
                   label + " (and the key stays)");
        if (name == "top")
            expect(near(moved.top, rect.top) && near(other.top, rect.top), label);
        if (name == "vertical-middle")
            expect(near(moved.centreY(), rect.centreY()) && other == key,
                   label + " (and the key stays)");
        if (name == "bottom")
            expect(near(moved.bottom, rect.bottom) && near(other.bottom, rect.bottom), label);
    }

    // The parent and its child together: the child is carried and lands on the edge as well.
    expect(fixture
               .op("bloom.layer.align-layers", {{"edge", Value("left")},
                                                {"target", Value("composition")},
                                                {"layers", layerList({parent, child})}})
               .succeeded(),
           "a parent and its child align together");
    const auto together = fixture.measure();
    expect(near(Fixture::boundsOf(together, parent).left, 0.0) &&
               near(Fixture::boundsOf(together, child).left, 0.0),
           "both the parent and its carried child reach the edge");
}

void testDistributeAndKeyedPosition() {
    Fixture fixture;
    const auto a = fixture.solid("A", {100, 100}, {50, 50});
    const auto b = fixture.solid("B", {150, 200}, {200, 30});
    const auto c = fixture.solid("C", {600, 300}, {20, 120});
    const auto d = fixture.solid("D", {900, 400}, {80, 80});
    const auto time = core::RationalTime::create(1, 2).value();
    expect(fixture.run<commands::CreateAnimationForParameter>(fixture.composition, b.position,
                                                              core::RationalTime::fromInteger(0))
                   .succeeded() &&
               fixture
                   .run<commands::SetKeyframeAtTimeForParameter>(fixture.composition, b.position,
                                                                 core::RationalTime::fromInteger(2),
                                                                 document::Vec2d{350, 200})
                   .succeeded(),
           "B's Position is animated");
    const auto result = fixture.op("bloom.layer.distribute-layers",
                                   {{"axis", Value("horizontal")},
                                    {"layers", layerList({d, b, a, c})},
                                    {"time", Value(ValueArray{Value(1), Value(2)})}});
    expect(result.succeeded() && result.command->changed(), "distribute succeeds");
    const auto after = fixture.measure(time);
    const std::array centres{
        Fixture::boundsOf(after, a).centreX(), Fixture::boundsOf(after, b).centreX(),
        Fixture::boundsOf(after, c).centreX(), Fixture::boundsOf(after, d).centreX()};
    expect(near(centres[0], 100) && near(centres[3], 900) &&
               near(centres[1] - centres[0], centres[2] - centres[1]) &&
               near(centres[2] - centres[1], centres[3] - centres[2]),
           "the evaluated centres are evenly spaced at the time");
    const auto snapshot = fixture.session->snapshot();
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto* record = composition->parameters().find(b.position);
    const auto* source = std::get_if<document::AnimationCurveSource>(&record->source);
    const auto* curve = source ? composition->animationCurves().findVec2(source->curveId) : nullptr;
    expect(curve != nullptr && curve->components[0].keyframes.size() == 3 &&
               curve->components[0].keyframes[1].time == time,
           "the animated layer gets a key at the time between its existing keys");
    expect(
        fixture.measure(core::RationalTime::fromInteger(2)).size() == 4 &&
            near(
                Fixture::boundsOf(fixture.measure(core::RationalTime::fromInteger(2)), b).centreX(),
                350),
        "the animated layer's later key is untouched");

    // Save and reopen: the aligned document is ordinary document state.
    const auto path =
        std::filesystem::temp_directory_path() /
        ("bloom-layer-alignment-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bloom");
    std::filesystem::remove(path);
    expect(fixture.session->saveAs(path).succeeded(), "the aligned project saves");
    auto reopened = scripting::Session::open(path);
    expect(static_cast<bool>(reopened), "the aligned project reopens");
    if (reopened) {
        const auto reopenedSnapshot = reopened.session()->snapshot();
        const auto* reopenedComposition =
            reopenedSnapshot.project().findComposition(fixture.composition);
        expect(reopenedComposition != nullptr &&
                   reopenedComposition->parameters().records().size() ==
                       composition->parameters().records().size() &&
                   std::ranges::equal(reopenedComposition->parameters().records(),
                                      composition->parameters().records()) &&
                   std::ranges::equal(reopenedComposition->animationCurves().records(),
                                      composition->animationCurves().records()),
               "save and reopen keep every aligned value and key");
        auto remeasured = reopened.session()->layerGeometryProvider()(fixture.composition, time);
        expect(remeasured.succeeded && remeasured.geometry == fixture.measure(time),
               "the reopened project measures the same");
    }
    std::filesystem::remove(path);
}

void testScriptingErrors() {
    Fixture fixture;
    const auto a = fixture.solid("A", {100, 100}, {50, 50});
    const auto b = fixture.solid("B", {300, 100}, {50, 50});
    const auto unknownEdge = fixture.op("bloom.layer.align-layers",
                                        {{"edge", Value("diagonal")}, {"layers", layerList({a})}});
    expect(unknownEdge.diagnostic.has_value() && unknownEdge.diagnostic->argument == "edge",
           "an unknown edge is refused naming the argument");
    const auto noKey = fixture.op(
        "bloom.layer.align-layers",
        {{"edge", Value("left")}, {"target", Value("key-object")}, {"layers", layerList({a})}});
    expect(noKey.diagnostic.has_value() && noKey.diagnostic->argument == "key",
           "key-object without a key is refused");
    const auto unknownTarget = fixture.op(
        "bloom.layer.align-layers",
        {{"edge", Value("left")}, {"target", Value("canvas")}, {"layers", layerList({a})}});
    expect(unknownTarget.diagnostic.has_value() && unknownTarget.diagnostic->argument == "target",
           "an unknown target is refused");
    const auto badAxis = fixture.op("bloom.layer.distribute-layers",
                                    {{"axis", Value("diagonal")}, {"layers", layerList({a, b})}});
    expect(badAxis.diagnostic.has_value() && badAxis.diagnostic->argument == "axis",
           "an unknown axis is refused");
    const auto noLayers = fixture.op("bloom.layer.align-layers", {{"edge", Value("left")}});
    expect(noLayers.diagnostic.has_value() && noLayers.diagnostic->argument == "layers",
           "a headless call without layers or a selection is refused");
    const auto revision = fixture.session->snapshot().revision();
    const auto tooFew = fixture.op("bloom.layer.distribute-layers",
                                   {{"axis", Value("horizontal")}, {"layers", layerList({a, b})}});
    expect(tooFew.command.has_value() &&
               tooFew.command->status == commands::CommandStatus::Rejected &&
               fixture.session->snapshot().revision() == revision,
           "distributing two layers is rejected by the command with no change");
    const auto missing =
        fixture.op("bloom.layer.align-layers", {{"edge", Value("left")},
                                                {"composition", Value(StableId{424242})},
                                                {"layers", layerList({a})}});
    expect(missing.diagnostic.has_value() &&
               missing.diagnostic->code == "bloom.scripting.layer-bounds-unavailable",
           "an unknown composition cannot be measured");
    const auto lockedOnly = [&] {
        expect(fixture.run<commands::SetLayerLocked>(fixture.composition, a.id, true).succeeded(),
               "a layer is locked");
        return fixture.op("bloom.layer.align-layers", {{"edge", Value("right")},
                                                       {"target", Value("composition")},
                                                       {"layers", layerList({a})}});
    }();
    expect(lockedOnly.command.has_value() &&
               lockedOnly.command->status == commands::CommandStatus::Rejected &&
               !lockedOnly.command->operationFailures.empty() &&
               lockedOnly.command->operationFailures.back().issue.message.find("locked") !=
                   std::string::npos,
           "aligning only a locked layer is rejected with the reason");
}

void testHeadlessBoundsMatchGpuScenePreparation() {
    Fixture fixture;
    const auto parent = fixture.solid("Parent", {300, 250}, {120, 80});
    const auto child = fixture.solid("Child", {40, -30}, {60, 40});
    fixture.set(parent.rotation, 30.0);
    fixture.set(parent.scale, document::Vec2d{2.0, 0.5});
    fixture.set(child.rotation, 15.0);
    expect(
        fixture.run<commands::SetLayerParent>(fixture.composition, child.id, parent.id).succeeded(),
        "the parity fixture is parented");
    const auto snapshot = fixture.session->snapshot();
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const runtime::CpuCompositionEvaluator evaluator;
    const auto headless = runtime::queryLayerBounds(
        compiler, evaluator,
        {.snapshot = snapshot, .composition = fixture.composition, .time = {}, .layers = {}});
    expect(headless.succeeded() && headless.bounds.size() == 2, "the headless query evaluates");
    const auto compiled =
        compiler.compile({.snapshot = snapshot, .compositionId = fixture.composition}, {});
    expect(compiled.plan != nullptr, "the plan compiles");
    if (compiled.plan == nullptr)
        return;
    const auto prepared = runtime::CpuGpuSceneBuilder{}.build(
        compiled.plan, {.time = {},
                        .output = compiled.plan->output(),
                        .resolution = runtime::CompositionFormatResolution{},
                        .pixelStorageByteLimit = runtime::kDefaultLayerBoundsPixelBudget});
    expect(prepared.hasValue(), "the GPU scene prepares on the host");
    if (!prepared)
        return;
    const auto gpu = runtime::layerOutputBounds(*compiled.plan, prepared.scene->bounds());
    expect(gpu.size() == headless.bounds.size(), "both paths report the same layers");
    for (std::size_t index = 0; index < std::min(gpu.size(), headless.bounds.size()); ++index) {
        const auto& cpu = headless.bounds[index];
        const auto& scene = gpu[index];
        bool same = cpu.layerId == scene.layerId && near(cpu.output.left, scene.output.left) &&
                    near(cpu.output.top, scene.output.top) &&
                    near(cpu.output.right, scene.output.right) &&
                    near(cpu.output.bottom, scene.output.bottom);
        for (std::size_t corner = 0; corner < 4; ++corner)
            same = same && near(cpu.polygon[corner].x, scene.polygon[corner].x) &&
                   near(cpu.polygon[corner].y, scene.polygon[corner].y);
        expect(same, "the headless bounds match the GPU scene preparation's bounds");
    }
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    const auto fromCpu = host::layerAlignmentGeometry(*composition, {}, headless.bounds);
    const auto fromScene = host::layerAlignmentGeometry(*composition, {}, gpu);
    commands::LayerAlignmentRequest request{.composition = fixture.composition,
                                            .time = {},
                                            .layers = {parent.id, child.id},
                                            .arrangement = commands::LayerArrangement::AlignLeft,
                                            .target = commands::AlignmentTarget::Composition,
                                            .keyLayer = {},
                                            .geometry = fromCpu};
    const auto cpuPlan = commands::planLayerAlignment(*composition, request);
    request.geometry = fromScene;
    const auto scenePlan = commands::planLayerAlignment(*composition, request);
    bool samePlan = cpuPlan.moves.size() == scenePlan.moves.size() && !cpuPlan.moves.empty();
    for (std::size_t index = 0; samePlan && index < cpuPlan.moves.size(); ++index)
        samePlan = near(cpuPlan.moves[index].value.x, scenePlan.moves[index].value.x) &&
                   near(cpuPlan.moves[index].value.y, scenePlan.moves[index].value.y);
    expect(samePlan, "both geometry sources plan the same move");
}

} // namespace

int main() {
    testParentedAndTransformedLayersAlignVisually();
    testDistributeAndKeyedPosition();
    testScriptingErrors();
    testHeadlessBoundsMatchGpuScenePreparation();
    if (failures != 0) {
        std::cerr << failures << " layer alignment checks failed\n";
        return 1;
    }
    return 0;
}
