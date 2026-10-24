// ADR 0028 layer masks, track mattes and Mask values on a real compiled document, evaluated by the
// CPU reference. A 16x16 composition holds full-frame white solids, so a pixel's alpha reads back
// the coverage the masks and mattes leave.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
core::RationalTime at(const std::int64_t numerator, const std::int64_t denominator = 1) {
    return *core::RationalTime::create(numerator, denominator);
}
bool near(const float value, const float expected, const float tolerance = 1e-4F) {
    return std::fabs(value - expected) <= tolerance;
}

document::PathValue rectangle(const double left, const double top, const double right,
                              const double bottom) {
    document::PathValue path;
    path.closed = true;
    for (const auto& [x, y] : {std::pair{left, top}, std::pair{right, top},
                               std::pair{right, bottom}, std::pair{left, bottom}})
        path.anchors.push_back({{x, y}, std::nullopt, std::nullopt});
    return path;
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Masks", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(16, 16, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Masks", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
        return result;
    }
    document::LayerId solid(const core::Color4d color, const double width = 16,
                            const double height = 16) {
        const auto result = run<commands::AddSolidLayer>(composition, "Solid", color);
        const auto node =
            *result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto* record =
            document.snapshot().project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters) {
            if (binding.role == "width")
                setScalar(binding.parameterId, width);
            if (binding.role == "height")
                setScalar(binding.parameterId, height);
        }
        return *result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    }
    document::LayerMaskId mask(const document::LayerId layer, document::PathValue path,
                               const document::LayerMaskMode mode = document::LayerMaskMode::Add) {
        const auto result = run<commands::AddLayerMask>(composition, layer, std::move(path), mode);
        return *result.outputId<document::LayerMaskId>("mask");
    }
    const document::LayerOutputBoundary& boundary(const document::LayerId layer) const {
        return *document.snapshot()
                    .project()
                    .findComposition(composition)
                    ->graph()
                    .findLayer(layer);
    }
    void setScalar(const document::ParameterId id, const double value) {
        run<commands::SetParameterSource>(composition, id, document::ConstantValueSource{value});
    }
    std::vector<render::Rgba32f> pixels(const core::RationalTime time = {}) {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({document.snapshot(), composition}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("mask compilation failed");
        }
        runtime::CpuCompositionEvaluator evaluator;
        const auto result = evaluator.evaluate(
            compiled.plan,
            {time, compiled.plan->output(), runtime::CompositionFormatResolution{},
             runtime::EvaluationQuality::Reference,
             runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 20U},
            {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("mask evaluation failed");
        }
        const auto span = result.frame()->processImage().pixels();
        return {span.begin(), span.end()};
    }
};

float alphaAt(const std::vector<render::Rgba32f>& pixels, const int x, const int y) {
    return pixels[static_cast<std::size_t>(y * 16 + x)].alpha();
}

void rectangleMask() {
    Fixture fixture;
    const auto layer = fixture.solid({1, 1, 1, 1});
    const auto before = fixture.pixels();
    expect(near(alphaAt(before, 0, 0), 1.0F), "an unmasked solid covers the frame");
    fixture.mask(layer, rectangle(4, 4, 12, 12));
    const auto masked = fixture.pixels();
    expect(near(alphaAt(masked, 8, 8), 1.0F) && near(alphaAt(masked, 4, 4), 1.0F) &&
               near(alphaAt(masked, 11, 11), 1.0F),
           "inside a rectangle mask is covered");
    expect(near(alphaAt(masked, 3, 8), 0.0F) && near(alphaAt(masked, 12, 8), 0.0F) &&
               near(alphaAt(masked, 0, 0), 0.0F),
           "outside a rectangle mask is uncovered");
    expect(near(masked[8 * 16 + 8].red(), 1.0F), "covered colour is unchanged (premultiplied)");
}

