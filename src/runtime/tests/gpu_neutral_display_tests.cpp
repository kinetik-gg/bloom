#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include "shaders/neutral_display_spirv.inc"

#if defined(BLOOM_RENDER_TEST_VULKAN_BACKEND)
// Minimal declaration of the private test seam, kept free of any Vulkan include so this test
// remains a Bloom/Vulkan-free translation unit. The definition and matching enum live in
// gpu_neutral_display_private.hpp / gpu_neutral_display_resources.cpp.
namespace bloom::render::neutral_display_detail {
enum class FenceOverride : std::int32_t {
    None = 0,
    NotReady = 1,
    Success = 2,
    Unknown = 3,
};
void setFenceOverride(FenceOverride override) noexcept;
} // namespace bloom::render::neutral_display_detail
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuNeutralDisplay;
using bloom::render::GpuNeutralDisplayBudgets;
using bloom::render::GpuNeutralDisplayDiagnostic;
using bloom::render::GpuNeutralDisplayDiagnosticCode;
using bloom::render::GpuNeutralDisplayJobState;
using bloom::render::GpuNeutralDisplayPollResult;
using bloom::render::GpuOperationId;
using bloom::render::GpuPrecision;
using bloom::render::GpuQualification;
using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba8;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

// The embedded SPIR-V array is the actual bytes uploaded at runtime, so hashing the serialized
// little-endian words proves the checked-in source -> SPIR-V -> array relationship directly: a
// changed array word fails even if every comment in the .inc is intact.
#include "gpu_neutral_display_spirv.ipp"

void testCpuOracleOnly(Expectations& expectations,
                       const bloom::color::PreparedCpuDisplayProcessorHandle& handle) {
    // The CPU oracle must remain available even where the GPU path rejects a frame.
    const std::vector<Rgba32f> subnormal{
        pixel(std::numeric_limits<float>::denorm_min(), 0.0F, 0.0F, 1.0F)};
    const auto cpu = cpuOracle(handle, subnormal, 1, 1);
    expectations.expect(cpu.has_value(), "CPU oracle processes a subnormal source independently");

    // Rgba32f::fromPremultiplied rejects non-finite components, so a Bloom-owned source can never
    // carry one. The shader's non-finite error bits are defence in depth for a corrupt upload, not
    // a reachable public input, and are exercised here only through the shader's own flag handling.
    expectations.expect(
        !Rgba32f::fromPremultiplied(std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F)
             .hasValue(),
        "the public pixel type rejects a non-finite component");
}

void testErrorAndBudgetPaths(Expectations& expectations, GpuNeutralDisplay& display,
                             const bloom::color::PreparedCpuDisplayProcessorHandle& handle) {
    // Empty source.
    const std::vector<Rgba32f> empty;
    expectations.expect(display.begin(empty, 1ULL << 30ULL).code ==
                            GpuNeutralDisplayDiagnosticCode::InvalidArgument,
                        "an empty source is rejected");
    expectations.expect(display.state() == GpuNeutralDisplayJobState::Idle,
                        "a rejected begin leaves the pipeline idle");

    // Over budget by the explicit per-call budget.
    const std::vector<Rgba32f> one{pixel(0.1F, 0.1F, 0.1F, 1.0F)};
    expectations.expect(display.begin(one, 4).code == GpuNeutralDisplayDiagnosticCode::OverBudget,
                        "a per-call byte budget below the frame is rejected");

    // Busy: begin twice without polling.
    const std::vector<Rgba32f> small = randomFixture(4096, 7);
    const auto first = display.begin(small, 1ULL << 30ULL);
    expectations.expect(first.code == GpuNeutralDisplayDiagnosticCode::None,
                        "the first begin is accepted");
    const auto second = display.begin(small, 1ULL << 30ULL);
    expectations.expect(second.code == GpuNeutralDisplayDiagnosticCode::Busy,
                        "a second begin while a job is in flight is Busy");
    while (display.poll() == GpuNeutralDisplayPollResult::Pending) {
        std::this_thread::yield();
    }
    expectations.expect(display.readback().hasValue(), "the first job still completes");

    // Non-finite inputs cannot be constructed through Rgba32f, so the shader's non-finite bits are
    // unreachable through the public API. Only the subnormal rejection is reachable here.
    expectations.expect(
        !Rgba32f::fromPremultiplied(std::numeric_limits<float>::infinity(), 0.0F, 0.0F, 1.0F)
             .hasValue(),
        "the public pixel type refuses a non-finite component before upload");

    // Subnormal source: rejected whole-frame until denormal preservation is qualified.
    const std::vector<Rgba32f> subnormal{
        pixel(std::numeric_limits<float>::denorm_min(), 0.0F, 0.0F, 1.0F)};
    const auto subnormalResult = runGpu(display, subnormal, 1ULL << 30ULL);
    expectations.expect(!subnormalResult.success &&
                            subnormalResult.code == GpuNeutralDisplayDiagnosticCode::ShaderRejected,
                        "a subnormal frame is rejected whole-frame");
    expectations.expect(cpuOracle(handle, subnormal, 1, 1).has_value(),
                        "CPU oracle still produces the subnormal frame");
}

