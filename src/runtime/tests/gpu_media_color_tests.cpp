// The bounded media-source GPU colour path end to end.
//
// A real AP0 (ACES2065-1 tagged) OpenEXR fixture is resolved through the production media pipeline.
// The production CpuGpuSceneBuilder (with the shared GpuSceneOcioContext) emits a RAW full-source
// upload plus a real input->working OCIO ProcessEffect command; the production GpuSceneExecutor
// runs it on a real device and the result is compared to the genuine, unchanged
// CpuCompositionEvaluator frame at the documented 2e-6 gate. There is no CPU OCIO pass on the GPU
// route: the source is decoded raw (file decode only) and the colour transform is the dispatched
// OCIO effect.
//
// Also asserted: the upload carries the FULL source dimensions and the composition display
// window/pixel aspect; the decode/upload identity is independent of the working space (a changed
// working space reuses the decoded upload); a warm build performs no decode and a warm executor run
// performs zero native dispatches; a missing/corrupt source fails with an honest diagnostic and no
// partial scene.
//
// Without the pinned shader tools the test prints SKIP. Without a device it skips cleanly unless
// --require-device is passed.
//
// The shared fixtures/oracle helpers and the still/video test groups live in the .ipp files
// included below, inside this translation unit's anonymous namespace, so the executable and every
// test name are unchanged.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_media_scene_preparation_test_support.hpp"
#include "gpu_media_video_scene_preparation_test_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

#include "gpu_media_color_test_support.ipp"

#include "gpu_media_color_still_tests.ipp"

#include "gpu_media_color_video_tests.ipp"

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return kSkipExit;
#else
        Expectations expectations;
        const auto fixture = makeAcesFixture();
        CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(fixture.directory);
        auto preparer = std::make_shared<GpuOcioProgramPreparer>();
        GpuOcioCompileOptions compileOptions;
        compileOptions.glslangValidatorPath =
            std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
        compileOptions.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
        GpuSceneOcioContext ocioContext;
        ocioContext.preparer = preparer;
        ocioContext.compileOptions = compileOptions;

        testChangedWorkingSpaceReusesDecode(expectations, evaluator, fixture, ocioContext);
        testMissingAndCorruptMedia(expectations, fixture, ocioContext);
        testProxyCancellation(expectations, evaluator, fixture, ocioContext);
        testVideoCancellation(expectations, options.fixtures, ocioContext);

        bloom::render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = bloom::render::GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "NOTE: no compatible Vulkan device; the native route was not executed\n";
        } else {
            testColdBuilderToExecutor(expectations, *device.device, evaluator, fixture,
                                      ocioContext);
            testProxiedStill(expectations, *device.device, evaluator, fixture, ocioContext);
            testProxiedIdentityStill(expectations, *device.device, evaluator, fixture, ocioContext);
            testRealVideoColour(expectations, *device.device, evaluator, options.fixtures,
                                ocioContext);
        }

        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU media colour expectations failed\n";
            return 1;
        }
        std::cout << "PASS: GPU media colour\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
