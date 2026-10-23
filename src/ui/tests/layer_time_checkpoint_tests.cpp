// ADR 0024 checkpoint (#353): two nested-composition layers with their own start frames and frame
// counts in a 100-frame composition. Each shows its content only over its own frames; after its
// last frame nothing shows (after mode None), even when its range is extended, and every timing
// edit undoes exactly.

#include "node_editor_add.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <iostream>
#include <stdexcept>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
document::FrameRate fps24() { return *document::FrameRate::create(24, 1); }
core::RationalTime frame(const std::int64_t index) { return *core::RationalTime::create(index, 24); }
document::CompositionFormat format() {
    return *document::CompositionFormat::create(4, 4, core::PixelAspectRatio::square(), fps24());
}

struct Fixture {
    document::NewProject initial =
        document::makeNewProject("Checkpoint", "Outer", frame(100), format());
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::LayerId first, second;

    // A nested composition `frames` long holding one opaque full-frame solid of `color`.
    document::CompositionId nested(const std::string& name, const std::int64_t frames,
                                   const core::Color4d color) {
        commands::Transaction add("Add composition", document.snapshot().revision());
        add.emplace<commands::AddComposition>(name, format(), fps24(), frame(frames),
                                              core::Color4d{0, 0, 0, 0});
        const auto created = commands.execute(std::move(add));
        const auto id = created.outputId<document::CompositionId>(commands::kAddCompositionOutput);
        if (!id)
            throw std::runtime_error("nested composition");
        commands::Transaction solid("Add solid", document.snapshot().revision());
        solid.emplace<commands::AddSolidLayer>(*id, "Solid", color);
        if (!commands.execute(std::move(solid)).changed())
            throw std::runtime_error("nested solid");
        return *id;
    }
    document::LayerId place(const document::CompositionId source) {
        commands::Transaction add("Add layer", document.snapshot().revision());
        add.emplace<ui::node_editor::AddCompositionSource>(initial.initialCompositionId, source,
                                              document::Vec2d{}, true);
        const auto result = commands.execute(std::move(add));
        const auto layer = result.outputId<document::LayerId>("layer");
        if (!layer)
            throw std::runtime_error("composition layer");
        return *layer;
    }
    Fixture() {
        first = place(nested("A", 24, {1, 0, 0, 1}));
        second = place(nested("B", 60, {0, 0, 1, 1}));
    }
    bool timing(const document::LayerId layer, const std::int64_t start, const std::int64_t in,
                const std::int64_t out) {
        commands::Transaction edit("Timing", document.snapshot().revision());
        edit.emplace<commands::SetLayerTiming>(initial.initialCompositionId, layer, frame(start),
                                               frame(in), frame(out));
        return commands.execute(std::move(edit)).changed();
    }
    const document::LayerOutputBoundary& boundary(const document::LayerId layer) {
        return *document.snapshot()
                    .project()
                    .findComposition(initial.initialCompositionId)
                    ->graph()
                    .findLayer(layer);
    }
    // {red, blue} of the centre pixel at `index`.
    std::pair<float, float> sample(const std::int64_t index) {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), initial.initialCompositionId}, {});
        if (!compiled.plan)
            throw std::runtime_error("outer composition compiles");

        runtime::CpuCompositionEvaluator evaluator;
        const auto result = evaluator.evaluate(
            compiled.plan,
            {frame(index), compiled.plan->output(), runtime::CompositionFormatResolution{},
             runtime::EvaluationQuality::Reference,
             runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 20U},
            {});
        if (!result.frame())
            throw std::runtime_error("outer composition evaluates");
        const auto pixel = result.frame()->processImage().pixels()[2 * 4 + 2];
        return {pixel.red(), pixel.blue()};
    }
};

void checkpoint() {
    Fixture f;
    expect(f.boundary(f.first).endPoint(frame(100)) == frame(24) &&
               f.boundary(f.second).endPoint(frame(100)) == frame(60),
           "dropped nested compositions end on their own frame counts");
    // A (24 frames) starts at frame 10; B (60 frames) starts at 0.
    expect(f.timing(f.first, 10, 10, 34), "moving A to frame 10 applies");
    expect(f.boundary(f.first).timing.startTime == frame(10), "A starts at frame 10");
    const auto red = [&](const std::int64_t index) { return f.sample(index).first > 0.5F; };
    const auto blue = [&](const std::int64_t index) { return f.sample(index).second > 0.5F; };
    expect(!red(9) && red(10) && red(33) && !red(34), "A shows exactly frames 10-33");
    expect(blue(0) && blue(59) && !blue(60) && !blue(99), "B shows exactly frames 0-59");
    // Extending A's range past its content shows nothing there (after mode None, no hold).
    expect(f.timing(f.first, 10, 10, 50), "extending A's range applies");
    expect(red(33) && !red(34) && !red(49), "after its last frame A shows nothing");
    // Undo restores each step exactly.
    (void)f.commands.undo();
    expect(f.boundary(f.first).endPoint(frame(100)) == frame(34), "undo restores A's range");
    (void)f.commands.undo();
    expect(f.boundary(f.first).timing.startTime == frame(0) &&
               f.boundary(f.first).endPoint(frame(100)) == frame(24) && red(0) && !red(24),
           "undo restores A's start and range");
}
} // namespace

int main() {
    try {
        checkpoint();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