void testCancelAndReuse(Expectations& expectations, GpuNeutralDisplay& display) {
    const std::vector<Rgba32f> pixels = randomFixture(257, 99);
    const auto begin = display.begin(pixels, 1ULL << 30ULL);
    expectations.expect(begin.code == GpuNeutralDisplayDiagnosticCode::None,
                        "the cancellable begin is accepted");
    display.cancel();
    GpuNeutralDisplayPollResult poll = GpuNeutralDisplayPollResult::Pending;
    while (poll == GpuNeutralDisplayPollResult::Pending) {
        poll = display.poll();
        if (poll == GpuNeutralDisplayPollResult::Pending) {
            std::this_thread::yield();
        }
    }
    expectations.expect(poll == GpuNeutralDisplayPollResult::Failure &&
                            display.diagnostic().code == GpuNeutralDisplayDiagnosticCode::Cancelled,
                        "a cancelled job reports Cancelled and publishes nothing");
    expectations.expect(display.readback().pixels.empty(),
                        "readback after cancellation yields no frame");

    // Reuse: the same pipeline runs a larger job, then a small one. Capacity is retained, never
    // shrunk, so the small job reuses the grown buffers.
    const std::vector<Rgba32f> larger = randomFixture(4096, 1234);
    const auto reuse = runGpu(display, larger, 1ULL << 30ULL);
    expectations.expect(reuse.success, "the pipeline is reusable after a cancelled job");
    const std::vector<Rgba32f> small = randomFixture(64, 55);
    const auto shrink = runGpu(display, small, 1ULL << 30ULL);
    expectations.expect(shrink.success && shrink.pixels.size() == 64,
                        "a smaller job reuses retained capacity");
}

// Every exposed Vulkan operation is owner-thread-only. A joined worker calling begin() must fail
// closed with WrongThread and must not touch the queue.
void testWrongThreadFailsClosed(Expectations& expectations, GpuNeutralDisplay& display) {
    const std::vector<Rgba32f> pixels = randomFixture(16, 3);
    auto observed = GpuNeutralDisplayDiagnosticCode::None;
    std::thread worker(
        [&display, &pixels, &observed]() { observed = display.begin(pixels, 1ULL << 30ULL).code; });
    worker.join();
    expectations.expect(observed == GpuNeutralDisplayDiagnosticCode::WrongThread,
                        "begin from a joined non-owner thread fails closed");
    expectations.expect(display.state() == GpuNeutralDisplayJobState::Idle,
                        "a wrong-thread begin leaves the pipeline idle");
}

