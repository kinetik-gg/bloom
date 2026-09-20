// Real native proof that a request ROI is honoured on the media + colour-transform path.
//
// A real AP0 (ACES2065-1 tagged) OpenEXR fixture is resolved through the production media pipeline.
// The production CpuGpuSceneBuilder (with the shared GpuSceneOcioContext) emits a RAW full-source
// upload plus a real input->working OCIO ProcessEffect, and the terminal Composition Output is
// clipped to the requested ROI. The prepared scene is then run through the production
// GpuSceneExecutor on a real device and compared with the unchanged CpuCompositionEvaluator oracle.
//
// The ROI must not change the raw upload (full source dimensions and identity), so an unchanged
// source is reused across an ROI edit; only the clipped output command re-dispatches.
//
// Without the pinned shader tools the test SKIPs; without a device it SKIPs (exit 77) unless
// --require-device is passed.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_media_scene_preparation_test_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::ImageWindow;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::GpuSceneUploadCommand;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::media_executor_test::descriptorMatchesScene;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::runScene;

constexpr std::string_view kAcesWorking = "ACEScg";

[[nodiscard]] bloom::runtime::EvaluationRequest
acesRequest(const CompiledCompositionPlan& plan,
            const std::optional<ImageWindow>& region = std::nullopt) {
    auto request = requestFor(plan);
    request.colorIntent = EvaluationColorIntent{
        .workingColorSpaceId = kAcesWorking,
        .ocioConfigRevision = {},
        .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri,
    };
    request.roi = region;
    return request;
}

[[nodiscard]] const GpuSceneUploadCommand* firstUpload(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
            return upload;
        }
    }
    return nullptr;
}

[[nodiscard]] const GpuSceneOcioEffectCommand* firstOcio(const PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
            return ocio;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<ImageWindow> region(const std::int64_t x, const std::int64_t y,
                                                const std::uint64_t w, const std::uint64_t h) {
    const auto window = ImageWindow::create(x, y, w, h);
    if (!window) {
        return std::nullopt;
    }
    return *window.value();
}

// Builds the ROI scene and asserts the raw upload stays full resolution, the OCIO effect consumes
// it, the process descriptor is the ROI, and the native run matches the CPU oracle with cold/warm
// cache evidence.
void testMediaRoi(Expectations& expectations, GpuDevice& device, CpuCompositionEvaluator& evaluator,
                  const std::filesystem::path& directory, const bloom::document::AssetRecord& asset,
                  const GpuSceneOcioContext& ocioContext) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = directory;
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan =
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 91000);
    const auto roiWindow = region(2, 1, 4, 5);
    expectations.expect(roiWindow.has_value(), "media ROI: the region builds");
    if (!roiWindow) {
        return;
    }
    const auto request = acesRequest(*plan, roiWindow);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "media ROI: prepares");
    if (!prepared) {
        std::cerr << "media ROI prepare diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    const auto* upload = firstUpload(*prepared.scene);
    const auto* ocio = firstOcio(*prepared.scene);
    expectations.expect(upload != nullptr && ocio != nullptr,
                        "media ROI: emits a raw upload and a real OCIO colour command");
    if (upload == nullptr || ocio == nullptr) {
        return;
    }
    const auto sourceWindow = ImageWindow::create(0, 0, asset.width, asset.height);
    expectations.expect(sourceWindow && upload->descriptor.dataWindow() == *sourceWindow.value(),
                        "media ROI: the raw upload keeps the FULL source dimensions");
    expectations.expect(ocio->outputWindow == upload->descriptor.dataWindow(),
                        "media ROI: the OCIO effect runs over the full source window");
    expectations.expect(prepared.scene->outputDescriptor().dataWindow() == *roiWindow,
                        "media ROI: the process data window is exactly the requested ROI");
    expectations.expect(prepared.scene->outputDescriptor().displayWindow() ==
                            upload->descriptor.displayWindow(),
                        "media ROI: the process display window is preserved");

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "media ROI: CPU oracle evaluates");
    if (!frame.frame()) {
        return;
    }
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{});
    expectations.expect(cache.hasValue(), "media ROI: cache creates");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "media ROI: executor creates");
    if (!executor) {
        return;
    }
    const auto cold = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(cold.ready, "media ROI: cold run completes");
    if (!cold.ready) {
        return;
    }
    const auto readback = readbackResidentImage(*cold.image, kSceneBudget);
    expectations.expect(readback.hasValue(), "media ROI: readback succeeds");
    expectations.expect(cold.countersAtReady.ocioEffectDispatches > 0,
                        "media ROI: a real native OCIO effect dispatch ran");
    expectations.expect(cold.countersAtReady.readbacks == 0,
                        "media ROI: the executor performs no full-frame readback");
    expectations.expect(descriptorMatchesScene(*cold.image, *prepared.scene),
                        "media ROI: the native descriptor is the scene ROI descriptor");
    expectations.expect(readback.hasValue() &&
                            pixelsClose(readback.pixels, frame.frame()->processImage().pixels()),
                        "media ROI: native ROI pixels match the CPU oracle (2e-6)");

    const auto warmCountersBefore = executor.executor->counters();
    const auto warm = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(warm.ready, "media ROI: warm run completes");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.dispatches == warmCountersBefore.dispatches,
                        "media ROI: warm rerun dispatches zero new native work");
    expectations.expect(warmCounters.commandCacheHits > warmCountersBefore.commandCacheHits,
                        "media ROI: warm rerun is served from the content cache");
}

