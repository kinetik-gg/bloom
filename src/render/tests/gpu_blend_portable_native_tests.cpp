// Real-device proof that the portable compensated-Float32 BlendV1 kernel runs the six general
// core::BlendMode values on a Float64-capable Linux device when the narrow production policy forces
// it, matching the retained CPU primitive render::blendLinearRec709SceneRow() under the documented
// per-finite-component 2e-6 absolute-or-relative gate over a deterministic wider-HDR field and tiny
// normal-alpha boundaries. Readback happens only for oracle assertions.
//
// This is a separate translation unit so the native test sources stay under the source-size budget.

#include "gpu_blend_native_support.hpp"

#include <bloom/render/gpu_blend.hpp>

// The complete GpuImageImpl so local GpuImage values can be constructed/destroyed in this TU.
#include "gpu_image_private.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render::blend_proof {
namespace {

using bloom::core::BlendMode;
using bloom::render::GpuBlend;
using bloom::render::GpuBlendDiagnosticCode;
using bloom::render::GpuBlendKernelPolicy;
using bloom::render::GpuBlendPollResult;
using bloom::render::gpuBlendShaderIdentity;
using bloom::render::ImageWindow;

// A deterministic wider-HDR field: straight (un-premultiplied) magnitudes up to 1e5 with alphas
// down to 1e-6, every fifth column cancelling exactly (destination == source). Every premultiplied
// lane stays normal, so the kernel's whole-frame subnormal rejection is not triggered.
[[nodiscard]] std::vector<Rgba32f> widerHdrField(const std::uint32_t width,
                                                 const std::uint32_t height, const bool source) {
    const std::array<float, 9> magnitudes{0.0F,   1.0e-6F, 1.0e-4F, 1.0e-2F, 1.0F,
                                          1.0e2F, 1.0e3F,  1.0e4F,  1.0e5F};
    const std::array<float, 5> alphas{1.0e-6F, 1.0e-4F, 1.0e-2F, 0.5F, 1.0F};
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto selector = x + y * width;
            const auto sourceMagnitude = magnitudes[(selector * 7U + 3U) % 9U];
            const auto destMagnitude = magnitudes[(selector * 11U + 5U) % 9U];
            const auto sourceAlpha = alphas[(selector * 3U + 1U) % 5U];
            auto destAlpha = alphas[(selector * 5U + 2U) % 5U];
            const auto sourceStraight = sourceMagnitude * (((selector % 2U) == 0U) ? 1.0F : -1.0F);
            auto destStraight = destMagnitude * ((((selector / 2U) % 2U) == 0U) ? 1.0F : -1.0F);
            if (selector % 5U == 0U) {
                destStraight = sourceStraight;
                destAlpha = sourceAlpha;
            }
            const auto straight = source ? sourceStraight : destStraight;
            const auto alpha = source ? sourceAlpha : destAlpha;
            pixels[static_cast<std::size_t>(y) * width + x] =
                pixel(straight * alpha, straight * alpha * 0.5F, straight * alpha * 0.25F, alpha);
        }
    }
    return pixels;
}

// Tiny normal-alpha boundaries: the smallest positive normal alpha and a range up to 1, with
// straight magnitudes chosen so every premultiplied lane stays normal (never subnormal).
[[nodiscard]] std::vector<Rgba32f> tinyAlphaField(const std::uint32_t width,
                                                  const std::uint32_t height, const bool source) {
    const float smallestNormal = std::numeric_limits<float>::min();
    const std::array<float, 7> alphas{smallestNormal, 1.0e-37F, 1.0e-30F, 1.0e-20F,
                                      1.0e-10F,       0.5F,     1.0F};
    const std::array<float, 4> magnitudes{4.0F, 16.0F, 64.0F, 4096.0F};
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto selector = x + y * width;
            const auto sourceMagnitude = magnitudes[(selector * 3U + 1U) % 4U];
            const auto destMagnitude = magnitudes[(selector * 5U + 2U) % 4U];
            const auto sourceAlpha = alphas[(selector * 7U + 3U) % 7U];
            auto destAlpha = alphas[(selector * 11U + 5U) % 7U];
            const auto sourceStraight = sourceMagnitude * (((selector % 2U) == 0U) ? 1.0F : -1.0F);
            auto destStraight = destMagnitude * ((((selector / 2U) % 2U) == 0U) ? 1.0F : -1.0F);
            if (selector % 5U == 0U) {
                destStraight = sourceStraight;
                destAlpha = sourceAlpha;
            }
            const auto straight = source ? sourceStraight : destStraight;
            const auto alpha = source ? sourceAlpha : destAlpha;
            pixels[static_cast<std::size_t>(y) * width + x] =
                pixel(straight * alpha, straight * alpha * 0.5F, straight * alpha * 0.25F, alpha);
        }
    }
    return pixels;
}