// Core regression: a wrong-thread poll() must NOT mutate the live job. The owner submitted a real
// GPU job; a joined foreign poll must return Failure without touching owned state, and the owner
// must then still complete and read back the correct frame.
void testWrongThreadPollDoesNotLoseJob(
    Expectations& expectations, GpuNeutralDisplay& display,
    const bloom::color::PreparedCpuDisplayProcessorHandle& handle) {
    const std::vector<Rgba32f> pixels = randomFixture(1024, 909);
    const auto begin = display.begin(pixels, 1ULL << 30ULL);
    expectations.expect(begin.code == GpuNeutralDisplayDiagnosticCode::None,
                        "the owner submits a real GPU job before the foreign poll");
    if (begin.code != GpuNeutralDisplayDiagnosticCode::None) {
        return;
    }
    auto foreignPoll = GpuNeutralDisplayPollResult::Ready;
    std::thread worker([&display, &foreignPoll]() { foreignPoll = display.poll(); });
    worker.join();
    expectations.expect(foreignPoll == GpuNeutralDisplayPollResult::Failure,
                        "a wrong-thread poll returns Failure");
    expectations.expect(display.state() == GpuNeutralDisplayJobState::Pending,
                        "a wrong-thread poll leaves the owner's job Pending, not Failed");

    GpuNeutralDisplayPollResult poll = GpuNeutralDisplayPollResult::Pending;
    while (poll == GpuNeutralDisplayPollResult::Pending) {
        poll = display.poll();
        std::this_thread::yield();
    }
    expectations.expect(poll == GpuNeutralDisplayPollResult::Ready,
                        "the owner's job still completes after the foreign poll");
    auto readback = display.readback();
    expectations.expect(readback.hasValue() && readback.pixels.size() == pixels.size(),
                        "the owner reads back the frame the foreign poll did not lose");
    if (readback.hasValue()) {
        const auto cpu = cpuOracle(handle, pixels, 1024, 1);
        expectations.expect(cpu.has_value(), "the wrong-thread-poll frame has a CPU oracle");
        if (cpu.has_value()) {
            expectations.expect(
                compareToOracle(expectations, "wrong-thread-poll", readback.pixels, *cpu) == 0,
                "the wrong-thread-poll frame matches the CPU oracle");
        }
    }
}

#if defined(BLOOM_RENDER_TEST_VULKAN_BACKEND)
// A pipeline's Vulkan objects live on the device owner thread, so create() from a foreign thread
// must fail closed with WrongThread and must not build a pipeline or disturb the device.
void testWrongThreadCreateFailsClosed(Expectations& expectations, GpuDevice& device) {
    auto observed = GpuNeutralDisplayDiagnosticCode::None;
    std::thread worker([&device, &observed]() {
        auto created = GpuNeutralDisplay::create(device);
        observed = created ? GpuNeutralDisplayDiagnosticCode::None : created.diagnostic.code;
    });
    worker.join();
    expectations.expect(observed == GpuNeutralDisplayDiagnosticCode::WrongThread,
                        "create from a joined non-owner thread fails closed");
    expectations.expect(device.state() == GpuDeviceState::Ready,
                        "the device remains Ready after a wrong-thread create");
    // The original device is still usable: a correct owner-thread create succeeds.
    auto created = GpuNeutralDisplay::create(device);
    expectations.expect(created.hasValue(),
                        "the owner thread can still create a pipeline after a wrong-thread create");
}

