// Real native proof that a prepared GPU scene honours a request ROI end to end.
//
// The scene is built by the REAL production CpuGpuSceneBuilder from a genuine compiled plan with a
// nonzero-origin ROI, run through the production GpuSceneExecutor on a real device, and read back
// for an assertion against the unchanged CpuCompositionEvaluator oracle only. The native output
// must be exactly the requested ROI (data window, display window, and pixel aspect), its pixels
// must match the CPU process image within 2e-6, the cold run must dispatch real native work, the
// immediate warm re-run must dispatch nothing and hit the content cache, and an ROI edit must reuse
// every unchanged intermediate command while only the clipped output command re-dispatches.
//
// Without a device the test reports an explicit SKIP (exit 77); --require-device makes that a
// failure. An explicit --loader selects the Vulkan loader.

#include "gpu_roi_scene_fixture_support.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::ImageWindow;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::PreparedGpuScene;

constexpr std::uint64_t kSceneBudget = 1U << 28U;

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

struct RunOutcome final {
    GpuSceneExecutorDiagnosticCode code = GpuSceneExecutorDiagnosticCode::None;
    GpuSceneExecutorPollResult pollResult = GpuSceneExecutorPollResult::Failure;
    std::vector<Rgba32f> pixels;
    std::optional<ImageWindow> dataWindow;
    std::optional<ImageWindow> displayWindow;
    bloom::core::PixelAspectRatio pixelAspect = bloom::core::PixelAspectRatio::square();
};

[[nodiscard]] RunOutcome runScene(GpuSceneExecutor& executor,
                                  const std::shared_ptr<const PreparedGpuScene>& scene) {
    RunOutcome outcome;
    outcome.code = executor.begin(scene, kSceneBudget).code;
    if (outcome.code != GpuSceneExecutorDiagnosticCode::None) {
        return outcome;
    }
    for (;;) {
        const auto poll = executor.poll();
        if (poll == GpuSceneExecutorPollResult::Pending) {
            continue;
        }
        outcome.pollResult = poll;
        break;
    }
    if (outcome.pollResult == GpuSceneExecutorPollResult::Ready) {
        const auto* image = executor.image();
        if (image != nullptr) {
            outcome.dataWindow = image->dataWindow();
            outcome.displayWindow = image->displayWindow();
            outcome.pixelAspect = image->pixelAspect();
            const auto readback = readbackResidentImage(*image, kSceneBudget);
            if (readback.hasValue()) {
                outcome.pixels = readback.pixels;
            }
        }
        static_cast<void>(executor.takeImage());
    }
    return outcome;
}

[[nodiscard]] bool parity(const std::vector<Rgba32f>& actual,
                          const std::span<const Rgba32f> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e)) {
                return false;
            }
            const float scale = std::max(std::abs(a), std::abs(e));
            if (std::abs(a - e) > 2e-6F * std::max(1.0F, scale)) {
                return false;
            }
        }
    }
    return true;
}

// The absolute-coordinate pixel of one process image.
[[nodiscard]] const Rgba32f* pixelAt(const Rgba32fImage& image, const std::int64_t x,
                                     const std::int64_t y) {
    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr) {
        return nullptr;
    }
    const auto& window = descriptor->dataWindow();
    if (x < window.originX() || x >= window.maxXExclusive() || y < window.originY() ||
        y >= window.maxYExclusive()) {
        return nullptr;
    }
    const auto width = window.extent().width();
    const auto index = static_cast<std::size_t>(y - window.originY()) * width +
                       static_cast<std::size_t>(x - window.originX());
    return &image.pixels()[index];
}

