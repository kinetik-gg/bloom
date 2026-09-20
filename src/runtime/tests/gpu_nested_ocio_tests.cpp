// Focused proof for the CompiledCompositionSource splice over a real OCIO ProcessEffect command.
//
// The production scene builder does not yet emit CompiledImageEffect (the colour builder lane owns
// that), so this fixture constructs an IMMUTABLE child PreparedGpuScene containing a genuine
// GpuSceneOcioEffectCommand prepared by the real GpuOcioProgramPreparer, then splices it through
// the production nested helper exactly as the builder does: child commands are appended at a
// constant base, every internal command reference is remapped, the immutable OCIO program
// identity/metadata is copied unchanged, and the child's resident bytes are charged to the parent
// allowance. The effect output is never CPU-uploaded: the splice carries the real command, and the
// executor runs it natively against the unchanged CPU OCIO oracle (readback only for the oracle).
//
// Without a device the test reports an explicit SKIP (exit 77); --require-device hard-fails.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include "gpu_scene_nested.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {

// Proof-only fixture seam (declared as a friend in prepared_gpu_scene.hpp). Defined only here so no
// production mutable scene-creation API is exposed. It carries real bounds so the nested helper can
// read the child's output bounds.
struct GpuSceneFixtureBuilder final {
    [[nodiscard]] static std::shared_ptr<const PreparedGpuScene>
    make(std::vector<GpuSceneCommand> commands, const GpuSceneCommandIndex output,
         std::vector<EvaluatedOperationBounds> bounds,
         const render::Rgba32fImageDescriptor outputDescriptor) {
        ProcessFrameIdentity identity{
            .plan = nullptr,
            .time = {},
            .output = OperationIndex::fromRaw(0),
            .resolution = {},
            .quality = EvaluationQuality::Reference,
            .colorIntent = EvaluationColorIntent::LinearRec709Scene,
            .provider = EvaluationProvider::CpuReference,
            .evaluatorSemanticsVersion = 0,
            .animationSamplingSemanticsVersion = 0,
            .imagePrimitiveSemanticsVersion = 0,
            .roi = std::nullopt,
            .bypassLookNodes = false,
        };
        return std::shared_ptr<const PreparedGpuScene>(
            new PreparedGpuScene(std::move(commands), {}, output, std::move(identity),
                                 std::move(bounds), outputDescriptor, {}));
    }
};

} // namespace bloom::runtime

namespace {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::core::RationalTime;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::CompiledColorParameter;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledCompositionSource;
using bloom::runtime::CompiledCompositionTimeMapping;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCommand;
using bloom::runtime::GpuSceneCommandIndex;
using bloom::runtime::GpuSceneCompositionOutputCommand;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorJobState;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneFixtureBuilder;
using bloom::runtime::GpuSceneMergeCommand;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::GpuSceneSolidCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::kInvalidGpuSceneCommand;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreparedGpuOcioCommand;
using bloom::runtime::PreparedGpuScene;

constexpr int kSkipExit = 77;

[[nodiscard]] bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
constexpr std::uint64_t kRevision = 7;
constexpr auto kProject = bloom::document::ProjectId::fromRaw(1);
constexpr std::uint32_t kWidth = 4;
constexpr std::uint32_t kHeight = 3;

// Every helper and test below drives real OCIO shader preparation from
// BLOOM_GPUSHADER_TOOLS_DIR, so this whole section is compiled only when the shader tools are
// packaged. Without them main() reports an honest SKIP (exit 77); there is no silent pass.

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] ImageWindow window(const std::int64_t x, const std::int64_t y,
                                 const std::uint64_t width, const std::uint64_t height) {
    const auto result = ImageWindow::create(x, y, width, height);
    return *result.value();
}

[[nodiscard]] Rgba32f pixel(const float r, const float g, const float b, const float a) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, a);
    return *result.value();
}

