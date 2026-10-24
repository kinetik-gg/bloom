// ADR 0026 on a real compiled document: Image source v2 part/layer selection, the channels output,
// Remove, Channel Copy and Shuffle compiled into one CompiledChannelShuffle and evaluated by the CPU
// reference.

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>
#include <bloom/runtime/channel_layers.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
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
bool near(const float value, const float expected, const float tolerance = 1e-4F) {
    return std::fabs(value - expected) <= tolerance;
}

constexpr int kWidth = 3;
constexpr int kHeight = 2;
constexpr std::size_t kSamples = 6;

// Part "beauty": premultiplied root RGBA (alpha 0.5), diffuse colour, depth with a NaN. Part
// "data": depth only.
void writeLayeredExr(const std::filesystem::path& path) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(kWidth - 1, kHeight - 1));
    std::vector<std::pair<std::string, std::vector<float>>> beauty;
    const auto plane = [](std::string name, auto&& sample) {
        std::vector<float> values;
        for (std::size_t index = 0; index < kSamples; ++index)
            values.push_back(sample(index));
        return std::pair{std::move(name), std::move(values)};
    };
    beauty.push_back(plane("R", [](std::size_t) { return 0.25F; }));
    beauty.push_back(plane("G", [](std::size_t) { return 0.125F; }));
    beauty.push_back(plane("B", [](std::size_t) { return 0.0F; }));
    beauty.push_back(plane("A", [](std::size_t) { return 0.5F; }));
    beauty.push_back(plane("diffuse.R", [](std::size_t i) { return 0.1F * static_cast<float>(i); }));
    beauty.push_back(plane("diffuse.G", [](std::size_t) { return 0.5F; }));
    beauty.push_back(plane("diffuse.B", [](std::size_t) { return 2.0F; }));
    beauty.push_back(plane("depth.Z", [](std::size_t i) {
        return i == 5 ? std::numeric_limits<float>::quiet_NaN()
                      : -0.5F + 0.25F * static_cast<float>(i);
    }));
    std::vector<std::pair<std::string, std::vector<float>>> data;
    data.push_back(plane("depth.Z", [](std::size_t i) { return 10.0F + static_cast<float>(i); }));

    Imf::Header first(window, window);
    first.setName("beauty");
    first.setType(Imf::SCANLINEIMAGE);
    for (const auto& [name, values] : beauty)
        first.channels().insert(name, Imf::Channel(Imf::FLOAT));
    Imf::Header second(window, window);
    second.setName("data");
    second.setType(Imf::SCANLINEIMAGE);
    for (const auto& [name, values] : data)
        second.channels().insert(name, Imf::Channel(Imf::FLOAT));
    const std::vector<Imf::Header> headers{first, second};
    Imf::MultiPartOutputFile file(path.string().c_str(), headers.data(), 2);
    for (int partIndex = 0; partIndex < 2; ++partIndex) {
        auto& planes = partIndex == 0 ? beauty : data;
        Imf::OutputPart part(file, partIndex);
        Imf::FrameBuffer frameBuffer;
        for (auto& [name, values] : planes)
            frameBuffer.insert(name, Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(values.data()),
                                                sizeof(float), kWidth * sizeof(float)));
        part.setFrameBuffer(frameBuffer);
        part.writePixels(kHeight);
    }
}

