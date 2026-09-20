// Real native proof for the additive GpuBlend host on a Vulkan device: an actual BlendV1 dispatch
// for every core::BlendMode value, each compared to the retained CPU primitive
// render::blendLinearRec709SceneRow() under the documented per-finite-component 2e-6
// absolute-or-relative gate.
//
// Full readback happens only for oracle assertions. The test registers only where the Vulkan
// backend is built, skips cleanly without a device, and --require-device fails closed.

#include "gpu_blend_native_support.hpp"

#include <bloom/render/gpu_blend.hpp>

// The complete GpuImageImpl so local GpuImage values can be constructed/destroyed in this TU.
#include "gpu_image_private.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;

using bloom::core::BlendMode;
using bloom::render::GpuBlend;
using bloom::render::GpuBlendDiagnosticCode;
using bloom::render::GpuBlendJobState;
using bloom::render::GpuBlendParameters;
using bloom::render::GpuBlendPollResult;
using bloom::render::blend_proof::blendPixels;
using bloom::render::blend_proof::runBlendBenchmark;

void runMode(Expectations& expectations, GpuBlend& blend, GpuImageUpload& uploader,
             const BlendMode mode, const std::string& name, const std::uint32_t width,
             const std::uint32_t height, const std::int64_t sourceOriginX,
             const std::int64_t sourceOriginY, const std::int64_t destOriginX,
             const std::int64_t destOriginY) {
    const auto sourceWindow = window(sourceOriginX, sourceOriginY, width, height);
    const auto destWindow = window(destOriginX, destOriginY, width, height);
    const auto sourcePixels = blendPixels(width, height, true);
    const auto destPixels = blendPixels(width, height, false);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    auto destImage = makeImage(destWindow, destWindow, PixelAspectRatio::square(), destPixels);
    if (!sourceImage || !destImage) {
        expectations.expect(false, name + ": fixture images build");
        return;
    }
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    auto destResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*destImage)));
    expectations.expect(sourceResident.has_value() && destResident.has_value(),
                        name + ": inputs upload");
    if (!sourceResident || !destResident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto destination = std::make_shared<const GpuImage>(std::move(*destResident));
    const auto sourceBefore = bloom::render::readbackResidentImage(*source, kBudget);
    const auto destBefore = bloom::render::readbackResidentImage(*destination, kBudget);

    const auto begin = blend.beginBlend({source, destination, mode}, kBudget);
    expectations.expect(begin.code == GpuBlendDiagnosticCode::None,
                        name + ": begin accepted: " + begin.message);
    if (begin.code != GpuBlendDiagnosticCode::None) {
        return;
    }
    expectations.expect(blend.hasUnretiredSubmission(),
                        name + ": an accepted job has an unretired submission");
    GpuBlendPollResult poll = GpuBlendPollResult::Pending;
    while (poll == GpuBlendPollResult::Pending) {
        poll = blend.poll();
    }
    expectations.expect(poll == GpuBlendPollResult::Ready,
                        name + ": completes: " + blend.diagnostic().message);
    if (poll != GpuBlendPollResult::Ready) {
        return;
    }
    expectations.expect(!blend.hasUnretiredSubmission(),
                        name + ": retirement is proven once Ready");
    auto result = blend.takeImage();
    expectations.expect(result.isValid(), name + ": resident output published");
    expectations.expect(result.displayWindow() == destination->displayWindow() &&
                            result.pixelAspect() == destination->pixelAspect(),
                        name +
                            ": output preserves the destination display window and pixel aspect");

    // CPU oracle: build each aligned row by sampling the source at (destAbsolute - sourceOrigin)
    // with transparent padding, then the retained blend primitive. This is the exact window mapping
    // the kernel implements.
    std::vector<Rgba32f> expected(destPixels);
    std::vector<Rgba32f> alignedSource(width, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto destY = destWindow.originY() + static_cast<std::int64_t>(y);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto destX = destWindow.originX() + static_cast<std::int64_t>(x);
            const auto sourceX = destX - sourceWindow.originX();
            const auto sourceY = destY - sourceWindow.originY();
            const bool inside = sourceX >= 0 && sourceY >= 0 &&
                                sourceX < static_cast<std::int64_t>(width) &&
                                sourceY < static_cast<std::int64_t>(height);
            alignedSource[x] = inside ? sourcePixels[static_cast<std::size_t>(sourceY) * width +
                                                     static_cast<std::size_t>(sourceX)]
                                      : Rgba32f::transparent();
        }
        auto destRow =
            std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * width, width);
        const auto status = bloom::render::blendLinearRec709SceneRow(mode, alignedSource, destRow);
        expectations.expect(!status.has_value(), name + ": oracle row succeeds");
    }
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), name + ": readback for oracle");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, name));
    }

    const auto sourceAfter = bloom::render::readbackResidentImage(*source, kBudget);
    const auto destAfter = bloom::render::readbackResidentImage(*destination, kBudget);
    expectations.expect(sourceAfter.hasValue() && sourceBefore.hasValue() &&
                            sourceAfter.pixels == sourceBefore.pixels,
                        name + ": the immutable source is unchanged");
    expectations.expect(destAfter.hasValue() && destBefore.hasValue() &&
                            destAfter.pixels == destBefore.pixels,
                        name + ": the immutable destination is unchanged");
}