// A full-frame and an ROI build of the SAME media plan over one prepared-upload cache and one
// executor cache: the raw upload semantic key is identical (so the source is reused across the ROI
// edit), and only the clipped output command re-dispatches.
void testMediaRoiEditReuse(Expectations& expectations, GpuDevice& device,
                           CpuCompositionEvaluator& evaluator,
                           const std::filesystem::path& directory,
                           const bloom::document::AssetRecord& asset,
                           const GpuSceneOcioContext& ocioContext) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = directory;
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan =
        mediaPlan(format(8, 8), asset, LayerValues{.position = {4.3, 3.1}, .opacity = 0.9}, 92000);
    const auto full = builder.build(plan, acesRequest(*plan));
    const auto roiWindow = region(2, 1, 4, 5);
    expectations.expect(roiWindow.has_value(), "media ROI edit: the region builds");
    if (!roiWindow) {
        return;
    }
    const auto cropped = builder.build(plan, acesRequest(*plan, roiWindow));
    expectations.expect(full.hasValue() && cropped.hasValue(),
                        "media ROI edit: both builds prepare");
    if (!full || !cropped) {
        return;
    }
    const auto* fullUpload = firstUpload(*full.scene);
    const auto* croppedUpload = firstUpload(*cropped.scene);
    expectations.expect(fullUpload != nullptr && croppedUpload != nullptr &&
                            fullUpload->semanticKey == croppedUpload->semanticKey,
                        "media ROI edit: the raw source upload identity is unchanged");
    expectations.expect(cropped.scene->mediaStatistics().uploadCacheHits == 1,
                        "media ROI edit: the unchanged source is served from the upload cache");

    auto oracleRequest = acesRequest(*plan);
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "media ROI edit: full CPU oracle evaluates");
    if (!frame.frame()) {
        return;
    }
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{});
    expectations.expect(cache.hasValue(), "media ROI edit: cache creates");
    if (!cache) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "media ROI edit: executor creates");
    if (!executor) {
        return;
    }
    const auto fullCold = runScene(*executor.executor, full.scene, kSceneBudget);
    expectations.expect(fullCold.ready, "media ROI edit: full cold run completes");
    if (!fullCold.ready) {
        return;
    }
    const auto fullCounters = executor.executor->counters();
    const auto roiCold = runScene(*executor.executor, cropped.scene, kSceneBudget);
    expectations.expect(roiCold.ready, "media ROI edit: ROI run completes on the same cache");
    if (!roiCold.ready) {
        return;
    }
    const auto roiCounters = executor.executor->counters();
    expectations.expect(roiCounters.dispatches - fullCounters.dispatches == 1,
                        "media ROI edit: exactly the clipped output command re-dispatches");
    const auto roiReadback = readbackResidentImage(*roiCold.image, kSceneBudget);
    bool sliceParity =
        roiReadback.hasValue() &&
        roiReadback.pixels.size() ==
            static_cast<std::size_t>(roiWindow->extent().width()) * roiWindow->extent().height();
    const auto& fullImage = frame.frame()->processImage();
    const auto* fullDescriptor = fullImage.descriptor();
    std::size_t index = 0;
    for (std::int64_t y = roiWindow->originY(); sliceParity && y < roiWindow->maxYExclusive();
         ++y) {
        for (std::int64_t x = roiWindow->originX(); sliceParity && x < roiWindow->maxXExclusive();
             ++x, ++index) {
            const auto width = fullDescriptor->dataWindow().extent().width();
            const auto fullIndex =
                static_cast<std::size_t>(y - fullDescriptor->dataWindow().originY()) * width +
                static_cast<std::size_t>(x - fullDescriptor->dataWindow().originX());
            if (!(roiReadback.pixels[index] == fullImage.pixels()[fullIndex])) {
                sliceParity = false;
            }
        }
    }
    expectations.expect(sliceParity,
                        "media ROI edit: ROI pixels are the full-frame golden inside the region");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return 77;
#else
        Expectations expectations;
        auto directory = std::filesystem::temp_directory_path() / "bloom_gpu_roi_media_color_test";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        const auto path = directory / "aces_ap0.exr";
        writeExrRgbaWithChromaticities(path, 3, 2, signedHdrPixels(), acesAp0Chromaticities());
        const auto asset = imageAsset(path, "aces_ap0", 700);

        CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(directory);

        auto preparer = std::make_shared<GpuOcioProgramPreparer>();
        GpuOcioCompileOptions compileOptions;
        compileOptions.glslangValidatorPath =
            std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
        compileOptions.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
        GpuSceneOcioContext ocioContext;
        ocioContext.preparer = preparer;
        ocioContext.compileOptions = compileOptions;

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
        testMediaRoi(expectations, *device.device, evaluator, directory, asset, ocioContext);
        testMediaRoiEditReuse(expectations, *device.device, evaluator, directory, asset,
                              ocioContext);
        if (!expectations.ok()) {
            std::cerr << "FAIL: native media/colour ROI expectations failed\n";
            return 1;
        }
        std::cout << "PASS: native GPU media/colour ROI vs CPU oracle\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