// Unknown fence status must not be treated as retired: the submission stays unretired, begin() must
// keep refusing Busy, and a later successful owner poll retires the submission so the pipeline is
// reusable and destruction proceeds without a drain. This exercises the seam directly.
void testUnknownFenceStatusKeepsRetirement(Expectations& expectations, GpuDevice& device) {
    const std::vector<Rgba32f> pixels = randomFixture(512, 17);
    auto created = GpuNeutralDisplay::create(device);
    expectations.expect(created.hasValue(), "a pipeline is created for the unknown-fence test");
    if (!created) {
        return;
    }
    auto display = std::move(created.display);
    expectations.expect(display->begin(pixels, 1ULL << 30ULL).code ==
                            GpuNeutralDisplayDiagnosticCode::None,
                        "the unknown-fence job is submitted");
    bloom::render::neutral_display_detail::setFenceOverride(
        bloom::render::neutral_display_detail::FenceOverride::Unknown);
    const auto poll = display->poll();
    expectations.expect(poll == GpuNeutralDisplayPollResult::Failure,
                        "an unknown fence status reports Failure");
    // The submission is still live, so begin must refuse rather than reset/free busy resources.
    expectations.expect(display->begin(pixels, 1ULL << 30ULL).code ==
                            GpuNeutralDisplayDiagnosticCode::Busy,
                        "begin while a failed submission is unretired reports Busy");
    // A later successful owner poll retires the submission and makes the pipeline reusable. Drive
    // that with a bounded poll+begin retry: begin returns Busy until the real fence is observed
    // signalled, then accepts.
    bloom::render::neutral_display_detail::setFenceOverride(
        bloom::render::neutral_display_detail::FenceOverride::None);
    GpuNeutralDisplayDiagnostic beginAgain{};
    for (int attempt = 0; attempt < 1000000; ++attempt) {
        beginAgain = display->begin(pixels, 1ULL << 30ULL);
        if (beginAgain.code != GpuNeutralDisplayDiagnosticCode::Busy) {
            break;
        }
        [[maybe_unused]] const auto retirementPoll = display->poll();
        std::this_thread::yield();
    }
    expectations.expect(beginAgain.code == GpuNeutralDisplayDiagnosticCode::None,
                        "begin is accepted once a later owner poll retires the submission");
    if (beginAgain.code == GpuNeutralDisplayDiagnosticCode::None) {
        GpuNeutralDisplayPollResult finish = GpuNeutralDisplayPollResult::Pending;
        while (finish == GpuNeutralDisplayPollResult::Pending) {
            finish = display->poll();
            std::this_thread::yield();
        }
        expectations.expect(finish == GpuNeutralDisplayPollResult::Ready,
                            "the retired-then-reused pipeline completes the new job");
        expectations.expect(display->readback().hasValue(),
                            "the reused pipeline publishes the new frame");
    }
    display.reset();
    expectations.expect(!GpuNeutralDisplay::teardownDrainIncomplete(),
                        "an unknown fence status does not strand an unretired submission");
}
#endif

// Destroying a pipeline with an in-flight job on the owner thread drains it (bounded) before
// releasing Vulkan resources, and reports no abandoned generation.
void testTeardownDrainsInflight(Expectations& expectations, GpuDevice& device) {
    const std::vector<Rgba32f> pixels = randomFixture(1024, 71);
    auto created = GpuNeutralDisplay::create(device);
    expectations.expect(created.hasValue(), "a second pipeline is created for the teardown test");
    if (!created) {
        return;
    }
    auto display = std::move(created.display);
    expectations.expect(display->begin(pixels, 1ULL << 30ULL).code ==
                            GpuNeutralDisplayDiagnosticCode::None,
                        "the teardown job is submitted");
    display.reset(); // destroy while Pending, on the owner thread
    expectations.expect(!GpuNeutralDisplay::teardownDrainIncomplete(),
                        "owner-thread teardown drained the in-flight job without abandoning it");
}

void testStubOrUnavailable(Expectations& expectations) {
    // With no device, create() must fail closed with a typed diagnostic and begin() must not crash.
    GpuDeviceCreationOptions options;
    options.loader_path = "/nonexistent/bloom-missing-vulkan-loader";
    const auto device = GpuDevice::create(options);
    expectations.expect(!device, "a forced missing loader yields no device");
#if defined(BLOOM_RENDER_TEST_VULKAN_BACKEND)
    expectations.expect(device.diagnostic.code ==
                            bloom::render::GpuDiagnosticCode::LoaderUnavailable,
                        "the Vulkan backend reports LoaderUnavailable");
#else
    expectations.expect(device.diagnostic.code == bloom::render::GpuDiagnosticCode::BackendNotBuilt,
                        "the CPU stub reports BackendNotBuilt");
#endif
}