[[nodiscard]] std::optional<Rgba32fImage> makeImage(const ImageWindow dataWindow,
                                                    const ImageWindow displayWindow,
                                                    const PixelAspectRatio aspect,
                                                    const std::vector<Rgba32f>& pixels) {
    const auto descriptor = Rgba32fImageDescriptor::create(dataWindow, displayWindow, aspect);
    if (!descriptor || pixels.size() != descriptor.value()->layout().pixelCount) {
        return std::nullopt;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
    if (!builder) {
        return std::nullopt;
    }
    for (std::uint32_t y = 0; y < dataWindow.extent().height(); ++y) {
        const auto row = builder.value()->row(dataWindow.originY() + y);
        if (!row) {
            return std::nullopt;
        }
        for (std::uint32_t x = 0; x < dataWindow.extent().width(); ++x) {
            (*row.value())[x] =
                pixels[static_cast<std::size_t>(y) * dataWindow.extent().width() + x];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    return frozen ? std::optional(std::move(*frozen.value())) : std::nullopt;
}

[[nodiscard]] std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
                                                 const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float alpha = ((x + y) % 3 == 0) ? 0.0F : 0.25F + 0.5F * fx;
            pixels[static_cast<std::size_t>(y) * width + x] = pixel(
                (1.6F + fx) * alpha, (-0.2F + fy) * alpha, (0.125F + 2.0F * fx) * alpha, alpha);
        }
    }
    return pixels;
}

[[nodiscard]] bool close(const float a, const float b) {
    if (a == b) {
        return true;
    }
    const double absolute = std::fabs(static_cast<double>(a) - static_cast<double>(b));
    const double magnitude =
        std::max(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)));
    return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
}

[[nodiscard]] GpuOcioCompileOptions compileOptions() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    return options;
}

[[nodiscard]] bloom::document::CompositionFormat format(const std::uint32_t width,
                                                        const std::uint32_t height) {
    const auto value = bloom::document::CompositionFormat::create(width, height);
    if (!value.has_value()) {
        throw std::logic_error("invalid nested-OCIO fixture composition format");
    }
    return *value;
}