void testAllModes(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(blend.hasValue() && uploader.hasValue(), "blend hosts created");
    if (!blend || !uploader) {
        return;
    }
    for (const auto mode : bloom::core::kBlendModes) {
        const auto value = static_cast<int>(bloom::core::blendModeStoredValue(mode));
        runMode(expectations, *blend.blend, *uploader.upload, mode,
                "mode-" + std::to_string(value) + "-odd-extent", 7, 3, 0, 0, 0, 0);
    }
    // Differing origins exercise the window mapping for every mode's Normal-equivalent path and for
    // a couple of arithmetic modes.
    runMode(expectations, *blend.blend, *uploader.upload, BlendMode::Normal,
            "normal-differing-origins", 16, 4, 7, 3, 100, -50);
    runMode(expectations, *blend.blend, *uploader.upload, BlendMode::Overlay,
            "overlay-differing-origins", 32, 8, 5, 5, -20, 40);
    runMode(expectations, *blend.blend, *uploader.upload, BlendMode::Add,
            "add-source-out-of-bounds", 16, 4, 1000, 1000, 0, 0);
}

void testRejections(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "rejection hosts created");
        return;
    }
    const auto window4 = window(0, 0, 4, 4);
    expectations.expect(
        blend.blend->beginBlend({nullptr, nullptr, BlendMode::Normal}, kBudget).code ==
            GpuBlendDiagnosticCode::InvalidArgument,
        "null blend inputs are rejected");
    expectations.expect(blend.blend->state() == GpuBlendJobState::Idle,
                        "a rejection starts no job");
    // A BlendMode value outside the durable mapping must be rejected, never fall through to a
    // Normal-equivalent shader branch.
    const auto invalidMode = static_cast<BlendMode>(200);
    expectations.expect(blend.blend->beginBlend({nullptr, nullptr, invalidMode}, kBudget).code ==
                            GpuBlendDiagnosticCode::InvalidArgument,
                        "an out-of-vocabulary blend mode is rejected");

    auto image = makeImage(window4, window4, PixelAspectRatio::square(), blendPixels(4, 4, true));
    auto dest = makeImage(window4, window4, PixelAspectRatio::square(), blendPixels(4, 4, false));
    if (!image || !dest) {
        expectations.expect(false, "rejection images build");
        return;
    }
    auto resident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*image)));
    auto destResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*dest)));
    if (!resident || !destResident) {
        expectations.expect(false, "rejection images upload");
        return;
    }
    auto shared = std::make_shared<const GpuImage>(std::move(*resident));
    auto sharedDest = std::make_shared<const GpuImage>(std::move(*destResident));

    // 4x4 RGBA32F is 256 requested bytes; an actual VMA allocation is larger, so a 300-byte budget
    // must be refused.
    expectations.expect(
        blend.blend->beginBlend({shared, sharedDest, BlendMode::Multiply}, 300).code ==
            GpuBlendDiagnosticCode::OverBudget,
        "an actual-allocation over-budget blend is refused");

    // Cancel: submit then cancel before polling.
    const auto started = blend.blend->beginBlend({shared, sharedDest, BlendMode::Screen}, kBudget);
    if (started.code == GpuBlendDiagnosticCode::None) {
        blend.blend->cancel();
        GpuBlendPollResult poll = GpuBlendPollResult::Pending;
        while (poll == GpuBlendPollResult::Pending) {
            poll = blend.blend->poll();
        }
        expectations.expect(poll == GpuBlendPollResult::Failure &&
                                blend.blend->diagnostic().code == GpuBlendDiagnosticCode::Cancelled,
                            "a cancelled blend reports Failure(Cancelled)");
    }

    // Subnormal input is rejected whole-frame by the kernel's status flag.
    const auto subnormal =
        Rgba32f::fromPremultiplied(std::numeric_limits<float>::denorm_min(), 0.0F, 0.0F, 1.0F);
    if (subnormal) {
        std::vector<Rgba32f> pixels(16, Rgba32f::transparent());
        pixels[0] = *subnormal.value();
        auto bad = makeImage(window4, window4, PixelAspectRatio::square(), pixels);
        if (bad) {
            auto badResident =
                upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*bad)));
            if (badResident) {
                auto badShared = std::make_shared<const GpuImage>(std::move(*badResident));
                const auto begin =
                    blend.blend->beginBlend({badShared, sharedDest, BlendMode::Normal}, kBudget);
                if (begin.code == GpuBlendDiagnosticCode::None) {
                    GpuBlendPollResult poll = GpuBlendPollResult::Pending;
                    while (poll == GpuBlendPollResult::Pending) {
                        poll = blend.blend->poll();
                    }
                    expectations.expect(poll == GpuBlendPollResult::Failure &&
                                            blend.blend->diagnostic().code ==
                                                GpuBlendDiagnosticCode::StatusFlagRejected,
                                        "a subnormal blend input is StatusFlagRejected");
                }
            }
        }
    }

    // Wrong thread: an owner-thread-only operation refuses another thread before any Vulkan call.
    GpuBlendDiagnosticCode foreignCode = GpuBlendDiagnosticCode::None;
    std::thread([&blend, &foreignCode]() {
        foreignCode = blend.blend->beginBlend({nullptr, nullptr, BlendMode::Normal}, kBudget).code;
    }).join();
    expectations.expect(foreignCode == GpuBlendDiagnosticCode::WrongThread,
                        "beginBlend from a foreign thread reports WrongThread");
}

