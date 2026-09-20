// Honest cost measurement for AffineBilinearV1: the complete GPU operation (host inverse-map
// metadata preparation inside beginAffine + dispatch + fence completion) versus the CPU oracle
// (render::layerTransformBilinearRow) at 720p and 1080p, plus the separate source-upload path.
//
// It reports wall-clock milliseconds; it does not fabricate a provider or relax a tolerance. Run it
// in a Release build for meaningful numbers (#ifdef NDEBUG prints the mode). It needs a live device
// and skips cleanly otherwise.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_affine.hpp>

#include "gpu_image_private.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuAffine;
using bloom::render::GpuAffineDiagnosticCode;
using bloom::render::GpuAffinePollResult;
using bloom::render::LayerTransform;
using bloom::render::Rgba32f;

template <typename Function>
[[nodiscard]] double bestMilliseconds(Function&& function, const int iterations) {
    double best = std::numeric_limits<double>::infinity();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        function();
        const auto finish = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = finish - start;
        best = std::min(best, elapsed.count());
    }
    return best;
}

void measureResolution(Expectations& expectations, GpuAffine& affine, GpuImageUpload& uploader,
                       const std::uint32_t width, const std::uint32_t height,
                       const int iterations) {
    const auto label = std::to_string(width) + "x" + std::to_string(height);
    const auto sourceWindow = window(0, 0, width, height);
    const auto outputWindow = window(0, 0, width, height);
    const auto pixels = semanticPixels(width, height);
    // A general (non-translation-only) affine placement so the host inverseMap and the shader
    // bilinear both take the full path.
    const auto authored = LayerTransform::create({.translationX = 1.5,
                                                  .translationY = -2.5,
                                                  .scaleX = 1.001,
                                                  .scaleY = 1.001,
                                                  .rotationDegrees = 7.0,
                                                  .opacity = 1.0},
                                                 sourceWindow, 1.0, 1.0);
    expectations.expect(static_cast<bool>(authored), label + ": transform valid");
    if (!authored) {
        return;
    }
    const auto transform = *authored.value();

    auto sourceImage = makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), pixels);
    if (!sourceImage) {
        expectations.expect(false, label + ": source builds");
        return;
    }
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    expectations.expect(sourceResident.has_value(), label + ": source uploads");
    if (!sourceResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));

    // CPU oracle: the real row primitive over every output row.
    auto oracleImage = makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), pixels);
    if (!oracleImage) {
        return;
    }
    const auto view = oracleImage->view();
    if (!view) {
        return;
    }
    std::vector<Rgba32f> rowBuffer(width, Rgba32f::transparent());
    const auto cpuOnce = [&] {
        for (std::uint32_t y = 0; y < height; ++y) {
            static_cast<void>(bloom::render::layerTransformBilinearRow(
                *view.value(), outputWindow, outputWindow.originY() + static_cast<std::int64_t>(y),
                transform, rowBuffer));
        }
    };

    // Complete GPU operation with the source already resident: beginAffine (host metadata + image
    // allocation + submit) through poll() Ready (fence retirement).
    std::uint64_t lastAllocationBytes = 0;
    const auto gpuResidentOnce = [&] {
        const auto begin = affine.beginAffine({source, outputWindow, transform}, kBudget);
        expectations.expect(begin.code == GpuAffineDiagnosticCode::None,
                            label + ": GPU begin accepted: " + begin.message);
        GpuAffinePollResult poll = GpuAffinePollResult::Pending;
        while (poll == GpuAffinePollResult::Pending) {
            poll = affine.poll();
        }
        expectations.expect(poll == GpuAffinePollResult::Ready, label + ": GPU completes");
        lastAllocationBytes = affine.lastJobAllocationBytes();
        auto image = affine.takeImage();
        expectations.expect(image.isValid(), label + ": GPU image published");
    };

    // The upload path: host image construction + staged GPU upload + fence, measured separately.
    const auto uploadOnce = [&] {
        auto fresh = makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), pixels);
        if (!fresh) {
            return;
        }
        static_cast<void>(
            upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*fresh))));
    };

    const double cpuMs = bestMilliseconds(cpuOnce, iterations);
    const double gpuResidentMs = bestMilliseconds(gpuResidentOnce, iterations);
    const double uploadMs = bestMilliseconds(uploadOnce, iterations);
    const auto metadataBytes = static_cast<std::uint64_t>(width) * height * 16ULL;
    std::cout << "affine " << label << " (best of " << iterations << "):"
              << " metadata=" << metadataBytes << "B"
              << " cpuOracle=" << cpuMs << "ms"
              << " gpuCompleteResident=" << gpuResidentMs << "ms"
              << " uploadPath=" << uploadMs << "ms"
              << " gpuCompleteWithUpload=" << (gpuResidentMs + uploadMs) << "ms"
              << " lastJobAllocation=" << lastAllocationBytes << "B\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
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
            std::cout << "SKIP: no compatible Vulkan device available\n";
            return 0;
        }
        auto affine = GpuAffine::create(*device.device);
        auto uploader = GpuImageUpload::create(*device.device);
        expectations.expect(affine.hasValue() && uploader.hasValue(), "cost hosts created");
        if (!affine || !uploader) {
            return 1;
        }
#ifdef NDEBUG
        std::cout << "build mode: Release\n";
#else
        std::cout << "build mode: Debug (Release numbers are the meaningful ones)\n";
#endif
        measureResolution(expectations, *affine.affine, *uploader.upload, 1280, 720, 5);
        measureResolution(expectations, *affine.affine, *uploader.upload, 1920, 1080, 3);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " affine cost expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: affine cost measured\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
