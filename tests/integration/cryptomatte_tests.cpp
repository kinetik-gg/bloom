// #364 Cryptomatte on a real compiled document: a multi-rank Cryptomatte EXR generated in-test,
// with and without a manifest, evaluated by the CPU reference through the Mask preview, the Image
// output with and without an input image, and a muted node.

#include "cryptomatte_fixture_support.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bloom;
namespace fixture = bloom::cryptomatte_fixture;
namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Fixture {
    std::filesystem::path folder;
    document::NewProject initial = document::makeNewProject(
        "Cryptomatte", "Main", core::RationalTime::fromInteger(1),
        *document::CompositionFormat::create(fixture::kWidth, fixture::kHeight,
                                             core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId source;
    document::NodeId layer;
    document::NodeId cryptomatte;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Cryptomatte", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
        return result;
    }
    Fixture(const std::string& name, const bool manifest, const std::string& manifestText = {}) {
        std::mt19937_64 random{std::random_device{}()};
        folder = std::filesystem::temp_directory_path() /
                 ("bloom-cryptomatte-" + name + "-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        fixture::writeCryptomatteExr(folder / "crypto.exr", manifest, "CryptoObject", manifestText);
        run<commands::ImportAssets>(std::vector{folder / "crypto.exr"}, folder);
        run<commands::AddImageLayer>(composition,
                                     document.snapshot().project().assets().front().id);
        for (const auto& node : graph().nodes()) {
            if (node.typeId == document::kImageSourceNodeType)
                source = node.id;
            if (node.typeId == document::kLayerOutputNodeType)
                layer = node.id;
        }
        cryptomatte =
            *run<commands::AddNode>(composition, std::string(document::kCryptomatteNodeType),
                                    document::Vec2d{})
                 .outputId<document::NodeId>(commands::kAddNodeOutput);
        connect(source, document::kImageSourceChannelsOutputPort, cryptomatte,
                document::kChannelSetPort);
        connect(cryptomatte, document::kCryptomatteImageOutputPort, layer,
                document::kLayerOutputContentInputPort);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    const document::CanonicalGraph& graph() const {
        return document.snapshot().project().findComposition(composition)->graph();
    }
    void connect(const document::NodeId from, const std::string_view output,
                 const document::NodeId to, const std::string_view input) {
        run<commands::ConnectPorts>(composition, document::OutputPortRef{from, std::string(output)},
                                    document::NodeInputRef{to, std::string(input)});
    }
    void setMattes(const std::vector<std::string>& mattes) {
        run<commands::SetNodeParameterValue>(
            composition, cryptomatte, std::string(document::kCryptomatteMattesParameterRole),
            document::ParameterValue{runtime::joinCryptomatteMatteList(mattes)});
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
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), composition, {}, std::move(inspection)}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("cryptomatte compilation failed");
        }
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(folder);
        const auto result = evaluator.evaluate(compiled.plan,
                                               {{},
                                                compiled.plan->output(),
                                                runtime::CompositionFormatResolution{},
                                                runtime::EvaluationQuality::Reference,
                                                runtime::EvaluationColorIntent::LinearRec709Scene,
                                                1U << 24U},
                                               {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("cryptomatte evaluation failed");
        }
        Frame value;
        const auto span = result.frame()->processImage().pixels();
        value.pixels.assign(span.begin(), span.end());
        for (const auto& diagnostic : result.diagnostics())
            value.warnings.push_back(diagnostic.summary + " " + diagnostic.detail);
        return value;
    }
    Frame matte() {
        return frame(runtime::ChannelInspection{
            cryptomatte, std::string(document::kCryptomatteMaskOutputPort), {}});
    }
};

[[nodiscard]] bool matches(const Fixture::Frame& frame, const std::vector<int>& selected) {
    for (std::size_t pixel = 0; pixel < frame.pixels.size(); ++pixel) {
        const auto expected = fixture::expectedCoverage(pixel, selected);
        const auto& value = frame.pixels[pixel];
        if (std::bit_cast<std::uint32_t>(value.alpha()) != std::bit_cast<std::uint32_t>(expected) ||
            value.red() != value.alpha() || value.green() != value.alpha()) {
            std::cerr << "pixel " << pixel << " alpha " << value.alpha() << " expected " << expected
                      << '\n';
            return false;
        }
    }
    return true;
}

void hashing() {
    expect(runtime::cryptomatteNameId("hello") == 0x248bfa47U,
           "MurmurHash3_x86_32(\"hello\", 0) is the reference vector");
    expect(runtime::cryptomatteNameId("The quick brown fox jumps over the lazy dog") == 0x2e4ff723U,
           "MurmurHash3_x86_32 reference vector for a 43-byte name");
    expect(runtime::cryptomatteNameId("") == 0x00800000U,
           "a zero hash has its exponent bit flipped to stay a normal float");
    const std::vector<std::string> mattes{"a,b", "plain", "back\\slash"};
    expect(runtime::splitCryptomatteMatteList(runtime::joinCryptomatteMatteList(mattes)) == mattes,
           "the matte list round-trips escaped commas and backslashes");
    const auto id = runtime::cryptomatteNameId("cube");
    expect(runtime::parseCryptomatteRawId(runtime::formatCryptomatteRawId(id)) == id &&
               runtime::parseCryptomatteRawId("<0x3f800000>") == 0x3f800000U,
           "raw IDs round-trip bit for bit");
}

void withManifest() {
    Fixture f("manifest", true);
    f.setMattes({"cube"});
    expect(matches(f.matte(), {1}), "a manifest name selects its object, summed over ranks");
    f.setMattes({"sphere", "floor"});
    expect(matches(f.matte(), {0, 2}), "several mattes add their coverage in rank order");
    expect(matches(f.frame(), {0, 2}),
           "with no input image the Image output equals the Mask as a white image");
    f.setMattes({runtime::formatCryptomatteRawId(runtime::cryptomatteNameId("sky"))});
    expect(matches(f.matte(), {3}), "a raw ID entry selects its object");
    f.setMattes({"teapot"});
    expect(matches(f.matte(), {}), "an absent object selects nothing");
}

void withoutManifest() {
    Fixture f("bare", false);
    f.setMattes({"cube", "sky"});
    expect(matches(f.matte(), {1, 3}), "without a manifest names resolve through their hash");
    // Without an input image the Image output is the matte as a white image.
    const auto& edges = f.graph().edges();
    expect(std::ranges::none_of(edges,
                                [&](const auto& edge) {
                                    const auto* input =
                                        std::get_if<document::NodeInputRef>(&edge.destination);
                                    return input && input->nodeId == f.cryptomatte &&
                                           input->port == document::kCryptomatteImageInputPort;
                                }),
           "the fixture leaves the Cryptomatte image input open");
    expect(matches(f.frame(), {1, 3}),
           "without an input image the Image output is the white matte");
}

void malformedManifest() {
    Fixture f("malformed", true, "{\"cube\": 12");
    f.setMattes({"cube"});
    const auto frame = f.matte();
    expect(matches(frame, {1}), "with a malformed manifest names still resolve through their hash");
    expect(frame.warned("manifest is not a valid JSON object"), "a malformed manifest is reported");
}

void imageInputAndMute() {
    Fixture f("mute", true);
    f.connect(f.source, document::kImageSourceImageOutputPort, f.cryptomatte,
              document::kCryptomatteImageInputPort);
    f.setMattes({"cube"});
    const auto isolated = f.frame();
    expect(std::abs(isolated.pixels[6].red() - 0.7F) < 1e-6F &&
               std::abs(isolated.pixels[2].green() - 0.25F) < 1e-6F &&
               std::abs(isolated.pixels[1].red() - 0.2F * 0.4F) < 1e-6F &&
               std::abs(isolated.pixels[1].alpha() - 0.4F) < 1e-6F &&
               isolated.pixels[0].alpha() == 0.0F,
           "an input image is multiplied by the matte (matte in alpha)");
    f.run<commands::SetNodeMuted>(f.composition, f.cryptomatte, true);
    const auto muted = f.frame();
    expect(std::abs(muted.pixels[0].red() - 0.1F) < 1e-6F && muted.pixels[0].alpha() == 1.0F,
           "a muted Cryptomatte passes its image through");
    f.run<commands::SetNodeMuted>(f.composition, f.cryptomatte, false);
    f.run<commands::SetNodeParameterValue>(f.composition, f.cryptomatte,
                                           std::string(document::kCryptomatteLayerParameterRole),
                                           document::ParameterValue{std::string("CryptoMaterial")});
    const auto missing = f.frame();
    expect(missing.warned("no Cryptomatte layer named CryptoMaterial") &&
               missing.pixels[0].alpha() == 0.0F,
           "a missing Cryptomatte layer is reported and isolates nothing");
}

} // namespace

int main() {
    try {
        hashing();
        withManifest();
        withoutManifest();
        imageInputAndMute();
        malformedManifest();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