void testForeignInputs(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "foreign-input hosts created");
        return;
    }
    GpuDeviceCreationOptions options;
    auto second = GpuDevice::create(options);
    if (!second) {
        std::cout
            << "NOTE: a second device is unavailable; foreign blend rejection not exercised\n";
        return;
    }
    auto foreignUploader = GpuImageUpload::create(*second.device);
    if (!foreignUploader) {
        return;
    }
    const auto window4 = window(0, 0, 4, 4);
    auto image = makeImage(window4, window4, PixelAspectRatio::square(), blendPixels(4, 4, true));
    auto dest = makeImage(window4, window4, PixelAspectRatio::square(), blendPixels(4, 4, false));
    if (!image || !dest) {
        return;
    }
    auto foreignResident =
        upload(*foreignUploader.upload, std::make_shared<const Rgba32fImage>(std::move(*image)));
    auto sameResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*dest)));
    if (!foreignResident || !sameResident) {
        return;
    }
    auto foreign = std::make_shared<const GpuImage>(std::move(*foreignResident));
    auto same = std::make_shared<const GpuImage>(std::move(*sameResident));
    expectations.expect(blend.blend->beginBlend({foreign, same, BlendMode::Darken}, kBudget).code ==
                            GpuBlendDiagnosticCode::InvalidArgument,
                        "a foreign blend foreground is rejected before dispatch");
    expectations.expect(
        blend.blend->beginBlend({same, foreign, BlendMode::Lighten}, kBudget).code ==
            GpuBlendDiagnosticCode::InvalidArgument,
        "a foreign blend backdrop is rejected before dispatch");
    expectations.expect(blend.blend->state() == GpuBlendJobState::Idle,
                        "a foreign-input rejection starts no job");
}

