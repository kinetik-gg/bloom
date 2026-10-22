#include "gpu_blend_native_support.hpp"

#include <bloom/render/image_coverage.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render::blend_proof {
namespace {
using namespace bloom::render::composite_proof;

[[nodiscard]] Rgba32f oracle(const Rgba32f base, const Rgba32f second,
                            const GpuImageCombineParameters mode) {
    if (mode.kind == GpuImageCombineKind::SetAlpha) {
        const auto result = setImageAlpha(base, imageCoverage(second, mode.channel));
        if (!result)
            throw std::logic_error("invalid Set Alpha oracle");
        return *result.value();
    }
    if (mode.kind == GpuImageCombineKind::MultiplyAlpha) {
        const auto amount = imageCoverage(second, mode.channel) * mode.factor;
        return pixel(base.red() * amount, base.green() * amount, base.blue() * amount,
                     base.alpha() * amount);
    }
    const auto inverse = 1.0F - mode.factor;
    return pixel(base.red() * inverse + second.red() * mode.factor,
                 base.green() * inverse + second.green() * mode.factor,
                 base.blue() * inverse + second.blue() * mode.factor,
                 base.alpha() * inverse + second.alpha() * mode.factor);
}
} // namespace

void runImageCombineTests(Expectations& expectations, GpuDevice& device) {
    auto blend = GpuBlend::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!blend || !uploader) {
        expectations.expect(false, "image combine hosts created");
        return;
    }
    // The base image owns the output extent. The smaller second window starts one pixel to the
    // right and one pixel below; every tap outside it is transparent black.
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 3;
    constexpr std::uint32_t secondWidth = 2;
    constexpr std::uint32_t secondHeight = 2;
    const auto baseWindow = window(-2, 1, width, height);
    const auto secondWindow = window(-1, 2, secondWidth, secondHeight);
    std::vector<Rgba32f> basePixels(width * height, Rgba32f::transparent());
    for (std::size_t index = 0; index < basePixels.size(); ++index) {
        const float alpha = index % 5 == 0 ? 0.0F : (index % 2 == 0 ? 0.5F : 1.0F);
        basePixels[index] = pixel(-0.75F * alpha, 2.5F * alpha, 0.125F * alpha, alpha);
    }
    const std::vector<Rgba32f> secondPixels{
        pixel(-0.5F, 1.5F, 0.25F, 0.5F), pixel(2.0F, -0.25F, 0.25F, 1.0F),
        pixel(0.0F, 0.0F, 0.0F, 0.0F), pixel(0.25F, 0.75F, 2.0F, 0.5F)};
    auto baseImage = makeImage(baseWindow, baseWindow, PixelAspectRatio::square(), basePixels);
    auto secondImage =
        makeImage(secondWindow, secondWindow, PixelAspectRatio::square(), secondPixels);
    if (!baseImage || !secondImage) {
        expectations.expect(false, "image combine fixtures build");
        return;
    }
    auto baseResident = upload(*uploader.upload,
                               std::make_shared<const Rgba32fImage>(std::move(*baseImage)));
    auto secondResident = upload(*uploader.upload,
                                 std::make_shared<const Rgba32fImage>(std::move(*secondImage)));
    if (!baseResident || !secondResident) {
        expectations.expect(false, "image combine fixtures upload");
        return;
    }
    auto base = std::make_shared<const GpuImage>(std::move(*baseResident));
    auto second = std::make_shared<const GpuImage>(std::move(*secondResident));
    const std::array modes{
        GpuImageCombineParameters{GpuImageCombineKind::Mix, 0.375F, ImageCoverageChannel::Alpha},
        GpuImageCombineParameters{GpuImageCombineKind::SetAlpha, 1.0F,
                                  ImageCoverageChannel::Luma},
        GpuImageCombineParameters{GpuImageCombineKind::MultiplyAlpha, 0.5F,
                                  ImageCoverageChannel::Red}};
    for (const auto mode : modes) {
        const auto label = "image-combine-" + std::to_string(static_cast<int>(mode.kind));
        const auto begin = blend.blend->beginBlend(
            GpuBlendParameters{second, base, core::BlendMode::Normal, mode}, kBudget);
        expectations.expect(begin.code == GpuBlendDiagnosticCode::None,
                            label + ": begin accepted: " + begin.message);
        if (begin.code != GpuBlendDiagnosticCode::None)
            continue;
        GpuBlendPollResult poll = GpuBlendPollResult::Pending;
        while (poll == GpuBlendPollResult::Pending)
            poll = blend.blend->poll();
        expectations.expect(poll == GpuBlendPollResult::Ready,
                            label + ": native execution completes");
        if (poll != GpuBlendPollResult::Ready)
            continue;
        const auto result = blend.blend->takeImage();
        expectations.expect(result.dataWindow() == baseWindow &&
                                result.displayWindow() == baseWindow,
                            label + ": base owns data and display windows");
        std::vector<Rgba32f> expected(basePixels);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto globalX = baseWindow.originX() + static_cast<std::int64_t>(x);
                const auto globalY = baseWindow.originY() + static_cast<std::int64_t>(y);
                Rgba32f tap = Rgba32f::transparent();
                if (globalX >= secondWindow.originX() &&
                    globalX < secondWindow.maxXExclusive() &&
                    globalY >= secondWindow.originY() &&
                    globalY < secondWindow.maxYExclusive()) {
                    const auto localX = static_cast<std::size_t>(globalX - secondWindow.originX());
                    const auto localY = static_cast<std::size_t>(globalY - secondWindow.originY());
                    tap = secondPixels[localY * secondWidth + localX];
                }
                const auto index = static_cast<std::size_t>(y) * width + x;
                expected[index] = oracle(basePixels[index], tap, mode);
            }
        }
        const auto readback = readbackResidentImage(result, kBudget);
        expectations.expect(readback.hasValue(), label + ": oracle readback succeeds");
        if (readback)
            static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, label));
    }
}
} // namespace bloom::render::blend_proof