// One direct ROI fixture: build, execute cold and warm, and check descriptor, pixel parity, and
// the real cold/warm dispatch evidence against the CPU oracle.
void runRoiFixture(Expectations& expectations, GpuDevice& device,
                   const CpuCompositionEvaluator& evaluator,
                   const std::shared_ptr<const CompiledCompositionPlan>& plan,
                   const EvaluationRequest& request, const std::string& label) {
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        std::cerr << label << " prepare diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU oracle evaluates");
    if (!frame.frame()) {
        return;
    }
    const auto& oracle = frame.frame()->processImage();

    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{});
    expectations.expect(cache.hasValue(), label + ": scene cache creates");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), label + ": executor creates");
    if (!executor) {
        return;
    }
    const auto cold = runScene(*executor.executor, prepared.scene);
    expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready,
                        label + ": cold run completes: " + executor.executor->diagnostic().message);
    if (cold.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto coldCounters = executor.executor->counters();
    expectations.expect(coldCounters.dispatches > 0, label + ": real native dispatch ran");
    expectations.expect(coldCounters.readbacks == 0,
                        label + ": no full-frame readback in the executor");
    expectations.expect(cold.pixels.size() == oracle.pixels().size() &&
                            parity(cold.pixels, oracle.pixels()),
                        label + ": native ROI pixels match the CPU oracle (2e-6)");
    const auto& oracleDescriptor = *oracle.descriptor();
    expectations.expect(cold.dataWindow.has_value() &&
                            *cold.dataWindow == oracleDescriptor.dataWindow() &&
                            cold.displayWindow.has_value() &&
                            *cold.displayWindow == oracleDescriptor.displayWindow() &&
                            cold.pixelAspect == oracleDescriptor.pixelAspect(),
                        label + ": native output descriptor is the CPU ROI descriptor");
    if (request.roi.has_value()) {
        expectations.expect(cold.dataWindow.has_value() && *cold.dataWindow == *request.roi,
                            label + ": native output is clipped to the requested ROI");
    }

    const auto warm = runScene(*executor.executor, prepared.scene);
    expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready,
                        label + ": warm run completes");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.dispatches == coldCounters.dispatches,
                        label + ": warm rerun dispatches zero new native work");
    expectations.expect(warmCounters.commandCacheHits > coldCounters.commandCacheHits,
                        label + ": warm rerun is served from the content cache");
    expectations.expect(warm.pixels.size() == oracle.pixels().size() &&
                            parity(warm.pixels, oracle.pixels()),
                        label + ": warm ROI pixels still match the CPU oracle");
}

void testSolidRoiNative(Expectations& expectations, GpuDevice& device,
                        const CpuCompositionEvaluator& evaluator, GpuSceneCache& cache) {
    const auto plan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 80000);
    auto request = requestFor(*plan);
    request.roi = roi(3, 2, 5, 6);
    runRoiFixture(expectations, device, evaluator, plan, request, "solid nonzero-origin ROI");

    auto transparentRequest = requestFor(*plan);
    transparentRequest.roi = roi(13, 9, 3, 3);
    runRoiFixture(expectations, device, evaluator, plan, transparentRequest,
                  "solid transparent ROI");
    (void)cache;
}

void testVectorRoiNative(Expectations& expectations, GpuDevice& device,
                         const CpuCompositionEvaluator& evaluator) {
    const auto text = textPlan(format(24, 16), LayerValues{.position = {12.3, 8.1}}, 80100);
    auto textRequest = requestFor(*text);
    textRequest.roi = roi(7, 3, 9, 6);
    runRoiFixture(expectations, device, evaluator, text, textRequest, "text fractional ROI");

    ShapeValues values;
    values.kind = bloom::document::ShapeKind::Ellipse;
    values.strokeEnabled = true;
    values.strokeWidth = 2.0;
    values.fillColor = Color4d{0.7, 0.2, 0.1, 0.8};
    values.strokeColor = Color4d{0.1, 0.4, 0.9, 0.6};
    const auto shape = shapePlan(
        format(24, 16), LayerValues{.position = {9.7, 6.2}, .opacity = 0.8}, values, 80200);
    auto shapeRequest = requestFor(*shape);
    shapeRequest.roi = roi(5, 2, 10, 8);
    runRoiFixture(expectations, device, evaluator, shape, shapeRequest, "shape coverage ROI");
}

void testAffineRoiNative(Expectations& expectations, GpuDevice& device,
                         const CpuCompositionEvaluator& evaluator) {
    const auto plan = twoLayerPlan(
        format(24, 16),
        LayerValues{.position = {12.0, 8.0}, .scale = {1.75, -0.5}, .rotation = 30.0},
        LayerValues{
            .position = {7.0, 9.0}, .anchor = {0.25, 0.5}, .scale = {2.0, 0.5}, .rotation = -15.0},
        9.0, 7.0, 80300);
    auto request = requestFor(*plan);
    request.roi = roi(2, 1, 12, 9);
    runRoiFixture(expectations, device, evaluator, plan, request, "fractional affine ROI");
}

void testNestedRoiNative(Expectations& expectations, GpuDevice& device,
                         const CpuCompositionEvaluator& evaluator) {
    const auto child = nestedChildPlan(80400, 811, Color4d{0.2, 0.4, 0.6, 0.75});
    const auto parent = nestedParentPlan(child, 812);
    auto request = requestFor(*parent);
    request.roi = roi(2, 1, 6, 5);
    runRoiFixture(expectations, device, evaluator, parent, request, "nested child ROI");
}