void modesInvertExpansionFeather() {
    Fixture fixture;
    const auto layer = fixture.solid({1, 1, 1, 1});
    fixture.mask(layer, rectangle(2, 2, 14, 14));
    const auto hole =
        fixture.mask(layer, rectangle(6, 6, 10, 10), document::LayerMaskMode::Subtract);
    auto pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 3, 3), 1.0F) && near(alphaAt(pixels, 8, 8), 0.0F),
           "Subtract cuts a hole");
    fixture.run<commands::SetLayerMaskMode>(fixture.composition, layer, hole,
                                            document::LayerMaskMode::Intersect);
    pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 3, 3), 0.0F) && near(alphaAt(pixels, 8, 8), 1.0F),
           "Intersect keeps the overlap");
    fixture.run<commands::SetLayerMaskMode>(fixture.composition, layer, hole,
                                            document::LayerMaskMode::Difference);
    pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 3, 3), 1.0F) && near(alphaAt(pixels, 8, 8), 0.0F),
           "Difference is exclusive");
    fixture.run<commands::SetLayerMaskMode>(fixture.composition, layer, hole,
                                            document::LayerMaskMode::None);
    pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 8, 8), 1.0F), "None skips the mask");

    Fixture single;
    const auto only = single.solid({1, 1, 1, 1});
    const auto id = single.mask(only, rectangle(4, 4, 12, 12));
    single.run<commands::SetLayerMaskInverted>(single.composition, only, id, true);
    pixels = single.pixels();
    expect(near(alphaAt(pixels, 8, 8), 0.0F) && near(alphaAt(pixels, 0, 0), 1.0F),
           "an inverted mask covers the outside");
    single.run<commands::SetLayerMaskInverted>(single.composition, only, id, false);
    const auto& mask = single.boundary(only).masks.front();
    single.setScalar(mask.expansion, 2.0);
    pixels = single.pixels();
    expect(near(alphaAt(pixels, 2, 8), 1.0F) && near(alphaAt(pixels, 1, 8), 0.0F),
           "expansion grows the edge by its distance");
    single.setScalar(mask.expansion, 0.0);
    single.setScalar(mask.feather, 4.0);
    pixels = single.pixels();
    // Pixel centre 3.5 is 0.5 outside the edge at 4: coverage 0.5 - 0.5 / 4.
    expect(near(alphaAt(pixels, 3, 8), 0.375F) && near(alphaAt(pixels, 4, 8), 0.625F),
           "feather is a linear ramp of its width centred on the edge");
    single.setScalar(mask.feather, 0.0);
    single.setScalar(mask.opacity, 0.5);
    pixels = single.pixels();
    expect(near(alphaAt(pixels, 8, 8), 0.5F), "opacity scales coverage");
}

void animatedPath() {
    Fixture fixture;
    const auto layer = fixture.solid({1, 1, 1, 1});
    const auto id = fixture.mask(layer, rectangle(0, 0, 4, 16));
    fixture.run<commands::AnimateLayerMaskPath>(fixture.composition, layer, id, at(0));
    fixture.run<commands::SetLayerMaskPath>(fixture.composition, layer, id, at(1),
                                            rectangle(0, 0, 12, 16));
    expect(fixture.document.snapshot().project().validate().ok(), "an animated mask validates");
    const auto start = fixture.pixels(at(0));
    const auto middle = fixture.pixels(at(1, 2));
    const auto end = fixture.pixels(at(1));
    expect(near(alphaAt(start, 6, 8), 0.0F) && near(alphaAt(middle, 6, 8), 1.0F) &&
               near(alphaAt(middle, 9, 8), 0.0F) && near(alphaAt(end, 10, 8), 1.0F),
           "a keyed mask path interpolates its anchors");
}

void trackMattes() {
    Fixture fixture;
    const auto source = fixture.solid({0.25, 0.25, 0.25, 1}, 8, 8);
    const auto target = fixture.solid({1, 1, 1, 1});
    fixture.run<commands::SetTrackMatte>(
        fixture.composition, target,
        document::TrackMatte{source, document::TrackMatteChannel::Alpha, false});
    auto pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 8, 8), 1.0F) && near(alphaAt(pixels, 0, 0), 0.0F) &&
               near(pixels[8 * 16 + 8].red(), 1.0F),
           "an alpha matte keeps the target where the hidden source is opaque");
    fixture.run<commands::SetTrackMatte>(
        fixture.composition, target,
        document::TrackMatte{source, document::TrackMatteChannel::Luma, false});
    pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 8, 8), 0.25F, 1e-3F), "a luma matte reads the source luminance");
    fixture.run<commands::SetTrackMatte>(
        fixture.composition, target,
        document::TrackMatte{source, document::TrackMatteChannel::Alpha, true});
    pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 8, 8), 0.0F) && near(alphaAt(pixels, 0, 0), 1.0F),
           "an inverted matte keeps the target outside the source");
    fixture.run<commands::SetTrackMatte>(fixture.composition, target, std::nullopt);
    pixels = fixture.pixels();
    expect(near(alphaAt(pixels, 0, 0), 1.0F) && near(pixels[8 * 16 + 8].red(), 1.0F),
           "clearing the matte shows both layers again");
}

