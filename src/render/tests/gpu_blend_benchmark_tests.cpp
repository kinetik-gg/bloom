// Cold/warm per-operation benchmark for GpuBlend, split into its own translation unit so the
// native test sources stay under the source-size budget. Every GPU interval is begin+poll+
// completion of one resident dispatch; upload and readback are measured and reported separately and
// are never inside a GPU interval. Pixel parity is validated outside timing.

#include "gpu_blend_native_support.hpp"

#include <bloom/core/blend_mode.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <array>
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
using bloom::render::GpuBlendPollResult;
using bloom::render::blend_proof::blendPixels;

// Per-operation timing against the CPU oracle at 720p and 1080p. Every GPU interval is
// begin+poll+completion of one resident dispatch; upload and readback are measured and reported
// separately and are NEVER inside a GPU interval. Pixel parity is validated outside timing on a
// separate dispatch, so timing cannot be accused of hiding a readback. This is a single operation's
// latency, never a general application FPS.
void benchmarkMode(Expectations& expectations, GpuBlend& blend, GpuImageUpload& uploader,
                   const std::uint32_t width, const std::uint32_t height) {
    const auto sourceWindow = window(0, 0, width, height);
    const auto sourcePixels = blendPixels(width, height, true);
    const auto destPixels = blendPixels(width, height, false);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    auto destImage = makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), destPixels);
    if (!sourceImage || !destImage) {
        expectations.expect(false, "benchmark images build");
        return;
    }
    // Upload boundary: both resident inputs are uploaded and measured before any dispatch timing.
    const auto uploadStarted = std::chrono::steady_clock::now();
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    auto destResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*destImage)));
    const auto uploadFinished = std::chrono::steady_clock::now();
    const double uploadMs =
        std::chrono::duration<double, std::milli>(uploadFinished - uploadStarted).count();
    if (!sourceResident || !destResident) {
        expectations.expect(false, "benchmark images upload");
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto destination = std::make_shared<const GpuImage>(std::move(*destResident));

    // A resident dispatch: begin + non-blocking poll to completion + takeImage. No readback.
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
    double warm = -1.0;
    for (int i = 0; i < 5; ++i) {
        const double sample = runOnce();
        if (sample >= 0.0 && (warm < 0.0 || sample < warm)) {
            warm = sample;
        }
    }

    // Parity outside timing: one fresh dispatch, then a readback measured separately.
    if (blend.beginBlend({source, destination, BlendMode::Overlay}, kBudget).code !=
        GpuBlendDiagnosticCode::None) {
        expectations.expect(false, "benchmark parity dispatch accepted");
        return;
    }
    GpuBlendPollResult poll = GpuBlendPollResult::Pending;
    while (poll == GpuBlendPollResult::Pending) {
        poll = blend.poll();
    }
    expectations.expect(poll == GpuBlendPollResult::Ready, "benchmark parity dispatch completes");
    auto result = blend.takeImage();
    const auto readbackStarted = std::chrono::steady_clock::now();
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    const auto readbackFinished = std::chrono::steady_clock::now();
    const double readbackMs =
        std::chrono::duration<double, std::milli>(readbackFinished - readbackStarted).count();
    expectations.expect(readback.hasValue(), "benchmark readback succeeds");
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
        static_cast<void>(pixelsMatch(readback.pixels, cpuDest, expectations,
                                      "benchmark parity " + std::to_string(width) + "x" +
                                          std::to_string(height)));
    }

    // CPU oracle timing for the identical operation.
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
    const double cpuMs =
        std::chrono::duration<double, std::milli>(cpuFinished - cpuStarted).count();

    expectations.expect(cold > 0.0 && warm > 0.0, "benchmark dispatch completes");
    std::cout << "BENCH BlendV1/Overlay " << width << 'x' << height << " upload=" << uploadMs
              << "ms cold=" << cold << "ms warm=" << warm << "ms readback=" << readbackMs
              << "ms cpu=" << cpuMs << "ms\n";
}

} // namespace

namespace bloom::render::blend_proof {

void runBlendBenchmark(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "benchmark hosts created");
        return;
    }
    benchmarkMode(expectations, *blend.blend, *uploader.upload, 1280, 720);
    benchmarkMode(expectations, *blend.blend, *uploader.upload, 1920, 1080);
}

} // namespace bloom::render::blend_proof