// A deterministic dense field over straight (un-premultiplied) magnitudes and alphas that includes
// HDR positive and negative values, tiny positive alphas, fractional alphas, and exact cancellation
// (identical source/destination operands under Difference/Screen). Values stay normal, so the
// kernel's whole-frame subnormal rejection is not triggered.
[[nodiscard]] std::vector<Rgba32f> denseField(const std::uint32_t width, const std::uint32_t height,
                                              const bool source) {
    const std::array<float, 9> magnitudes{0.0F, 1.0e-5F, 1.0e-3F, 0.25F,  0.5F,
                                          1.0F, 2.0F,    64.0F,   4096.0F};
    const std::array<float, 4> alphas{1.0e-5F, 0.25F, 0.5F, 1.0F};
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto selector = x + y * width;
            const auto sourceMagnitude = magnitudes[(selector * 7U + 3U) % 9U];
            const auto destMagnitude = magnitudes[(selector * 11U + 5U) % 9U];
            const auto sourceAlpha = alphas[(selector * 3U + 1U) % 4U];
            auto destAlpha = alphas[(selector * 5U + 2U) % 4U];
            const auto sourceStraight = sourceMagnitude * (((selector % 2U) == 0U) ? 1.0F : -1.0F);
            auto destStraight = destMagnitude * ((((selector / 2U) % 2U) == 0U) ? 1.0F : -1.0F);
            // Every fifth column cancels exactly: the destination equals the source, so the
            // Difference blend function is exactly zero from large HDR operands.
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

// Real-device dense HDR/cancellation parity for all eight modes against the Float64 CPU oracle,
// strictly within the 2e-6 absolute-or-relative gate per component.
void testDenseHdr(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "dense HDR hosts created");
        return;
    }
    constexpr std::uint32_t kWidth = 257;
    constexpr std::uint32_t kHeight = 3;
    const auto sourceWindow = window(0, 0, kWidth, kHeight);
    const auto sourcePixels = denseField(kWidth, kHeight, true);
    const auto destPixels = denseField(kWidth, kHeight, false);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    auto destImage = makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), destPixels);
    if (!sourceImage || !destImage) {
        expectations.expect(false, "dense HDR images build");
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    auto destResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*destImage)));
    if (!sourceResident || !destResident) {
        expectations.expect(false, "dense HDR images upload");
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto destination = std::make_shared<const GpuImage>(std::move(*destResident));
    for (const auto mode : bloom::core::kBlendModes) {
        const auto value = static_cast<int>(bloom::core::blendModeStoredValue(mode));
        const auto label = "dense-hdr-mode-" + std::to_string(value);
        const auto begin = blend.blend->beginBlend({source, destination, mode}, kBudget);
        expectations.expect(begin.code == GpuBlendDiagnosticCode::None,
                            label + ": begin accepted: " + begin.message);
        if (begin.code != GpuBlendDiagnosticCode::None) {
            continue;
        }
        GpuBlendPollResult poll = GpuBlendPollResult::Pending;
        while (poll == GpuBlendPollResult::Pending) {
            poll = blend.blend->poll();
        }
        expectations.expect(poll == GpuBlendPollResult::Ready,
                            label + ": completes: " + blend.blend->diagnostic().message);
        if (poll != GpuBlendPollResult::Ready) {
            continue;
        }
        auto result = blend.blend->takeImage();
        std::vector<Rgba32f> expected(destPixels);
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            auto row =
                std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * kWidth, kWidth);
            const auto status = bloom::render::blendLinearRec709SceneRow(
                mode,
                std::span<const Rgba32f>(sourcePixels)
                    .subspan(static_cast<std::size_t>(y) * kWidth, kWidth),
                row);
            expectations.expect(!status.has_value(), label + ": oracle row succeeds");
        }
        const auto readback = bloom::render::readbackResidentImage(result, kBudget);
        expectations.expect(readback.hasValue(), label + ": readback succeeds");
        if (readback) {
            static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, label));
        }
    }
}