void maskNodeIntoImageMix() {
    Fixture fixture;
    const auto layer = fixture.solid({1, 0, 0, 1});
    const auto& boundary = fixture.boundary(layer);
    const auto layerNode = boundary.nodeId;
    const auto* graph =
        &fixture.document.snapshot().project().findComposition(fixture.composition)->graph();
    document::NodeId redSolid;
    for (const auto& edge : graph->edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input && input->nodeId == layerNode && input->port == "image")
            redSolid = edge.source.nodeId;
    const auto blue =
        *fixture
             .run<commands::AddNode>(fixture.composition,
                                     std::string(document::kSolidSourceNodeType), document::Vec2d{})
             .outputId<document::NodeId>(commands::kAddNodeOutput);
    const auto mix =
        *fixture.run<commands::AddNode>(fixture.composition, "bloom.image-mix", document::Vec2d{})
             .outputId<document::NodeId>(commands::kAddNodeOutput);
    const auto maskNode =
        *fixture
             .run<commands::AddNode>(fixture.composition, std::string(document::kMaskNodeType),
                                     document::Vec2d{})
             .outputId<document::NodeId>(commands::kAddNodeOutput);
    const auto parameterOf = [&](const document::NodeId node, const std::string_view role) {
        const auto* record = fixture.document.snapshot()
                                 .project()
                                 .findComposition(fixture.composition)
                                 ->graph()
                                 .findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("missing parameter");
    };
    fixture.run<commands::SetParameterSource>(
        fixture.composition, parameterOf(blue, "color"),
        document::ConstantValueSource{core::Color4d{0, 0, 1, 1}});
    fixture.run<commands::SetParameterSource>(fixture.composition, parameterOf(blue, "width"),
                                              document::ConstantValueSource{16.0});
    fixture.run<commands::SetParameterSource>(fixture.composition, parameterOf(blue, "height"),
                                              document::ConstantValueSource{16.0});
    fixture.run<commands::SetParameterSource>(
        fixture.composition, parameterOf(maskNode, "path"),
        document::ConstantValueSource{rectangle(0, 0, 8, 16)});
    fixture.run<commands::ConnectPorts>(fixture.composition,
                                        document::OutputPortRef{redSolid, "image"},
                                        document::NodeInputRef{mix, "input"});
    fixture.run<commands::ConnectPorts>(fixture.composition, document::OutputPortRef{blue, "image"},
                                        document::NodeInputRef{mix, "second"});
    fixture.run<commands::ConnectPorts>(fixture.composition,
                                        document::OutputPortRef{maskNode, "mask"},
                                        document::NodeInputRef{mix, "mask"});
    fixture.run<commands::ConnectPorts>(fixture.composition, document::OutputPortRef{mix, "image"},
                                        document::NodeInputRef{layerNode, "image"});
    const auto pixels = fixture.pixels();
    expect(near(pixels[8 * 16 + 2].blue(), 1.0F) && near(pixels[8 * 16 + 2].red(), 0.0F),
           "inside the Mask the mix takes the second image");
    expect(near(pixels[8 * 16 + 12].red(), 1.0F) && near(pixels[8 * 16 + 12].blue(), 0.0F),
           "outside the Mask the mix keeps the base image");
    bool refused = false;
    try {
        fixture.run<commands::ConnectPorts>(fixture.composition,
                                            document::OutputPortRef{maskNode, "mask"},
                                            document::NodeInputRef{mix, "second"});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    expect(refused, "a Mask cannot connect to an Image input");
}

} // namespace

int main() {
    try {
        rectangleMask();
        modesInvertExpansionFeather();
        animatedPath();
        trackMattes();
        maskNodeIntoImageMix();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "layer masks, track mattes and Mask values PASS\n";
    return failures == 0 ? 0 : 1;
}