struct Fixture {
    std::filesystem::path folder;
    document::NewProject initial = document::makeNewProject(
        "Channels", "Main", core::RationalTime::fromInteger(1),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId source;
    document::NodeId layer;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Channels", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
        return result;
    }
    explicit Fixture(const std::string& name) {
        std::mt19937_64 random{std::random_device{}()};
        folder = std::filesystem::temp_directory_path() /
                 ("bloom-channel-" + name + "-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        writeLayeredExr(folder / "layers.exr");
        run<commands::ImportAssets>(std::vector{folder / "layers.exr"}, folder);
        const auto asset = document.snapshot().project().assets().front().id;
        run<commands::AddImageLayer>(composition, asset);
        const auto& graph = document.snapshot().project().findComposition(composition)->graph();
        for (const auto& node : graph.nodes()) {
            if (node.typeId == document::kImageSourceNodeType)
                source = node.id;
            if (node.typeId == document::kLayerOutputNodeType)
                layer = node.id;
        }
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    document::NodeId add(const std::string_view type) {
        return *run<commands::AddNode>(composition, std::string(type), document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    document::ParameterId parameter(const document::NodeId node, const std::string_view role) {
        const auto* record =
            document.snapshot().project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    template <typename Value>
    void set(const document::NodeId node, const std::string_view role, Value value) {
        run<commands::SetParameterSource>(
            composition, parameter(node, role),
            document::ConstantValueSource{document::ParameterValue{std::move(value)}});
    }
    void connect(const document::NodeId from, const std::string_view output,
                 const document::NodeId to, const std::string_view input) {
        run<commands::ConnectPorts>(composition, document::OutputPortRef{from, std::string(output)},
                                    document::NodeInputRef{to, std::string(input)});
    }
    std::shared_ptr<const runtime::CompiledCompositionPlan>
    compile(std::optional<runtime::ChannelInspection> inspection = std::nullopt) {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), composition, {}, std::move(inspection)}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("channel compilation failed");
        }
        return compiled.plan;
    }
    struct Frame {
        std::vector<render::Rgba32f> pixels;
        std::vector<std::string> warnings;
        [[nodiscard]] bool warned(const std::string_view text) const {
            return std::ranges::any_of(warnings, [&](const std::string& warning) {
                return warning.find(text) != std::string::npos;
            });
        }
    };
    Frame frame(std::optional<runtime::ChannelInspection> inspection = std::nullopt) {
        const auto plan = compile(std::move(inspection));
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(folder);
        const auto result = evaluator.evaluate(
            plan,
            {{}, plan->output(), runtime::CompositionFormatResolution{},
             runtime::EvaluationQuality::Reference,
             runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 24U},
            {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("channel evaluation failed");
        }
        Frame value;
        const auto span = result.frame()->processImage().pixels();
        value.pixels.assign(span.begin(), span.end());
        for (const auto& diagnostic : result.diagnostics())
            value.warnings.push_back(diagnostic.summary + " " + diagnostic.detail);
        return value;
    }
};

void imageLayerSelection() {
    Fixture fixture("layer");
    const auto root = fixture.frame();
    expect(near(root.pixels[0].red(), 0.25F) && near(root.pixels[0].alpha(), 0.5F),
           "the default selection is the first part's root RGBA");
    fixture.set(fixture.source, document::kImageLayerParameterRole, std::string("diffuse"));
    const auto diffuse = fixture.frame();
    expect(near(diffuse.pixels[2].red(), 0.2F) && near(diffuse.pixels[2].blue(), 2.0F) &&
               near(diffuse.pixels[2].alpha(), 1.0F),
           "a named colour layer decodes opaque");
    fixture.set(fixture.source, document::kImagePartParameterRole, std::string("data"));
    fixture.set(fixture.source, document::kImageLayerParameterRole, std::string("depth"));
    const auto data = fixture.frame();
    expect(near(data.pixels[0].alpha(), 0.0F) && data.warned("no colour channels"),
           "a data-only layer has no colour image and reports why");
}

void shuffleChain() {
    Fixture fixture("shuffle");
    const auto remove = fixture.add(document::kChannelRemoveNodeType);
    const auto shuffle = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, remove,
                    document::kChannelSetPort);
    fixture.connect(remove, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    fixture.connect(shuffle, document::kChannelShuffleImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    fixture.set(remove, "select", std::string("depth.Z"));
    fixture.set(shuffle, "red", std::string("diffuse.R"));
    fixture.set(shuffle, "green", std::string("diffuse.G"));
    fixture.set(shuffle, "blue", std::string("diffuse.B"));
    fixture.set(shuffle, "alpha", std::string("1"));

    const auto plan = fixture.compile();
    const auto shuffles = std::ranges::count_if(plan->operations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledChannelShuffle>(operation);
    });
    const auto images = std::ranges::count_if(plan->operations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledImageSource>(operation);
    });
    expect(shuffles == 1 && images == 0,
           "Remove and the channels output compile into one Shuffle; the unused image output "
           "compiles nothing");
    if (const auto it = std::ranges::find_if(plan->operations(),
                                             [](const auto& operation) {
                                                 return std::holds_alternative<
                                                     runtime::CompiledChannelShuffle>(operation);
                                             });
        it != plan->operations().end()) {
        const auto& compiled = std::get<runtime::CompiledChannelShuffle>(*it);
        expect(compiled.channels.sources.size() == 1 && compiled.channels.steps.size() == 2 &&
                   compiled.channels.steps[1].kind == runtime::CompiledChannelStepKind::Remove &&
                   compiled.mapping[0] == "diffuse.R",
               "the expression records the source, the Remove step and the slots");
    }
    const auto colour = fixture.frame();
    expect(near(colour.pixels[3].red(), 0.3F) && near(colour.pixels[3].green(), 0.5F) &&
               near(colour.pixels[3].blue(), 2.0F) && near(colour.pixels[3].alpha(), 1.0F),
           "a Colour shuffle of a scene-linear layer reproduces its values");

    fixture.set(shuffle, "red", std::string("depth.Z"));
    const auto removed = fixture.frame();
    expect(removed.warned("not in its input: depth.Z") && near(removed.pixels[0].alpha(), 0.0F),
           "a removed channel is a reported, transparent shuffle");

    fixture.set(remove, "select", std::string(""));
    fixture.set(shuffle, "green", std::string("depth.Z"));
    fixture.set(shuffle, "blue", std::string("0"));
    fixture.set(shuffle, "mode", document::kChannelShuffleRawMode);
    const auto raw = fixture.frame();
    expect(near(raw.pixels[0].red(), -0.5F) && near(raw.pixels[0].green(), -0.5F) &&
               near(raw.pixels[4].red(), 0.5F) && near(raw.pixels[0].blue(), 0.0F),
           "a Raw shuffle places data values in RGB unchanged, negatives included");
    expect(near(raw.pixels[5].red(), 0.0F) && raw.warned("repaired"),
           "a NaN data sample becomes zero and is reported");

    // Channel Copy brings depth from the second part into a set that removed it.
    const auto partSource = fixture.add(document::kImageSourceNodeType);
    fixture.set(partSource, "asset",
                std::to_string(fixture.document.snapshot().project().assets().front().id.value()));
    fixture.set(partSource, document::kImagePartParameterRole, std::string("data"));
    const auto copy = fixture.add(document::kChannelCopyNodeType);
    fixture.set(remove, "select", std::string("depth.Z"));
    fixture.connect(remove, document::kChannelSetPort, copy, document::kChannelSetPort);
    fixture.connect(partSource, document::kImageSourceChannelsOutputPort, copy,
                    document::kChannelCopyFromPort);
    fixture.set(copy, "select", std::string("@depth"));
    fixture.connect(copy, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    const auto copied = fixture.frame();
    expect(near(copied.pixels[0].red(), 10.0F) && near(copied.pixels[5].green(), 15.0F) &&
               !copied.warned("repaired"),
           "Channel Copy replaces the removed depth with the data part's depth");

    // Shuffle's own channel output renames into a layer that a second Shuffle reads.
    const auto rename = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(copy, document::kChannelSetPort, rename, document::kChannelSetPort);
    fixture.set(rename, "red", std::string("diffuse.B"));
    fixture.set(rename, "layer", std::string("view"));
    fixture.connect(rename, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    fixture.set(shuffle, "red", std::string("view.R"));
    fixture.set(shuffle, "green", std::string("view.A"));
    const auto renamed = fixture.frame();
    expect(near(renamed.pixels[1].red(), 2.0F) && near(renamed.pixels[1].green(), 1.0F),
           "a Shuffle channel output renames mapped channels into a layer");
}

void viewerInspection() {
    Fixture fixture("inspect");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto listing = runtime::listChannelInspectionChoices(
        compiler, fixture.document.snapshot(), fixture.composition, fixture.source,
        std::string(document::kImageSourceChannelsOutputPort), fixture.folder, {},
        runtime::EvaluationColorIntent::LinearRec709Scene, {});
    const auto has = [&](const std::string& label) {
        return std::ranges::any_of(listing.choices,
                                   [&](const auto& choice) { return choice.label == label; });
    };
    expect(listing.diagnostic.empty() && has("RGB (RGB)") && has("diffuse (RGB)") &&
               has("depth.Z") && has("A"),
           "the Viewer lists every channel and each RGB layer of a channel set");

    const auto inspection = runtime::ChannelInspection{
        fixture.source, std::string(document::kImageSourceChannelsOutputPort), {"depth.Z"}};
    const auto inspected = fixture.frame(inspection);
    expect(near(inspected.pixels[0].red(), -0.5F) && near(inspected.pixels[0].green(), -0.5F) &&
               near(inspected.pixels[0].blue(), -0.5F) && near(inspected.pixels[0].alpha(), 1.0F),
           "one inspected channel shows raw as grey, alpha 1");
    const auto layer = fixture.frame(runtime::ChannelInspection{
        fixture.source, std::string(document::kImageSourceChannelsOutputPort),
        {"diffuse.R", "diffuse.G", "diffuse.B"}});
    expect(near(layer.pixels[1].red(), 0.1F) && near(layer.pixels[1].blue(), 2.0F),
           "an inspected layer shows its three channels as RGB");
    const auto plan = fixture.compile();
    expect(std::ranges::none_of(plan->operations(),
                                [](const auto& operation) {
                                    return std::holds_alternative<runtime::CompiledChannelShuffle>(
                                        operation);
                                }) &&
               near(fixture.frame().pixels[0].red(), 0.25F),
           "inspection never changes the composition's own plan or pixels");
}

void outputChannelLayers() {
    Fixture fixture("output");
    const auto& graph = fixture.document.snapshot().project().findComposition(fixture.composition)->graph();
    const auto output = graph.compositionOutput()->nodeId;
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, output,
                    document::kCompositionOutputChannelsInputPort);
    // A project's Composition Output predates the optional layer list: the first edit binds it.
    fixture.run<commands::SetNodeParameterValue>(
        fixture.composition, output, std::string(document::kCompositionOutputLayersParameterRole),
        document::ParameterValue{std::string("depth")});
    const auto plan = fixture.compile();
    const auto created = render::ImageWindow::create(0, 0, kWidth, kHeight);
    const auto window = *created.value();
    const auto assembled = runtime::assembleOutputChannelLayers(
        *plan, {}, fixture.folder, runtime::EvaluationColorIntent::LinearRec709Scene, window,
        window, 1U << 24U, {});
    expect(assembled.ok() && assembled.layers && assembled.layers->channels().size() == 1 &&
               assembled.layers->channels()[0].name == "depth.Z" &&
               assembled.layers->channels()[0].sampleKind == render::ChannelSampleKind::Float &&
               near(assembled.layers->plane(0)[0], -0.5F) &&
               std::isnan(assembled.layers->plane(0)[5]),
           "the Composition Output's selected layers assemble as raw source channels");
    fixture.run<commands::SetNodeParameterValue>(
        fixture.composition, output, std::string(document::kCompositionOutputLayersParameterRole),
        document::ParameterValue{std::string("")});
    const auto all = runtime::assembleOutputChannelLayers(
        *fixture.compile(), {}, fixture.folder, runtime::EvaluationColorIntent::LinearRec709Scene,
        window, window, 1U << 24U, {});
    expect(all.ok() && all.layers && all.layers->channels().size() == 4 &&
               !all.layers->find("R") && all.layers->find("diffuse.G"),
           "an empty selection writes every layer except the RGBA root");
    expect(near(fixture.frame().pixels[0].red(), 0.25F),
           "channel layers never change the composited image");
}

} // namespace

int main() {
    try {
        imageLayerSelection();
        shuffleChain();
        viewerInspection();
        outputChannelLayers();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
