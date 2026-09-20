#pragma once

// Shared helpers for the native GpuBlend tests, split out so each translation unit stays under the
// source-size budget. Reuses the composite proof's Expectations/upload/solid fixtures.

#include "gpu_composite_native_support.hpp"

#include <bloom/core/blend_mode.hpp>
#include <bloom/render/gpu_blend.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render::blend_proof {

using bloom::render::composite_proof::Expectations;
using bloom::render::composite_proof::GpuDevice;
using bloom::render::composite_proof::GpuImageUpload;
using bloom::render::composite_proof::kBudget;
using bloom::render::composite_proof::makeImage;
using bloom::render::composite_proof::Options;
using bloom::render::composite_proof::parseOptions;
using bloom::render::composite_proof::pixel;
using bloom::render::composite_proof::PixelAspectRatio;
using bloom::render::composite_proof::Rgba32f;
using bloom::render::composite_proof::Rgba32fImage;
using bloom::render::composite_proof::upload;
using bloom::render::composite_proof::window;

// A fixture field that covers alpha endpoints (0, partial, 1), negative RGB, HDR RGB, and an odd
// extent. Straight values are converted to premultiplied by the caller through pixel().
[[nodiscard]] inline std::vector<Rgba32f>
blendPixels(const std::uint32_t width, const std::uint32_t height, const bool source) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto fx = static_cast<float>(x) / static_cast<float>(width);
            const auto fy = static_cast<float>(y) / static_cast<float>(height);
            const auto alpha = (x % 3 == 0) ? 0.0F : ((x % 3 == 1) ? 0.5F : 1.0F);
            if (source) {
                pixels[static_cast<std::size_t>(y) * width + x] =
                    pixel(1.5F * fx - 0.25F, -0.4F + fy, 2.5F * fx, alpha);
            } else {
                pixels[static_cast<std::size_t>(y) * width + x] =
                    pixel(0.2F + 0.5F * fx, 0.75F - 0.5F * fy, -0.1F + fx * fy, alpha);
            }
        }
    }
    return pixels;
}

// The cold/warm benchmark lives in its own translation unit; declared here for the test driver.
void runBlendBenchmark(Expectations& expectations, GpuDevice& device);

// The forced-portable Float32 proof lives in its own translation unit; declared here for the
// driver.
void runPortableBlendTests(Expectations& expectations, GpuDevice& device);

} // namespace bloom::render::blend_proof
