// Real native acceptance for the PRODUCTION GpuSceneExecutor driving an OCIO ProcessEffect scene
// command. A genuine PreparedGpuScene (proof-only GpuSceneFixtureBuilder) is
//   solid + upload -> OCIO CST effect -> merge (solid under OCIO) -> composition output
// and the executor runs it on the device owner thread with the retained native OCIO program. The
// final resident image is compared against the actual unchanged CPU OCIO oracle plus the CPU
// source-over primitive; readback happens only for that oracle. A warm run must cut the whole
// subtree with ZERO OCIO dispatches, and a changed effect program must invalidate only the effect
// (and its downstream merge/output) while the upstream solid/upload stay cache hits.
//
// Without a device the test skips cleanly; --require-device makes that a failure.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::runtime {

// Proof-only fixture seam (declared as a friend in prepared_gpu_scene.hpp). Defined only here.
struct GpuSceneFixtureBuilder final {
    [[nodiscard]] static std::shared_ptr<const PreparedGpuScene>
    make(std::vector<GpuSceneCommand> commands, const GpuSceneCommandIndex output,
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
        return std::shared_ptr<const PreparedGpuScene>(new PreparedGpuScene(
            std::move(commands), {}, output, std::move(identity), {}, outputDescriptor, {}));
    }
};

} // namespace bloom::runtime

namespace {

using bloom::core::PixelAspectRatio;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::EvaluationProvider;
using bloom::runtime::EvaluationQuality;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneCacheCreateResult;
using bloom::runtime::GpuSceneCommand;
using bloom::runtime::GpuSceneCommandIndex;
using bloom::runtime::GpuSceneCompositionOutputCommand;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneFixtureBuilder;
using bloom::runtime::GpuSceneMergeCommand;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::GpuSceneSolidCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreparedGpuOcioCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::ProcessFrameIdentity;

constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
constexpr int kSkipExit = 77;

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

[[nodiscard]] bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

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

constexpr std::uint32_t kWidth = 4;
constexpr std::uint32_t kHeight = 3;

struct Fixture final {
    ImageWindow data;
    ImageWindow display;
    PixelAspectRatio aspect = PixelAspectRatio::square();
    std::vector<Rgba32f> uploadPixels;
    Rgba32f solidPixel = pixel(0.1F, 0.2F, 0.05F, 0.5F);
    std::shared_ptr<const Rgba32fImage> uploadImage;
};

[[nodiscard]] std::string ocioCommandKey(const PreparedGpuOcioCommand& program,
                                         const std::string& inputKey) {
    return bloom::runtime::makeGpuSceneOcioEffectSemanticKey(
        inputKey, program.identity(), window(0, 0, kWidth, kHeight), PixelAspectRatio::square());
}

[[nodiscard]] std::shared_ptr<const PreparedGpuScene>
buildScene(const Fixture& fixture, std::shared_ptr<const PreparedGpuOcioCommand> program) {
    const std::string solidKey = "solid-ocio-scene-v1";
    const std::string uploadKey = "upload-ocio-scene-v1";
    const std::string ocioKey = ocioCommandKey(*program, uploadKey);
    const std::string mergeKey = "merge-ocio-scene-v1|" + ocioKey;
    const std::string outputKey = "output-ocio-scene-v1|" + mergeKey;
    std::vector<GpuSceneCommand> commands;
    commands.push_back(GpuSceneSolidCommand{.index = 0,
                                            .sourceOperation = OperationIndex::fromRaw(0),
                                            .pixel = fixture.solidPixel,
                                            .dataWindow = fixture.data,
                                            .displayWindow = fixture.display,
                                            .pixelAspect = fixture.aspect,
                                            .semanticKey = solidKey});
    commands.push_back(GpuSceneUploadCommand{.index = 1,
                                             .sourceOperation = OperationIndex::fromRaw(0),
                                             .image = fixture.uploadImage,
                                             .descriptor = *fixture.uploadImage->descriptor(),
                                             .semanticKey = uploadKey});
    commands.push_back(GpuSceneOcioEffectCommand{.index = 2,
                                                 .sourceOperation = OperationIndex::fromRaw(0),
                                                 .input = 1,
                                                 .inputKey = uploadKey,
                                                 .program = std::move(program),
                                                 .outputWindow = fixture.data,
                                                 .displayWindow = fixture.display,
                                                 .pixelAspect = fixture.aspect,
                                                 .semanticKey = ocioKey});
    commands.push_back(GpuSceneMergeCommand{.index = 3,
                                            .sourceOperation = OperationIndex::fromRaw(0),
                                            .foregrounds = {0, 2},
                                            .outputWindow = fixture.data,
                                            .displayWindow = fixture.display,
                                            .pixelAspect = fixture.aspect,
                                            .semanticKey = mergeKey});
    commands.push_back(
        GpuSceneCompositionOutputCommand{.index = 4,
                                         .sourceOperation = OperationIndex::fromRaw(0),
                                         .input = 3,
                                         .dataWindow = fixture.data,
                                         .displayWindow = fixture.display,
                                         .pixelAspect = fixture.aspect,
                                         .semanticKey = outputKey});
    const auto descriptor =
        Rgba32fImageDescriptor::create(fixture.data, fixture.display, fixture.aspect);
    return GpuSceneFixtureBuilder::make(std::move(commands), 4, *descriptor.value());
}

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

[[nodiscard]] bool runScene(Expectations& expectations, GpuSceneExecutor& executor,
                            const std::shared_ptr<const PreparedGpuScene>& scene,
                            const std::string_view label) {
    const auto begun = executor.begin(scene, kBudget);
    expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                        std::string(label) + ": begin accepts the scene");
    if (begun.code != GpuSceneExecutorDiagnosticCode::None) {
        std::cerr << label << " begin diagnostic: " << begun.message << '\n';
        return false;
    }
    auto poll = executor.poll();
    while (poll == GpuSceneExecutorPollResult::Pending) {
        poll = executor.poll();
    }
    expectations.expect(poll == GpuSceneExecutorPollResult::Ready,
                        std::string(label) + ": the scene completes");
    if (poll != GpuSceneExecutorPollResult::Ready) {
        std::cerr << label << " poll diagnostic: " << executor.diagnostic().message << '\n';
    }
    return poll == GpuSceneExecutorPollResult::Ready;
}