[[nodiscard]] bool hasNoSubnormalLane(const std::vector<Rgba32f>& pixels) {
    for (const auto& value : pixels) {
        const std::array<float, 4> lanes{value.red(), value.green(), value.blue(), value.alpha()};
        for (const auto lane : lanes) {
            const auto bits = std::fabs(lane);
            if (bits != 0.0F && bits < std::numeric_limits<float>::min()) {
                return false;
            }
        }
    }
    return true;
}

// One forced-portable aligned dispatch compared to the CPU oracle, with the exact output descriptor
// asserted from the destination.
void runAlignedMode(Expectations& expectations, GpuBlend& blend, GpuImageUpload& uploader,
                    const BlendMode mode, const std::vector<Rgba32f>& sourcePixels,
                    const std::vector<Rgba32f>& destPixels, const std::uint32_t width,
                    const std::uint32_t height, const std::string& label) {
    const auto win = window(0, 0, width, height);
    auto sourceImage = makeImage(win, win, PixelAspectRatio::square(), sourcePixels);
    auto destImage = makeImage(win, win, PixelAspectRatio::square(), destPixels);
    expectations.expect(sourceImage.has_value() && destImage.has_value(), label + ": images build");
    if (!sourceImage || !destImage) {
        return;
    }
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    auto destResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*destImage)));
    expectations.expect(sourceResident.has_value() && destResident.has_value(),
                        label + ": inputs upload");
    if (!sourceResident || !destResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto destination = std::make_shared<const GpuImage>(std::move(*destResident));

    const auto begin = blend.beginBlend({source, destination, mode}, kBudget);
    expectations.expect(begin.code == GpuBlendDiagnosticCode::None,
                        label + ": begin accepted: " + begin.message);
    if (begin.code != GpuBlendDiagnosticCode::None) {
        return;
    }
    expectations.expect(blend.hasUnretiredSubmission(),
                        label + ": a real submission is in flight (positive actual dispatch)");
    GpuBlendPollResult poll = GpuBlendPollResult::Pending;
    while (poll == GpuBlendPollResult::Pending) {
        poll = blend.poll();
    }
    expectations.expect(poll == GpuBlendPollResult::Ready,
                        label + ": completes: " + blend.diagnostic().message);
    if (poll != GpuBlendPollResult::Ready) {
        return;
    }
    expectations.expect(!blend.hasUnretiredSubmission(),
                        label + ": retirement is proven once Ready");
    auto result = blend.takeImage();
    expectations.expect(result.isValid(), label + ": resident output published");
    expectations.expect(result.dataWindow() == destination->dataWindow() &&
                            result.displayWindow() == destination->displayWindow() &&
                            result.pixelAspect() == destination->pixelAspect(),
                        label + ": output preserves the exact destination descriptor");

    std::vector<Rgba32f> expected = destPixels;
    for (std::uint32_t y = 0; y < height; ++y) {
        auto row = std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * width, width);
        const auto status =
            blendLinearRec709SceneRow(mode,
                                      std::span<const Rgba32f>(sourcePixels)
                                          .subspan(static_cast<std::size_t>(y) * width, width),
                                      row);
        expectations.expect(!status.has_value(), label + ": oracle row succeeds");
    }
    const auto readback = readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), label + ": readback for oracle");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, label));
    }
}

} // namespace