// One origin case with int64-safe CPU alignment. For every destination pixel the source-local
// coordinate is computed with an explicit subtraction-overflow guard, so extreme origins never
// wrap; an unrepresentable or out-of-window tap is transparent, exactly the mapping the host
// resolves. This is the oracle for the safe-source-offset fix.
void runOriginCase(Expectations& expectations, GpuBlend& blend, GpuImageUpload& uploader,
                   const BlendMode mode, const std::string& name, const std::int64_t sourceOriginX,
                   const std::int64_t sourceOriginY, const std::int64_t destOriginX,
                   const std::int64_t destOriginY, const std::uint32_t width,
                   const std::uint32_t height) {
    const auto sourceWindow = window(sourceOriginX, sourceOriginY, width, height);
    const auto destWindow = window(destOriginX, destOriginY, width, height);
    const auto sourcePixels = blendPixels(width, height, true);
    const auto destPixels = blendPixels(width, height, false);
    auto sourceImage =
        makeImage(sourceWindow, sourceWindow, PixelAspectRatio::square(), sourcePixels);
    auto destImage = makeImage(destWindow, destWindow, PixelAspectRatio::square(), destPixels);
    if (!sourceImage || !destImage) {
        expectations.expect(false, name + ": images build");
        return;
    }
    auto sourceResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    auto destResident =
        upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*destImage)));
    if (!sourceResident || !destResident) {
        expectations.expect(false, name + ": images upload");
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto destination = std::make_shared<const GpuImage>(std::move(*destResident));
    const auto begin = blend.beginBlend({source, destination, mode}, kBudget);
    expectations.expect(begin.code == GpuBlendDiagnosticCode::None,
                        name + ": begin accepted: " + begin.message);
    if (begin.code != GpuBlendDiagnosticCode::None) {
        return;
    }
    GpuBlendPollResult poll = GpuBlendPollResult::Pending;
    while (poll == GpuBlendPollResult::Pending) {
        poll = blend.poll();
    }
    expectations.expect(poll == GpuBlendPollResult::Ready, name + ": completes");
    if (poll != GpuBlendPollResult::Ready) {
        return;
    }
    auto result = blend.takeImage();
    std::vector<Rgba32f> expected(destPixels);
    std::vector<Rgba32f> alignedSource(width, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto destBaseY = destWindow.originY() + static_cast<std::int64_t>(y);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto destBaseX = destWindow.originX() + static_cast<std::int64_t>(x);
            std::int64_t localX = 0;
            std::int64_t localY = 0;
            const bool representable = !__builtin_sub_overflow(destBaseX, sourceOriginX, &localX) &&
                                       !__builtin_sub_overflow(destBaseY, sourceOriginY, &localY);
            alignedSource[x] = representable && localX >= 0 && localY >= 0 &&
                                       localX < static_cast<std::int64_t>(width) &&
                                       localY < static_cast<std::int64_t>(height)
                                   ? sourcePixels[static_cast<std::size_t>(localY) * width +
                                                  static_cast<std::size_t>(localX)]
                                   : Rgba32f::transparent();
        }
        auto destRow =
            std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * width, width);
        static_cast<void>(bloom::render::blendLinearRec709SceneRow(mode, alignedSource, destRow));
    }
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), name + ": readback");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, name));
    }
}

void testExtremeOrigins(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "extreme-origin hosts created");
        return;
    }
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    // Origin differences that overflow int64 or exceed int32: the source window cannot overlap, so
    // every tap is transparent and the output equals the backdrop. A narrowing bug would wrap the
    // offset and sample the source instead.
    runOriginCase(expectations, *blend.blend, *uploader.upload, BlendMode::Difference,
                  "origin-int64-extremes", kMin + 4, kMin + 4, kMax - 4, kMax - 4, 2, 2);
    runOriginCase(expectations, *blend.blend, *uploader.upload, BlendMode::Multiply,
                  "origin-difference-2p32", 0, 0, (std::int64_t{1} << 32), (std::int64_t{1} << 32),
                  2, 2);
    // Genuinely overlapping windows with enormous absolute origins: the int32 offset is exact and
    // the source is really sampled.
    runOriginCase(expectations, *blend.blend, *uploader.upload, BlendMode::Overlay,
                  "origin-overlap-near-int64-min", kMin, kMin, kMin + 1, kMin + 1, 4, 4);
    runOriginCase(expectations, *blend.blend, *uploader.upload, BlendMode::Screen,
                  "origin-overlap-2p33", (std::int64_t{1} << 33), (std::int64_t{1} << 33),
                  (std::int64_t{1} << 33) + 1, (std::int64_t{1} << 33) + 1, 4, 4);
}

