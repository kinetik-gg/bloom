// Transfer-inclusive per-operation benchmark for GpuBlend, split into its own translation unit so
// the native test sources stay under the source-size budget.
//
// Every dispatch interval is begin + non-blocking poll to completion + takeImage. Upload and
// readback are measured separately and are also folded into a transfer-inclusive total, because the
// architecture contract requires the transfer cost to be visible when a path is chosen. Warm and
// CPU figures are medians of seven runs (the documented minimum is five); pixel parity is validated
// outside timing on a separate dispatch. This is one operation's latency, never an application FPS.

#include "gpu_blend_native_support.hpp"

#include <bloom/core/blend_mode.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::core::BlendMode;
using bloom::render::GpuBlend;
using bloom::render::GpuBlendDiagnosticCode;
using bloom::render::GpuBlendKernelPolicy;
using bloom::render::GpuBlendPollResult;
using bloom::render::blend_proof::blendPixels;

constexpr int kWarmRuns = 7;
constexpr int kCpuRuns = 7;

[[nodiscard]] double median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto count = samples.size();
    return count % 2 == 1 ? samples[count / 2]
                          : 0.5 * (samples[count / 2 - 1] + samples[count / 2]);
}

void benchmarkMode(Expectations& expectations, GpuBlend& blend, GpuImageUpload& uploader,
                   const std::uint32_t width, const std::uint32_t height,
                   const std::string& label) {
    const auto sourceWindow = window(0, 0, width, height);
    const auto sourcePixels = blendPixels(width, height, true);
    const auto destPixels = blendPixels(width, height, false);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    auto destImage = makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), destPixels);
    if (!sourceImage || !destImage) {
        expectations.expect(false, label + ": benchmark images build");
        return;
    }
    const auto uploadStarted = std::chrono::steady_clock::now();
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    auto destResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*destImage)));
    const auto uploadFinished = std::chrono::steady_clock::now();
    const double uploadMs =
        std::chrono::duration<double, std::milli>(uploadFinished - uploadStarted).count();
    if (!sourceResident || !destResident) {
        expectations.expect(false, label + ": benchmark images upload");
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto destination = std::make_shared<const GpuImage>(std::move(*destResident));

    const auto runOnce = [&]() -> double {
        const auto started = std::chrono::steady_clock::now();
        if (blend.beginBlend({source, destination, BlendMode::Overlay}, kBudget).code !=
            GpuBlendDiagnosticCode::None) {
            return -1.0;
        }
        GpuBlendPollResult poll = GpuBlendPollResult::Pending;
        while (poll == GpuBlendPollResult::Pending) {
            poll = blend.poll();
        }
        if (poll != GpuBlendPollResult::Ready) {
            return -1.0;
        }
        static_cast<void>(blend.takeImage());
        const auto finished = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(finished - started).count();
    };
    const double cold = runOnce();
    std::vector<double> warmSamples;
    for (int index = 0; index < kWarmRuns; ++index) {
        const double sample = runOnce();
        if (sample >= 0.0) {
            warmSamples.push_back(sample);
        }
    }
    const double warmMedian = warmSamples.empty() ? -1.0 : median(warmSamples);

    // Parity outside timing: one fresh dispatch, then a readback measured separately.
    if (blend.beginBlend({source, destination, BlendMode::Overlay}, kBudget).code !=
        GpuBlendDiagnosticCode::None) {
        expectations.expect(false, label + ": benchmark parity dispatch accepted");
        return;
    }
    GpuBlendPollResult poll = GpuBlendPollResult::Pending;
    while (poll == GpuBlendPollResult::Pending) {
        poll = blend.poll();
    }
    expectations.expect(poll == GpuBlendPollResult::Ready,
                        label + ": benchmark parity dispatch completes");
    auto result = blend.takeImage();
    const auto readbackStarted = std::chrono::steady_clock::now();
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    const auto readbackFinished = std::chrono::steady_clock::now();
    const double readbackMs =
        std::chrono::duration<double, std::milli>(readbackFinished - readbackStarted).count();
    expectations.expect(readback.hasValue(), label + ": benchmark readback succeeds");
    std::vector<Rgba32f> cpuDest(destPixels);
    std::vector<Rgba32f> sourceRow(width, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            sourceRow[x] = sourcePixels[static_cast<std::size_t>(y) * width + x];
        }
        auto destRow =
            std::span<Rgba32f>(cpuDest).subspan(static_cast<std::size_t>(y) * width, width);
        static_cast<void>(
            bloom::render::blendLinearRec709SceneRow(BlendMode::Overlay, sourceRow, destRow));
    }
    if (readback) {
        static_cast<void>(
            pixelsMatch(readback.pixels, cpuDest, expectations,
                        label + " parity " + std::to_string(width) + "x" + std::to_string(height)));
    }

    // CPU oracle timing for the identical operation, median of seven runs.
    std::vector<double> cpuSamples;
    for (int index = 0; index < kCpuRuns; ++index) {
        std::vector<Rgba32f> cpuTimed(destPixels);
        const auto cpuStarted = std::chrono::steady_clock::now();
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                sourceRow[x] = sourcePixels[static_cast<std::size_t>(y) * width + x];
            }
            auto destRow =
                std::span<Rgba32f>(cpuTimed).subspan(static_cast<std::size_t>(y) * width, width);
            static_cast<void>(
                bloom::render::blendLinearRec709SceneRow(BlendMode::Overlay, sourceRow, destRow));
        }
        const auto cpuFinished = std::chrono::steady_clock::now();
        cpuSamples.push_back(
            std::chrono::duration<double, std::milli>(cpuFinished - cpuStarted).count());
    }
    const double cpuMedian = median(cpuSamples);

    expectations.expect(cold > 0.0 && warmMedian > 0.0, label + ": benchmark dispatch completes");
    const double transferCold = uploadMs + cold + readbackMs;
    const double transferWarm = uploadMs + warmMedian + readbackMs;
    std::cout << "BENCH BlendV1/Overlay " << label << ' ' << width << 'x' << height
              << " upload=" << uploadMs << "ms cold=" << cold << "ms warmMedian(" << kWarmRuns
              << ")=" << warmMedian << "ms readback=" << readbackMs
              << "ms transferCold=" << transferCold << "ms transferWarm=" << transferWarm
              << "ms cpuMedian(" << kCpuRuns << ")=" << cpuMedian << "ms"
              << " speedupWarm=" << (warmMedian > 0.0 ? cpuMedian / warmMedian : 0.0)
              << "x transferWarmVsCpu=" << (transferWarm > 0.0 ? cpuMedian / transferWarm : 0.0)
              << "x\n";
}

} // namespace

namespace bloom::render::blend_proof {

void runBlendBenchmark(Expectations& expectations, GpuDevice& device) {
    for (const auto policy : {GpuBlendKernelPolicy::Auto, GpuBlendKernelPolicy::PortableFloat32}) {
        auto blend = GpuBlend::create(device, {}, policy);
        auto uploader = GpuImageUpload::create(device);
        if (!blend || !uploader) {
            expectations.expect(false, "benchmark hosts created");
            continue;
        }
        const std::string tag = policy == GpuBlendKernelPolicy::Auto ? "auto" : "portable";
        benchmarkMode(expectations, *blend.blend, *uploader.upload, 1280, 720, tag + "-720p");
        benchmarkMode(expectations, *blend.blend, *uploader.upload, 1920, 1080, tag + "-1080p");
    }
}

} // namespace bloom::render::blend_proof