void runPortableBlendTests(Expectations& expectations, GpuDevice& device) {
    auto automatic = GpuBlend::create(device);
    auto portable = GpuBlend::create(device, {}, GpuBlendKernelPolicy::PortableFloat32);
    expectations.expect(automatic.hasValue(), "portable: Auto pipeline creates");
    expectations.expect(portable.hasValue(), "portable: forced-portable pipeline creates");
    if (!automatic || !portable) {
        return;
    }

    // The render-owned canonical identity is the actual selection. On this device Auto may be the
    // exact Float64 companion; the forced policy must always be the portable kernel, and Normal/Add
    // must stay the exact Float32 kernel under both policies.
    const auto automaticGeneral = automatic.blend->shaderIdentity(BlendMode::Multiply);
    const bool deviceHasFloat64 = automaticGeneral == "blend-v1-f64";
    expectations.expect(automaticGeneral == "blend-v1-f64" ||
                            automaticGeneral == "blend-v1-f32-portable",
                        "portable: Auto general identity is exact or portable");
    expectations.expect(portable.blend->shaderIdentity(BlendMode::Multiply) ==
                            "blend-v1-f32-portable",
                        "portable: forced policy selects the portable kernel");
    if (deviceHasFloat64) {
        expectations.expect(portable.blend->shaderIdentity(BlendMode::Multiply) != automaticGeneral,
                            "portable: forcing portable changes the identity on an f64 device");
    }
    for (const auto mode : {BlendMode::Normal, BlendMode::Add}) {
        expectations.expect(portable.blend->shaderIdentity(mode) == "blend-v1-f32" &&
                                gpuBlendShaderIdentity(mode, false) == "blend-v1-f32",
                            "portable: Normal/Add stay on the exact Float32 kernel");
    }

    auto uploader = GpuImageUpload::create(device);
    expectations.expect(uploader.hasValue(), "portable: uploader creates");
    if (!uploader) {
        return;
    }

    // All eight modes on the retained small fixture, forced through the portable pipeline.
    constexpr std::uint32_t kWidth = 7;
    constexpr std::uint32_t kHeight = 3;
    const auto smallSource = blendPixels(kWidth, kHeight, true);
    const auto smallDest = blendPixels(kWidth, kHeight, false);
    for (const auto mode : bloom::core::kBlendModes) {
        const auto value = static_cast<int>(bloom::core::blendModeStoredValue(mode));
        runAlignedMode(expectations, *portable.blend, *uploader.upload, mode, smallSource,
                       smallDest, kWidth, kHeight, "portable-small-mode-" + std::to_string(value));
    }

    // Wider HDR and tiny normal-alpha boundaries on the six general modes.
    constexpr std::uint32_t kFieldWidth = 257;
    constexpr std::uint32_t kFieldHeight = 3;
    const auto wideSource = widerHdrField(kFieldWidth, kFieldHeight, true);
    const auto wideDest = widerHdrField(kFieldWidth, kFieldHeight, false);
    expectations.expect(hasNoSubnormalLane(wideSource) && hasNoSubnormalLane(wideDest),
                        "portable: wider-HDR field has no subnormal lane");
    const auto tinySource = tinyAlphaField(kFieldWidth, kFieldHeight, true);
    const auto tinyDest = tinyAlphaField(kFieldWidth, kFieldHeight, false);
    expectations.expect(hasNoSubnormalLane(tinySource) && hasNoSubnormalLane(tinyDest),
                        "portable: tiny-alpha field has no subnormal lane");
    for (std::uint32_t modeValue = 2; modeValue <= 7; ++modeValue) {
        const auto mode = bloom::core::kBlendModes[modeValue];
        const auto tag = std::to_string(modeValue);
        runAlignedMode(expectations, *portable.blend, *uploader.upload, mode, wideSource, wideDest,
                       kFieldWidth, kFieldHeight, "portable-wider-hdr-mode-" + tag);
        runAlignedMode(expectations, *portable.blend, *uploader.upload, mode, tinySource, tinyDest,
                       kFieldWidth, kFieldHeight, "portable-tiny-alpha-mode-" + tag);
    }
}

} // namespace bloom::render::blend_proof