// A blend output must be a real, fully written GENERAL image usable as the next operation's input.
// This chains two blends and compares the second result to the CPU oracle, which fails if the first
// output's contents were discarded by an UNDEFINED old layout on the final barrier.
void testChainedBlend(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "chain hosts created");
        return;
    }
    constexpr std::uint32_t kWidth = 16;
    constexpr std::uint32_t kHeight = 4;
    const auto win = window(0, 0, kWidth, kHeight);
    const auto sourcePixels = blendPixels(kWidth, kHeight, true);
    const auto destPixels = blendPixels(kWidth, kHeight, false);
    auto sourceImage = makeImage(win, win, PixelAspectRatio::square(), sourcePixels);
    auto destImage = makeImage(win, win, PixelAspectRatio::square(), destPixels);
    if (!sourceImage || !destImage) {
        expectations.expect(false, "chain images build");
        return;
    }
    auto sourceResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    auto destResident =
        upload(*uploader.upload, std::make_shared<const Rgba32fImage>(std::move(*destImage)));
    if (!sourceResident || !destResident) {
        expectations.expect(false, "chain images upload");
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*sourceResident));
    auto destination = std::make_shared<const GpuImage>(std::move(*destResident));

    const auto run = [&](const std::shared_ptr<const GpuImage>& a,
                         const std::shared_ptr<const GpuImage>& b,
                         const BlendMode mode) -> std::optional<GpuImage> {
        if (blend.blend->beginBlend({a, b, mode}, kBudget).code != GpuBlendDiagnosticCode::None) {
            return std::nullopt;
        }
        GpuBlendPollResult poll = GpuBlendPollResult::Pending;
        while (poll == GpuBlendPollResult::Pending) {
            poll = blend.blend->poll();
        }
        return poll == GpuBlendPollResult::Ready ? std::optional(blend.blend->takeImage())
                                                 : std::nullopt;
    };
    auto first = run(source, destination, BlendMode::Overlay);
    expectations.expect(first.has_value(), "the first blend completes");
    if (!first) {
        return;
    }
    auto firstResident = std::make_shared<const GpuImage>(std::move(*first));
    auto second = run(firstResident, destination, BlendMode::Multiply);
    expectations.expect(second.has_value(), "the second blend completes");
    if (!second) {
        return;
    }
    std::vector<Rgba32f> intermediate(destPixels);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        auto row =
            std::span<Rgba32f>(intermediate).subspan(static_cast<std::size_t>(y) * kWidth, kWidth);
        static_cast<void>(bloom::render::blendLinearRec709SceneRow(
            BlendMode::Overlay,
            std::span<const Rgba32f>(sourcePixels)
                .subspan(static_cast<std::size_t>(y) * kWidth, kWidth),
            row));
    }
    std::vector<Rgba32f> expected(destPixels);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        auto row =
            std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * kWidth, kWidth);
        static_cast<void>(bloom::render::blendLinearRec709SceneRow(
            BlendMode::Multiply,
            std::span<const Rgba32f>(intermediate)
                .subspan(static_cast<std::size_t>(y) * kWidth, kWidth),
            row));
    }
    const auto readback = bloom::render::readbackResidentImage(*second, kBudget);
    expectations.expect(readback.hasValue(), "the chained readback succeeds");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, "chained-blend"));
    }
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
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 0;
        }
        expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
        testAllModes(expectations, *device.device);
        testDenseHdr(expectations, *device.device);
        testExtremeOrigins(expectations, *device.device);
        testChainedBlend(expectations, *device.device);
        testRejections(expectations, *device.device);
        testForeignInputs(expectations, *device.device);
        runBlendBenchmark(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " blend native expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU blend native modes\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