// Fair, production-comparable benchmark. Both paths consume the same preconstructed source image
// (built OUTSIDE every timed region) and both allocate their own output each iteration. The CPU
// path calls produceBloomNeutralDisplayFrame directly with the production chunk default, exactly as
// CpuQualifiedDisplayPreparer does, and its result is read in-place without an extra vector copy.
// 5 warmups then 20 interleaved pairs alternate GPU/CPU order; each side's reported number is the
// median of the FULL per-pair interval (GPU: begin through readback; CPU: the full produce call).
void runBenchmark(GpuNeutralDisplay& display,
                  const bloom::color::PreparedCpuDisplayProcessorHandle& handle,
                  const std::uint32_t width, const std::uint32_t height) {
    const std::size_t count = static_cast<std::size_t>(width) * height;
    const std::vector<Rgba32f> source = randomFixture(count, 20260919);
    const auto image = makeImage(source, width, height);
    if (!image.has_value()) {
        std::cerr << "benchmark fixture image failed\n";
        return;
    }
    const auto view = image->view();
    if (!view) {
        std::cerr << "benchmark fixture view failed\n";
        return;
    }
    const std::uint64_t budget = 1ULL << 30ULL;
    // Production default from runtime/qualified_display_preparation.hpp.
    constexpr std::size_t kChunkPixelCount = 65536;

    const auto runGpuOnce = [&display, &source]() -> bool {
        const auto beginStart = std::chrono::steady_clock::now();
        if (display.begin(source, budget).code != GpuNeutralDisplayDiagnosticCode::None) {
            return false;
        }
        auto poll = GpuNeutralDisplayPollResult::Pending;
        while (poll == GpuNeutralDisplayPollResult::Pending) {
            poll = display.poll();
        }
        if (poll != GpuNeutralDisplayPollResult::Ready) {
            return false;
        }
        auto readback = display.readback();
        const auto end = std::chrono::steady_clock::now();
        if (!readback || readback.pixels.empty()) {
            return false;
        }
        g_benchmarkSink += readback.pixels[0].red;
        g_benchmarkGpuMs = std::chrono::duration<double, std::milli>(end - beginStart).count();
        return true;
    };
    const auto runCpuOnce = [&handle, &view]() -> bool {
        const auto start = std::chrono::steady_clock::now();
        auto result = bloom::color::produceBloomNeutralDisplayFrame(
            handle, *view.value(), kChunkPixelCount, 1ULL << 30ULL);
        const auto end = std::chrono::steady_clock::now();
        if (!result) {
            return false;
        }
        g_benchmarkSink += result.value()->pixels()[0].red;
        g_benchmarkCpuMs = std::chrono::duration<double, std::milli>(end - start).count();
        return true;
    };

    constexpr int kWarmups = 5;
    constexpr int kPairs = 20;
    for (int iteration = 0; iteration < kWarmups; ++iteration) {
        if (!runGpuOnce() || !runCpuOnce()) {
            std::cerr << "benchmark warmup failed\n";
            return;
        }
    }
    std::vector<double> gpuMs;
    std::vector<double> cpuMs;
    for (int pair = 0; pair < kPairs; ++pair) {
        if ((pair % 2) == 0) {
            if (!runGpuOnce()) {
                std::cerr << "benchmark GPU run failed\n";
                return;
            }
            gpuMs.push_back(g_benchmarkGpuMs);
            if (!runCpuOnce()) {
                std::cerr << "benchmark CPU run failed\n";
                return;
            }
            cpuMs.push_back(g_benchmarkCpuMs);
        } else {
            if (!runCpuOnce()) {
                std::cerr << "benchmark CPU run failed\n";
                return;
            }
            cpuMs.push_back(g_benchmarkCpuMs);
            if (!runGpuOnce()) {
                std::cerr << "benchmark GPU run failed\n";
                return;
            }
            gpuMs.push_back(g_benchmarkGpuMs);
        }
    }
    const auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    std::cout << "BENCH " << width << 'x' << height << " px=" << count
              << " gpu_full_begin_to_readback_ms=" << std::fixed << std::setprecision(3)
              << median(gpuMs) << " cpu_full_produce_ms=" << median(cpuMs) << " pairs=" << kPairs
              << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }

        Expectations expectations;
        testEmbeddedSpirvDigest(expectations);
        auto handle = buildCpuHandle();
        expectations.expect(handle.has_value(), "the Bloom Neutral CPU oracle handle builds");
        if (!handle.has_value()) {
            std::cerr << "FAIL: the CPU oracle could not be built; cannot compare\n";
            return 1;
        }

        testCpuOracleOnly(expectations, *handle);
        testStubOrUnavailable(expectations);