// An ROI edit over the same plan: the full scene runs cold, then the ROI scene runs on the same
// cache. Every unchanged intermediate command is reused (the ROI run dispatches strictly fewer
// commands than the full cold run) and only the clipped output command is new; the ROI pixels are
// exactly the full-frame pixels inside the region.
void testRoiEditCacheReuse(Expectations& expectations, GpuDevice& device,
                           const CpuCompositionEvaluator& evaluator) {
    const auto plan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 80500);
    const CpuGpuSceneBuilder builder;
    const auto full = builder.build(plan, requestFor(*plan));
    auto roiRequest = requestFor(*plan);
    roiRequest.roi = roi(3, 2, 5, 6);
    const auto cropped = builder.build(plan, roiRequest);
    expectations.expect(full.hasValue() && cropped.hasValue(), "full and ROI scenes prepare");
    if (!full || !cropped) {
        return;
    }
    auto fullOracleRequest = requestFor(*plan);
    fullOracleRequest.bypassOperationCache = true;
    const auto fullFrame = evaluator.evaluate(plan, fullOracleRequest, {});
    expectations.expect(fullFrame.frame() != nullptr, "full CPU oracle evaluates");
    if (!fullFrame.frame()) {
        return;
    }

    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{});
    expectations.expect(cache.hasValue(), "ROI edit: cache creates");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "ROI edit: executor creates");
    if (!executor) {
        return;
    }
    const auto fullCold = runScene(*executor.executor, full.scene);
    expectations.expect(fullCold.pollResult == GpuSceneExecutorPollResult::Ready,
                        "ROI edit: full cold run completes");
    if (fullCold.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto fullCounters = executor.executor->counters();
    const auto roiCold = runScene(*executor.executor, cropped.scene);
    expectations.expect(roiCold.pollResult == GpuSceneExecutorPollResult::Ready,
                        "ROI edit: ROI run completes on the same cache");
    if (roiCold.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto roiCounters = executor.executor->counters();
    // The unchanged source/layer/merge commands are reused; only the terminal output translation
    // dispatches again because its clipped data window changed.
    expectations.expect(roiCounters.dispatches - fullCounters.dispatches == 1,
                        "ROI edit: exactly the clipped output command re-dispatches");
    // The frozen full-frame process image, restricted to the ROI, is bit-identical to the ROI run.
    bool sliceParity =
        roiCold.pixels.size() == static_cast<std::size_t>(roiRequest.roi->extent().width()) *
                                     roiRequest.roi->extent().height();
    const auto& fullImage = fullFrame.frame()->processImage();
    std::size_t index = 0;
    for (std::int64_t y = roiRequest.roi->originY();
         sliceParity && y < roiRequest.roi->maxYExclusive(); ++y) {
        for (std::int64_t x = roiRequest.roi->originX();
             sliceParity && x < roiRequest.roi->maxXExclusive(); ++x, ++index) {
            const auto* expected = pixelAt(fullImage, x, y);
            if (expected == nullptr || !(roiCold.pixels[index] == *expected)) {
                sliceParity = false;
            }
        }
    }
    expectations.expect(sliceParity,
                        "ROI edit: ROI pixels are the full-frame golden restricted to the region");
}

void testInvalidRoiRejected(Expectations& expectations) {
    const auto plan = twoLayerPlan(format(8, 8), LayerValues{}, LayerValues{}, 8.0, 8.0, 80600);
    const CpuGpuSceneBuilder builder;
    auto request = requestFor(*plan);
    request.roi = roi(0, 0, 9, 8);
    const auto prepared = builder.build(plan, request);
    expectations.expect(!prepared.hasValue(), "an out-of-resolution ROI is refused before the GPU");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 77;
        }
        const CpuCompositionEvaluator evaluator;
        auto cache = GpuSceneCache::create(*device.device, GpuSceneCacheBudgets{});
        if (!cache) {
            std::cerr << "scene cache could not be created\n";
            return 1;
        }
        testSolidRoiNative(expectations, *device.device, evaluator, *cache.cache);
        testVectorRoiNative(expectations, *device.device, evaluator);
        testAffineRoiNative(expectations, *device.device, evaluator);
        testNestedRoiNative(expectations, *device.device, evaluator);
        testRoiEditCacheReuse(expectations, *device.device, evaluator);
        testInvalidRoiRejected(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: native ROI GPU scene expectations failed\n";
            return 1;
        }
        std::cout << "PASS: native GPU scene ROI vs CPU oracle\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