// A minimal, valid child plan whose only role is metadata for the nested reference and its output
// index. The child SCENE is the hand-built immutable scene below; the plan is not compiled from it.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan> childPlan() {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        CompiledSolid{bloom::document::NodeId::fromRaw(10),
                      {bloom::document::ParameterId::fromRaw(11), Color4d{0.0, 0.0, 0.0, 1.0}},
                      {bloom::document::ParameterId::fromRaw(12), 1.0},
                      {bloom::document::ParameterId::fromRaw(13), 1.0}});
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(20),
                      std::vector<CompiledMergeInput>{CompiledMergeInput{
                          bloom::document::LayerSlotId::fromRaw(21), bloom::document::LayerId{},
                          OperationIndex::fromRaw(0)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(22),
                                                      OperationIndex::fromRaw(1)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 bloom::document::CompositionId::fromRaw(2),
                                                 format(kWidth, kHeight),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(2)};
    definition.duration = RationalTime::fromInteger(100);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
parentPlan(const std::shared_ptr<const CompiledCompositionPlan>& child) {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledCompositionSource{
        bloom::document::NodeId::fromRaw(30), 0,
        CompiledCompositionTimeMapping{{bloom::document::ParameterId::fromRaw(31), 0.0},
                                       {bloom::document::ParameterId::fromRaw(32), 1.0},
                                       0}});
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(40),
                      std::vector<CompiledMergeInput>{CompiledMergeInput{
                          bloom::document::LayerSlotId::fromRaw(41), bloom::document::LayerId{},
                          OperationIndex::fromRaw(0)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(42),
                                                      OperationIndex::fromRaw(1)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 bloom::document::CompositionId::fromRaw(1),
                                                 format(kWidth, kHeight),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(2)};
    definition.duration = RationalTime::fromInteger(100);
    definition.nestedPlans.push_back(child);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

struct Fixture final {
    ImageWindow data = window(0, 0, kWidth, kHeight);
    ImageWindow display = window(0, 0, kWidth, kHeight);
    PixelAspectRatio aspect = PixelAspectRatio::square();
    std::vector<Rgba32f> uploadPixels;
    Rgba32f solidPixel = pixel(0.1F, 0.2F, 0.05F, 0.5F);
    std::shared_ptr<const Rgba32fImage> uploadImage;
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    std::string uploadKey = "upload-nested-ocio-v1";
    std::string ocioKey;
};

// The immutable child scene: upload -> OCIO effect -> composition output. Its output index (2)
// matches the child plan's output so the nested helper can read its bounds.
[[nodiscard]] std::shared_ptr<const PreparedGpuScene> buildChildScene(const Fixture& fixture) {
    std::vector<GpuSceneCommand> commands;
    commands.push_back(GpuSceneUploadCommand{.index = 0,
                                             .sourceOperation = OperationIndex::fromRaw(0),
                                             .image = fixture.uploadImage,
                                             .descriptor = *fixture.uploadImage->descriptor(),
                                             .semanticKey = fixture.uploadKey});
    commands.push_back(GpuSceneOcioEffectCommand{.index = 1,
                                                 .sourceOperation = OperationIndex::fromRaw(0),
                                                 .input = 0,
                                                 .inputKey = fixture.uploadKey,
                                                 .program = fixture.program,
                                                 .outputWindow = fixture.data,
                                                 .displayWindow = fixture.display,
                                                 .pixelAspect = fixture.aspect,
                                                 .semanticKey = fixture.ocioKey});
    commands.push_back(
        GpuSceneCompositionOutputCommand{.index = 2,
                                         .sourceOperation = OperationIndex::fromRaw(0),
                                         .input = 1,
                                         .dataWindow = fixture.data,
                                         .displayWindow = fixture.display,
                                         .pixelAspect = fixture.aspect,
                                         .semanticKey = "child-output-nested-ocio-v1"});
    std::vector<bloom::runtime::EvaluatedOperationBounds> bounds(3);
    const auto descriptor =
        Rgba32fImageDescriptor::create(fixture.data, fixture.display, fixture.aspect);
    return GpuSceneFixtureBuilder::make(std::move(commands), 2, std::move(bounds),
                                        *descriptor.value());
}

[[nodiscard]] std::uint64_t windowBytes(const ImageWindow window) {
    return static_cast<std::uint64_t>(window.extent().width()) * window.extent().height() *
           sizeof(Rgba32f);
}

// The CPU oracle: the real CPU OCIO CST transform of the upload, source-over the backdrop solid.
[[nodiscard]] std::vector<Rgba32f> cpuOracle(const Fixture& fixture,
                                             const bloom::color::ResolvedBloomNeutralConfig& aces) {
    const auto cpu = bloom::color::CpuColorSpaceProcessor::prepare(aces, "ACES2065-1", "ACEScg");
    std::vector<Rgba32f> expected(fixture.uploadPixels.size(), Rgba32f::transparent());
    for (std::size_t index = 0; index < fixture.uploadPixels.size(); ++index) {
        const auto& source = fixture.uploadPixels[index];
        std::array<float, 4> straight{0.0F, 0.0F, 0.0F, 1.0F};
        if (source.alpha() != 0.0F) {
            straight = {source.red() / source.alpha(), source.green() / source.alpha(),
                        source.blue() / source.alpha(), 1.0F};
        }
        std::span<std::array<float, 4>> span(&straight, 1);
        if (cpu && cpu.processor() != nullptr) {
            static_cast<void>(cpu.processor()->apply(span));
        }
        const Rgba32f ocio = pixel(straight[0] * source.alpha(), straight[1] * source.alpha(),
                                   straight[2] * source.alpha(), source.alpha());
        Rgba32f accumulator = Rgba32f::transparent();
        std::span<Rgba32f> destination(&accumulator, 1);
        const std::array<Rgba32f, 1> solid{fixture.solidPixel};
        static_cast<void>(bloom::render::sourceOverLinearRec709SceneRow(solid, destination));
        const std::array<Rgba32f, 1> ocioRow{ocio};
        static_cast<void>(bloom::render::sourceOverLinearRec709SceneRow(ocioRow, destination));
        expected[index] = accumulator;
    }
    return expected;
}

// Splices the real child OCIO scene through the production nested helper with a non-zero base (the
// backdrop solid is already at index 0), then appends the parent merge/output. Returns the spliced
// command list and the parent scene, or nullopt on a helper failure.
[[nodiscard]] std::optional<std::shared_ptr<const PreparedGpuScene>>
spliceParentScene(const Fixture& fixture, const CompiledCompositionSource& source,
                  const CompiledCompositionPlan& parent,
                  const std::shared_ptr<const PreparedGpuScene>& childScene,
                  std::uint64_t& chargedBytes, std::uint64_t allowance,
                  bloom::runtime::PreparedGpuSceneDiagnosticCode& failure) {
    std::vector<GpuSceneCommand> commands;
    commands.push_back(GpuSceneSolidCommand{.index = 0,
                                            .sourceOperation = OperationIndex::fromRaw(0),
                                            .pixel = fixture.solidPixel,
                                            .dataWindow = fixture.data,
                                            .displayWindow = fixture.display,
                                            .pixelAspect = fixture.aspect,
                                            .semanticKey = "parent-backdrop-nested-ocio-v1"});
    const auto parentDescriptor =
        Rgba32fImageDescriptor::create(fixture.data, fixture.display, fixture.aspect);
    if (!parentDescriptor) {
        failure = bloom::runtime::PreparedGpuSceneDiagnosticCode::InternalInvariant;
        return std::nullopt;
    }
    EvaluationRequest request{.time = RationalTime{},
                              .output = OperationIndex::fromRaw(0),
                              .resolution = bloom::runtime::CompositionFormatResolution{},
                              .pixelStorageByteLimit = allowance};
    bloom::runtime::detail::ResolvedEvaluation resolved{.imageDescriptor =
                                                            *parentDescriptor.value(),
                                                        .horizontalScale = 1.0,
                                                        .verticalScale = 1.0,
                                                        .imageBytes = 0,
                                                        .remainingConsumers = {},
                                                        .scalarCurveValues = {},
                                                        .vec2CurveValues = {},
                                                        .color4CurveValues = {},
                                                        .valueOutputs = {}};
    bloom::runtime::detail::GpuSceneNestedResult nested;
    const auto error = bloom::runtime::detail::prepareNestedComposition(
        source, parent, request, resolved, 1.0, 1.0, 0, allowance, chargedBytes,
        bloom::runtime::CancellationToken{},
        [&childScene](const std::shared_ptr<const CompiledCompositionPlan>&,
                      const EvaluationRequest&, const bloom::runtime::CancellationToken&) {
            return bloom::runtime::PreparedGpuSceneBuildResult{childScene, {}};
        },
        commands, nested);
    if (error) {
        failure = error->code;
        return std::nullopt;
    }
    const auto mergeIndex = static_cast<GpuSceneCommandIndex>(commands.size());
    commands.push_back(GpuSceneMergeCommand{.index = mergeIndex,
                                            .sourceOperation = OperationIndex::fromRaw(0),
                                            .foregrounds = {0, nested.outputCommand},
                                            .outputWindow = fixture.data,
                                            .displayWindow = fixture.display,
                                            .pixelAspect = fixture.aspect,
                                            .semanticKey = "parent-merge-nested-ocio-v1"});
    const auto outputIndex = static_cast<GpuSceneCommandIndex>(commands.size());
    commands.push_back(
        GpuSceneCompositionOutputCommand{.index = outputIndex,
                                         .sourceOperation = OperationIndex::fromRaw(0),
                                         .input = mergeIndex,
                                         .dataWindow = fixture.data,
                                         .displayWindow = fixture.display,
                                         .pixelAspect = fixture.aspect,
                                         .semanticKey = "parent-output-nested-ocio-v1"});
    std::vector<bloom::runtime::EvaluatedOperationBounds> bounds(commands.size());
    const auto descriptor =
        Rgba32fImageDescriptor::create(fixture.data, fixture.display, fixture.aspect);
    return GpuSceneFixtureBuilder::make(std::move(commands), outputIndex, std::move(bounds),
                                        *descriptor.value());
}

void testSpliceAndNative(Expectations& expectations, GpuDevice* device,
                         const bloom::color::ResolvedBloomNeutralConfig& aces,
                         const GpuOcioCompileOptions& options) {
    Fixture fixture;
    fixture.uploadPixels = fixturePixels(kWidth, kHeight);
    auto built = makeImage(fixture.data, fixture.display, fixture.aspect, fixture.uploadPixels);
    expectations.expect(built.has_value(), "the upload fixture image builds");
    if (!built.has_value()) {
        return;
    }
    fixture.uploadImage = std::make_shared<const Rgba32fImage>(std::move(*built));

    GpuOcioProgramPreparer preparer;
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = "ACES2065-1";
    spec.toId = "ACEScg";
    const auto prepared =
        preparer.prepare(aces, spec, GpuOcioCommandGeometry{kWidth, kHeight}, options);
    expectations.expect(prepared.hasValue(), "the OCIO CST command prepares off-device");
    if (!prepared) {
        return;
    }
    expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::FinalRgba32f,
                        "the nested OCIO command is a ProcessEffect");
    fixture.program = prepared.command;
    fixture.ocioKey = bloom::runtime::makeGpuSceneOcioEffectSemanticKey(
        fixture.uploadKey, fixture.program->identity(), fixture.data, fixture.aspect);

    const auto childScene = buildChildScene(fixture);
    const auto child = childPlan();
    const auto parent = parentPlan(child);
    const auto* source = std::get_if<CompiledCompositionSource>(&parent->operations()[0]);
    expectations.expect(source != nullptr, "the parent plan has a composition source");
    if (source == nullptr) {
        return;
    }

    // CPU splice proof: non-zero base, remapped OCIO input, unchanged program identity/metadata,
    // and exact HOST-RETAINED byte accounting. Scene preparation retains only host allocations (the
    // frozen upload image); the OCIO effect output and the child composition output are
    // GPU-transient, allocated at executor time under the executor's live-pin budget, and are
    // deliberately NOT charged to the preparation allowance.
    std::uint64_t chargedBytes = 0;
    bloom::runtime::PreparedGpuSceneDiagnosticCode failure =
        bloom::runtime::PreparedGpuSceneDiagnosticCode::None;
    const auto spliced =
        spliceParentScene(fixture, *source, *parent, childScene, chargedBytes, kBudget, failure);
    expectations.expect(spliced.has_value(), "the nested OCIO child scene splices");
    if (!spliced.has_value()) {
        return;
    }
    const auto& commands = (*spliced)->commands();
    // The child commands start at base 1 (the backdrop solid is at 0): upload 1, OCIO 2, output 3.
    const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&commands[2]);
    expectations.expect(ocio != nullptr, "the spliced OCIO command is present at base+1");
    if (ocio != nullptr) {
        expectations.expect(ocio->input == 1,
                            "the OCIO input dependency is remapped by the splice base");
        expectations.expect(std::get_if<GpuSceneUploadCommand>(&commands[ocio->input]) != nullptr,
                            "the remapped OCIO input names the spliced upload command");
        expectations.expect(ocio->program.get() == fixture.program.get(),
                            "the immutable OCIO program identity is unchanged by the splice");
        expectations.expect(ocio->semanticKey == fixture.ocioKey,
                            "the OCIO command metadata/semantic key is unchanged by the splice");
    }
    // Only the frozen upload image is host-retained; the two RGBA32F command outputs are not.
    const std::uint64_t retainedUploadBytes = windowBytes(fixture.data);
    const std::uint64_t expectedResident = retainedUploadBytes;
    expectations.expect(
        chargedBytes == expectedResident,
        "the splice charges only the frozen host upload, not the GPU-transient OCIO/child outputs");
    expectations.expect(chargedBytes != retainedUploadBytes * 3,
                        "the OCIO output and child composition output are not host-charged");

    // A tight allowance must refuse before the parent publishes anything.
    std::uint64_t tightCharged = 0;
    bloom::runtime::PreparedGpuSceneDiagnosticCode tightFailure =
        bloom::runtime::PreparedGpuSceneDiagnosticCode::None;
    const auto refused = spliceParentScene(fixture, *source, *parent, childScene, tightCharged,
                                           expectedResident - 1, tightFailure);
    expectations.expect(
        !refused.has_value() &&
            tightFailure ==
                bloom::runtime::PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
        "a tight allowance refuses the spliced OCIO child scene");

    if (device == nullptr) {
        return;
    }

    // Native execution of the spliced parent/child OCIO scene against the CPU oracle.
    const auto expected = cpuOracle(fixture, aces);
    auto cache = GpuSceneCache::create(*device);
    expectations.expect(cache.hasValue(), "the scene content cache hosts");
    if (!cache) {
        return;
    }
    auto executorResult = GpuSceneExecutor::create(*device, *cache.cache);
    expectations.expect(executorResult.hasValue(), "the scene executor hosts");
    if (!executorResult) {
        return;
    }
    GpuSceneExecutor& executor = *executorResult.executor;
    // The GPU-transient OCIO/child outputs are bounded by the EXECUTOR's request byte budget, not
    // the preparation allowance. A budget of one byte cannot cover the largest native step, so
    // begin() refuses with OverBudget without any Vulkan work and leaves the executor reusable.
    const auto tightBegin = executor.begin(*spliced, 1);
    expectations.expect(tightBegin.code == GpuSceneExecutorDiagnosticCode::OverBudget,
                        "the executor budget bounds the nested OCIO output allocation");
    expectations.expect(executor.state() == GpuSceneExecutorJobState::Idle,
                        "the executor budget refusal leaves the executor idle");
    const auto begun = executor.begin(*spliced, kBudget);
    expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                        "the spliced parent/child scene begins");
    if (begun.code != GpuSceneExecutorDiagnosticCode::None) {
        std::cerr << "begin diagnostic: " << begun.message << '\n';
        return;
    }
    auto poll = executor.poll();
    while (poll == GpuSceneExecutorPollResult::Pending) {
        poll = executor.poll();
    }
    expectations.expect(poll == GpuSceneExecutorPollResult::Ready,
                        "the spliced parent/child OCIO scene completes");
    if (poll != GpuSceneExecutorPollResult::Ready) {
        std::cerr << "poll diagnostic: " << executor.diagnostic().message << '\n';
        return;
    }
    const auto counters = executor.counters();
    expectations.expect(counters.ocioEffectDispatches == 1,
                        "the nested OCIO effect dispatches exactly once");
    expectations.expect(counters.readbacks == 0, "the executor never reads back a full frame");
    auto image = executor.takeImage();
    expectations.expect(image != nullptr, "the spliced scene publishes its output");
    if (image == nullptr) {
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*image, kBudget);
    expectations.expect(readback.hasValue(), "the output reads back for the test oracle only");
    if (readback) {
        std::size_t mismatches = 0;
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto& gpu = readback.pixels[index];
            const auto& want = expected[index];
            if (!close(gpu.red(), want.red()) || !close(gpu.green(), want.green()) ||
                !close(gpu.blue(), want.blue()) || gpu.alpha() != want.alpha()) {
                ++mismatches;
            }
        }
        expectations.expect(mismatches == 0,
                            "every spliced parent/child OCIO pixel matches the CPU oracle (2e-6)");
    }
}