#if !defined(BLOOM_RENDER_TEST_VULKAN_BACKEND)
        std::cout << "SKIP: Bloom was built without Vulkan dependencies; the GPU operation is "
                     "unavailable and the CPU oracle above still passed\n";
        return expectations.ok() ? 0 : 1;
#else
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                expectations.expect(false,
                                    "a device was required but the probe returned Unavailable");
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return expectations.ok() ? 0 : 1;
        }
        expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
        expectations.expect(
            device.device->qualificationFor(GpuOperationId::OcioDisplayV1, GpuPrecision::Rgba32f) ==
                GpuQualification::Unavailable,
            "OcioDisplayV1 remains Unavailable; compile-time presence is not qualification");

        auto created = GpuNeutralDisplay::create(*device.device);
        expectations.expect(created.hasValue(),
                            "the neutral display pipeline is created from the device");
        if (!created) {
            std::cerr << "FAIL: pipeline creation failed: " << created.diagnostic.message << '\n';
            return 1;
        }
        GpuNeutralDisplay& display = *created.display;
        std::cout
            << "GPU neutral display pipeline ready (embedded SPIR-V, unqualified operation)\n";

        const Fixture single{"1-pixel", 1, 1, {pixel(0.25F, 0.5F, 0.75F, 1.0F)}};
        runFixture(expectations, display, *handle, single);

        Fixture odd{"odd-257", 257, 1, randomFixture(257, 11)};
        runFixture(expectations, display, *handle, odd);

        Fixture transparent{
            "special-values",
            6,
            1,
            {pixel(0.7F, 0.7F, 0.7F, 0.0F),             // transparent, nonzero premultiplied RGB
             pixel(0.0F, 0.0F, 0.0F, -0.0F),            // negative zero alpha
             pixel(-0.1F, -0.05F, -0.02F, 1.0F),        // negative HDR
             pixel(4.0F, 2.0F, 1.0F, 1.0F),             // large HDR
             pixel(1.0e-6F, 2.0e-6F, 3.0e-6F, 1.0e-6F), // tiny alpha
             pixel(0.5F, 0.5F, 0.5F, 0.5F)}};           // exact half alpha
        runFixture(expectations, display, *handle, transparent);

        runFixture(expectations, display, *handle, alphaBoundaryFixture());

        Fixture hd{"1280x720", 1280, 720,
                   randomFixture(static_cast<std::size_t>(1280U) * 720U, 2222)};
        runFixture(expectations, display, *handle, hd);
        Fixture fhd{"1920x1080", 1920, 1080,
                    randomFixture(static_cast<std::size_t>(1920U) * 1080U, 3333)};
        runFixture(expectations, display, *handle, fhd);

        testErrorAndBudgetPaths(expectations, display, *handle);
        testCancelAndReuse(expectations, display);
        testWrongThreadFailsClosed(expectations, display);
        testWrongThreadPollDoesNotLoseJob(expectations, display, *handle);
#if defined(BLOOM_RENDER_TEST_VULKAN_BACKEND)
        testWrongThreadCreateFailsClosed(expectations, *device.device);
        testUnknownFenceStatusKeepsRetirement(expectations, *device.device);
#endif
        testTeardownDrainsInflight(expectations, *device.device);

        if (options.benchmark) {
            runBenchmark(display, *handle, 1280, 720);
            runBenchmark(display, *handle, 1920, 1080);
            runBenchmark(display, *handle, 3840, 2160);
        }

        return expectations.ok() ? 0 : 1;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
