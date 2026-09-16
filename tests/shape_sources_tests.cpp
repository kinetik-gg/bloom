#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace bloom;
namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
document::CompositionFormat testFormat() {
    const auto format = document::CompositionFormat::create(8, 8);
    if (!format)
        throw std::runtime_error("invalid test format");
    return *format;
}
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Shapes", "Main", core::RationalTime::fromInteger(2), testFormat());
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    std::map<std::string, document::ParameterId> parameters;
    document::NodeId node;
    runtime::CpuCompositionEvaluator evaluator;
    explicit Fixture(document::ShapeKind kind) {
        commands::Transaction add("Add shape", document.snapshot().revision());
        add.emplace<commands::AddShapeLayer>(initial.initialCompositionId, kind);
        const auto result = commands.execute(std::move(add));
        if (!result.changed())
            throw std::runtime_error("shape creation failed");
        const auto created = result.outputId<document::NodeId>("shapeNode");
        if (!created)
            throw std::runtime_error("missing shape output");
        node = *created;
        const auto snapshot = document.snapshot();
        const auto* composition = snapshot.project().findComposition(initial.initialCompositionId);
        for (const auto& binding : composition->graph().findNode(node)->parameters)
            parameters[binding.role] = binding.parameterId;
        set("size", document::Vec2d{4, 4});
        if (kind == document::ShapeKind::Line) {
            set("lineEnd", document::Vec2d{4, 0});
            set("strokeWidth", 2.0);
        }
    }
    void set(const std::string& role, document::ParameterValue value) {
        commands::Transaction edit("Edit shape", document.snapshot().revision());
        edit.emplace<commands::SetParameterSource>(initial.initialCompositionId,
                                                   parameters.at(role),
                                                   document::ConstantValueSource{std::move(value)});
        if (!commands.execute(std::move(edit)).succeeded())
            throw std::runtime_error("shape edit failed: " + role);
    }
    std::shared_ptr<const runtime::CompiledCompositionPlan> compile() {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), initial.initialCompositionId}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << '\n';
            throw std::runtime_error("shape compilation failed");
        }
        return compiled.plan;
    }
    runtime::EvaluationResult evaluate(core::RationalTime time = {},
                                       std::size_t budget = 1U << 20U) {
        const auto plan = compile();
        return evaluator.evaluate(plan,
                                  {time, plan->output(), runtime::CompositionFormatResolution{},
                                   runtime::EvaluationQuality::Reference,
                                   runtime::EvaluationColorIntent::LinearRec709Scene, budget},
                                  {});
    }
};
float alpha(const runtime::EvaluationResult& result, int x, int y) {
    if (!result.frame()) {
        for (const auto& diagnostic : result.diagnostics())
            std::cerr << diagnostic.summary << '\n';
        ++failures;
        return -1;
    }
    const auto pixel = result.frame()->processImage().read(x, y);
    return pixel ? pixel.value()->alpha() : 0;
}
void pixelsAndBounds() {
    Fixture rectangle(document::ShapeKind::Rectangle);
    const auto rect = rectangle.evaluate();
    expect(alpha(rect, 2, 2) == 1 && alpha(rect, 1, 2) == 0, "rectangle pixel pin");
    expect(rect.frame() &&
               rect.frame()->evaluatedBounds()[0].local == runtime::ContentBounds{0, 0, 4, 4},
           "content-sized shape bounds");
    const auto cached = rectangle.evaluate();
    expect(alpha(cached, 2, 2) == 1 && cached.frame()->operationCacheStatistics().hits > 0,
           "cached shape equals uncached pixels");
    rectangle.set("fillColor", core::Color4d{1, 0, 0, 0.5});
    rectangle.set("strokeEnabled", true);
    rectangle.set("strokeWidth", 2.0);
    rectangle.set("strokeAlign", std::int64_t{1});
    rectangle.set("strokeColor", core::Color4d{0, 0, 1, 0.5});
    const auto layered = rectangle.evaluate();
    const auto pixel = layered.frame()->processImage().read(2, 2);
    expect(pixel && pixel.value()->alpha() == 0.75F && pixel.value()->red() == 0.25F &&
               pixel.value()->blue() == 0.5F,
           "stroke is premultiplied source-over on fill");
    Fixture circle(document::ShapeKind::Ellipse);
    const auto round = circle.evaluate();
    expect(std::abs(alpha(round, 2, 2) - 96.0F / 255.0F) < 1e-6F && alpha(round, 3, 3) == 1,
           "circle coverage pixel pin");
    Fixture star(document::ShapeKind::Star);
    const auto pointed = star.evaluate();
    expect(alpha(pointed, 2, 2) == 0 && std::abs(alpha(pointed, 3, 2) - 64.0F / 255.0F) < 1e-6F &&
               alpha(pointed, 3, 4) == 1,
           "five-point star pixel pins");
    Fixture line(document::ShapeKind::Line);
    const auto stroked = line.evaluate();
    expect(alpha(stroked, 2, 3) == 1 && alpha(stroked, 2, 2) == 0, "stroked line pixel pins");
    Fixture path(document::ShapeKind::Path);
    document::PathValue twice;
    twice.closed = true;
    for (int loop = 0; loop < 2; ++loop)
        for (auto p : {document::Vec2d{0, 0}, document::Vec2d{4, 0}, document::Vec2d{4, 4},
                       document::Vec2d{0, 4}})
            twice.anchors.push_back({p, {}, {}});
    path.set("path", twice);
    expect(alpha(path.evaluate(), 3, 3) == 1, "nonzero path pixel pin");
    path.set("fillRule", std::int64_t{1});
    expect(alpha(path.evaluate(), 3, 3) == 0, "evenodd path pixel pin and cache invalidation");
    expect(rectangle.evaluate({}, 1).status() == runtime::EvaluationStatus::Failed,
           "shape budget refusal");
}
void animation() {
    Fixture shape(document::ShapeKind::Rectangle);
    auto plan = shape.compile();
    expect(!plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "static shape is time independent");
    commands::Transaction animate("Animate shape", shape.document.snapshot().revision());
    animate.emplace<commands::CreateAnimationForParameter>(
        shape.initial.initialCompositionId, shape.parameters.at("size"), core::RationalTime{});
    animate.emplace<commands::SetKeyframeAtTimeForParameter>(
        shape.initial.initialCompositionId, shape.parameters.at("size"),
        core::RationalTime::fromInteger(1), document::Vec2d{6, 6});
    expect(shape.commands.execute(std::move(animate)).changed(),
           "shape size animates through normal commands");
    plan = shape.compile();
    expect(plan->operationTimeDependent(runtime::OperationIndex::fromRaw(0)),
           "animated shape is time dependent");
    expect(alpha(shape.evaluate(), 1, 1) == 0 &&
               alpha(shape.evaluate(core::RationalTime::fromInteger(1)), 1, 1) == 1,
           "animated size changes pixels");
}
} // namespace
int main() {
    try {
        pixelsAndBounds();
        animation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures ? 1 : 0;
}