void testScene(Expectations& expectations, GpuDevice& device,
               const bloom::color::ResolvedBloomNeutralConfig& aces,
               const GpuOcioCompileOptions& options) {
    Fixture fixture{.data = window(0, 0, kWidth, kHeight),
                    .display = window(0, 0, kWidth, kHeight),
                    .aspect = PixelAspectRatio::square(),
                    .uploadPixels = fixturePixels(kWidth, kHeight),
                    .uploadImage = nullptr};
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
                        "the scene OCIO command is ProcessEffect");
    const auto scene = buildScene(fixture, prepared.command);
    const auto expected = cpuOracle(fixture, aces);

    auto cacheResult = GpuSceneCache::create(device);
    expectations.expect(cacheResult.hasValue(), "the scene content cache hosts");
    if (!cacheResult) {
        return;
    }
    GpuSceneExecutorBudgets budgets;
    auto executorResult = GpuSceneExecutor::create(device, *cacheResult.cache, budgets);
    expectations.expect(executorResult.hasValue(), "the scene executor hosts");
    if (!executorResult) {
        return;
    }
    GpuSceneExecutor& executor = *executorResult.executor;

    expectations.expect(runScene(expectations, executor, scene, "cold"), "the cold scene runs");
    auto image = executor.takeImage();
    expectations.expect(image != nullptr, "the resident scene output is published");
    if (image == nullptr) {
        return;
    }
    const auto coldCounters = executor.counters();
    expectations.expect(coldCounters.ocioEffectDispatches == 1,
                        "the cold scene dispatches the OCIO effect exactly once");
    expectations.expect(coldCounters.ocioProgramCreations == 1,
                        "the cold scene creates the native OCIO program once");
    expectations.expect(coldCounters.uploads == 1 && coldCounters.solidDispatches == 2,
                        "the cold scene runs its upstream upload and both solid steps");
    expectations.expect(coldCounters.readbacks == 0, "the executor never reads back a full frame");
    const auto readback = bloom::render::readbackResidentImage(*image, kBudget);
    expectations.expect(readback.hasValue(), "the scene output reads back for the oracle");
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
                            "every scene pixel matches the CPU OCIO + source-over oracle");
    }

    expectations.expect(runScene(expectations, executor, scene, "warm"), "the warm scene runs");
    auto warmImage = executor.takeImage();
    expectations.expect(warmImage != nullptr, "the warm scene publishes the cached output");
    const auto warmCounters = executor.counters();
    expectations.expect(warmCounters.ocioEffectDispatches == coldCounters.ocioEffectDispatches,
                        "a warm unchanged scene dispatches zero OCIO effects");
    expectations.expect(warmCounters.dispatches == coldCounters.dispatches,
                        "a warm unchanged scene dispatches zero native operations");
    expectations.expect(warmCounters.outputCacheHits == coldCounters.outputCacheHits + 1,
                        "a warm unchanged scene is an output cache hit");

    // Dirty effect only: a different OCIO program invalidates the effect (and downstream merge/
    // output) while the upstream solid/upload stay cache hits.
    GpuOcioTransformSpec adjust;
    adjust.kind = GpuOcioTransformKind::ExposureContrast;
    adjust.fromId = "ACES2065-1";
    adjust.exposure = 0.5;
    adjust.contrast = 1.0;
    const auto dirtyProgram =
        preparer.prepare(aces, adjust, GpuOcioCommandGeometry{kWidth, kHeight}, options);
    expectations.expect(dirtyProgram.hasValue(), "the dirty OCIO command prepares");
    if (dirtyProgram) {
        const auto dirtyScene = buildScene(fixture, dirtyProgram.command);
        expectations.expect(runScene(expectations, executor, dirtyScene, "dirty"),
                            "the dirty scene runs");
        static_cast<void>(executor.takeImage());
        const auto dirtyCounters = executor.counters();
        expectations.expect(dirtyCounters.ocioEffectDispatches ==
                                warmCounters.ocioEffectDispatches + 1,
                            "a changed effect dispatches exactly one new OCIO effect");
        expectations.expect(dirtyCounters.uploads == warmCounters.uploads,
                            "a changed effect keeps the upstream upload a cache hit");
        // Only the merge's transparent base re-runs; the upstream solid command itself stays a
        // cache hit (a re-dispatch of the upstream solid would add two, not one).
        expectations.expect(dirtyCounters.solidDispatches == warmCounters.solidDispatches + 1,
                            "a changed effect keeps the upstream solid a cache hit");
    }

    // Actual-byte budget: a ceiling one byte below the measured retained allocation refuses the
    // program without entering the cache. Measured from the single CST program of the cold run (the
    // dirty run later adds a second program, so the running total would not isolate one program).
    const std::uint64_t actualBytes = coldCounters.ocioRetainedProgramBytes;
    expectations.expect(actualBytes > 0, "the retained OCIO program reports actual bytes");
    auto tightCache = GpuSceneCache::create(device);
    GpuSceneExecutorBudgets tightBudgets;
    tightBudgets.maxOcioRetainedProgramBytes = actualBytes > 0 ? actualBytes - 1 : 0;
    auto tightResult = GpuSceneExecutor::create(device, *tightCache.cache, tightBudgets);
    if (tightResult) {
        const auto tightScene = buildScene(fixture, prepared.command);
        const auto begun = tightResult.executor->begin(tightScene, kBudget);
        expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                            "the tight-budget scene begins");
        auto poll = tightResult.executor->poll();
        while (poll == GpuSceneExecutorPollResult::Pending) {
            poll = tightResult.executor->poll();
        }
        expectations.expect(poll == GpuSceneExecutorPollResult::Failure &&
                                tightResult.executor->diagnostic().code ==
                                    GpuSceneExecutorDiagnosticCode::OverBudget,
                            "a budget below the actual retained bytes refuses the OCIO program");
        expectations.expect(tightResult.executor->counters().ocioProgramRefusals >= 1,
                            "the refusal is counted");
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return kSkipExit;
#else
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
    if (!device) {
        if (requireDevice) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return kSkipExit;
    }
    expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
    testScene(expectations, *device.device, *aces, compileOptions());
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO scene executor expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: runtime OCIO scene executor native acceptance\n";
    return 0;
#endif
}