#endif // BLOOM_GPUSHADER_TOOLS_DIR

} // namespace

int main(int argc, char** argv) {
    try {
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        if (parseRequireDevice(argc, argv)) {
            std::cerr << "FAIL: --require-device requested but BLOOM_GPUSHADER_TOOLS_DIR is not "
                         "set\n";
            return 1;
        }
        std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return kSkipExit;
#else
        const bool requireDevice = parseRequireDevice(argc, argv);
        Expectations expectations;
        const auto revision = bloom::color::ocioBuiltInContentRevision(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
        if (!revision.has_value()) {
            std::cout << "SKIP: the ACES built-in is unavailable\n";
            return kSkipExit;
        }
        auto acesResolution =
            bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                             bloom::color::kAcesCgV1ConfigUri, *revision, "ACEScg");
        auto aces = std::move(acesResolution).takeResolved();
        if (!aces.has_value()) {
            std::cerr << "FAILED: the ACES built-in does not resolve\n";
            return 1;
        }
        GpuDeviceCreationOptions options;
        auto device = GpuDevice::create(options);
        // The splice proof is device-free; run it first and fail honestly on its own assertions.
        testSpliceAndNative(expectations, device ? device.device.get() : nullptr, *aces,
                            compileOptions());
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " nested OCIO expectation(s) failed\n";
            return 1;
        }
        if (!device) {
            if (requireDevice) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return kSkipExit;
        }
        std::cout << "PASS: nested OCIO splice + native parent/child execution\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
