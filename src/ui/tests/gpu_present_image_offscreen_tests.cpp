// Native offscreen proof for the P2/P3 presentation sampling path. A real Vulkan device builds an
// RGBA32F input (GpuImageUpload) -> resident RGBA8 display (GpuResidentDisplay), then the SAME
// production present draw (renderResidentIntoAcquired) renders it into a test-only offscreen VMA
// RGBA8/BGRA8 color attachment. The test reads the attachment back and compares against an
// independent QPainter oracle built from the *same* resident-display readback bytes, so the
// presentation stage is isolated from display/colour processing.
//
// Everything is compiled against the frozen snapshot's private headers only (proof/README.md), so
// no live native object participates and no DeviceAllocatorState layout can mix.

#include "gpu_present_image_native_support.hpp"
#include "gpu_present_image_oracle.hpp"
#include "gpu_resident_display_oracle.hpp"
#include <cstddef>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::present_native_support;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuPresentChannel;
using bloom::render::GpuPresentColor;
using bloom::render::GpuPresentOverlay;
using bloom::render::present_oracle::compareImages;
using bloom::render::present_oracle::renderOracle;

void testIdentity(Expectations& expectations, GpuDevice& device, GpuImageUpload& uploader,
                  GpuResidentDisplay& display) {
    auto image = makeImage(*window(0, 0, 2, 2), sentinelPixels());
    if (!image) {
        expectations.expect(false, "2x2 sentinel input builds");
        return;
    }
    auto resident = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*image)));
    expectations.expect(resident.has_value(), "2x2 input uploads");
    if (!resident) {
        return;
    }
    auto shared = std::make_shared<const GpuImage>(std::move(*resident));
    const auto displayBytes = displayReadback(display, shared);
    expectations.expect(displayBytes.has_value(), "resident display readback succeeds");
    if (!displayBytes) {
        return;
    }
    expectations.expect(displayBytes->size() == 4, "resident display is 2x2");

    auto control = bloom::render::GpuRendererAccess::state(device);
    expectations.expect(control != nullptr, "device allocator state is available");
    if (control == nullptr) {
        return;
    }
    const auto input = std::make_shared<const GpuDisplayImage>(display.takeImage());
    expectations.expect(input->isValid(), "resident display image is valid");

    for (const VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        const auto rendered = renderOffscreen(control, format, 2, 2, input, identityParams(2, 2),
                                              GpuPresentOverlay{});
        const std::string label = format == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8";
        expectations.expect(rendered.ok, label + " offscreen render succeeds: " + rendered.message);
        if (!rendered.ok) {
            continue;
        }
        bool match = rendered.pixels.size() == displayBytes->size();
        for (std::size_t index = 0; match && index < rendered.pixels.size(); ++index) {
            match = rendered.pixels[index] == (*displayBytes)[index];
        }
        expectations.expect(match, label + " 1:1 present reproduces the display bytes exactly");

        const auto oracle = renderOracle(*displayBytes, 2, 2, identityParams(2, 2), {}, 0, 0, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 0);
        expectations.expect(delta.ok, label + " matches the QPainter 1:1 oracle (delta " +
                                          std::to_string(delta.maxChannelDelta) + ")");
    }
}

struct DisplayBundle final {
    std::shared_ptr<const GpuDisplayImage> input;
    std::vector<Rgba8> bytes;
};

[[nodiscard]] std::optional<DisplayBundle> makeDisplayBundle(GpuImageUpload& uploader,
                                                             GpuResidentDisplay& display,
                                                             const std::vector<Rgba32f>& pixels,
                                                             const std::uint32_t width,
                                                             const std::uint32_t height) {
    auto image = makeImage(*window(0, 0, width, height), pixels);
    if (!image) {
        return std::nullopt;
    }
    auto resident = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*image)));
    if (!resident) {
        return std::nullopt;
    }
    auto shared = std::make_shared<const GpuImage>(std::move(*resident));
    auto bytes = displayReadback(display, shared);
    if (!bytes) {
        return std::nullopt;
    }
    DisplayBundle bundle;
    bundle.input = std::make_shared<const GpuDisplayImage>(display.takeImage());
    bundle.bytes = std::move(*bytes);
    return bundle;
}

void testOracleChannels(Expectations& expectations,
                        const std::shared_ptr<DeviceAllocatorState>& control,
                        const std::shared_ptr<const GpuDisplayImage>& input,
                        const std::vector<Rgba8>& displayBytes) {
    const std::vector<GpuPresentChannel> channels{
        GpuPresentChannel::Rgba,  GpuPresentChannel::Rgb,  GpuPresentChannel::Red,
        GpuPresentChannel::Green, GpuPresentChannel::Blue, GpuPresentChannel::Alpha};
    for (const GpuPresentChannel channel : channels) {
        auto params = identityParams(2, 2);
        params.channel = channel;
        params.background = GpuPresentBackground::Black;
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, input, params, {});
        const auto oracle = renderOracle(displayBytes, 2, 2, params, {}, 0, 0, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 1);
        expectations.expect(rendered.ok && delta.ok,
                            "channel mode " + std::to_string(static_cast<int>(channel)) +
                                " matches the oracle (delta " +
                                std::to_string(delta.maxChannelDelta) + ")");
    }
}

void testOracleZoomCrop(Expectations& expectations,
                        const std::shared_ptr<DeviceAllocatorState>& control,
                        GpuImageUpload& uploader, GpuResidentDisplay& display,
                        const std::shared_ptr<const GpuDisplayImage>& sentinelInput,
                        const std::vector<Rgba8>& sentinelBytes) {
    // Fractional smooth upscale of the 2x2 sentinel to 8x8.
    {
        auto params = identityParams(8, 8);
        params.destination = GpuPresentRect{0.0F, 0.0F, 8.0F, 8.0F};
        params.source = GpuPresentSourceWindow{0.0, 0.0, 2.0, 2.0};
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, sentinelInput, params, {});
        const auto oracle = renderOracle(sentinelBytes, 2, 2, params, {}, 0, 0, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 3);
        expectations.expect(rendered.ok && delta.ok,
                            "fractional smooth 2x2->8x8 matches the QPainter oracle (delta " +
                                std::to_string(delta.maxChannelDelta) + ")");
    }
    // Pan / source crop: a sub-window that straddles the sentinel boundary.
    {
        auto params = identityParams(4, 4);
        params.source = GpuPresentSourceWindow{0.25, 0.25, 1.5, 1.5};
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, sentinelInput, params, {});
        const auto oracle = renderOracle(sentinelBytes, 2, 2, params, {}, 0, 0, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 3);
        expectations.expect(rendered.ok && delta.ok,
                            "pan/source crop matches the QPainter oracle (delta " +
                                std::to_string(delta.maxChannelDelta) + ")");
    }
    // Pixel aspect via an explicit non-square destination.
    {
        auto params = identityParams(8, 4);
        params.destination = GpuPresentRect{0.0F, 0.0F, 8.0F, 4.0F};
        params.source = GpuPresentSourceWindow{0.0, 0.0, 2.0, 2.0};
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 8, 4, sentinelInput, params, {});
        const auto oracle = renderOracle(sentinelBytes, 2, 2, params, {}, 0, 0, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 3);
        expectations.expect(rendered.ok && delta.ok,
                            "non-square PAR destination matches the QPainter oracle (delta " +
                                std::to_string(delta.maxChannelDelta) + ")");
    }
    // Premultiplied-alpha filtering at fractional zoom: the display carries alpha endpoints and a
    // half-alpha texel, so a straight-alpha filter would produce a different result.
    {
        auto image = makeImage(*window(0, 0, 2, 2), premultipliedPixels());
        auto resident = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*image)));
        if (!resident) {
            expectations.expect(false, "premultiplied input uploads");
            return;
        }
        auto shared = std::make_shared<const GpuImage>(std::move(*resident));
        const auto bytes = displayReadback(display, shared);
        if (!bytes) {
            expectations.expect(false, "premultiplied display readback");
            return;
        }
        const auto premulInput = std::make_shared<const GpuDisplayImage>(display.takeImage());
        auto params = identityParams(6, 6);
        params.destination = GpuPresentRect{0.0F, 0.0F, 6.0F, 6.0F};
        params.source = GpuPresentSourceWindow{0.0, 0.0, 2.0, 2.0};
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 6, 6, premulInput, params, {});
        const auto oracle = renderOracle(*bytes, 2, 2, params, {}, 0, 0, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 3);
        expectations.expect(rendered.ok && delta.ok,
                            "premultiplied-alpha fractional zoom matches the QPainter oracle "
                            "(delta " +
                                std::to_string(delta.maxChannelDelta) + ")");
    }
}

void testChecker(Expectations& expectations, const std::shared_ptr<DeviceAllocatorState>& control,
                 const std::shared_ptr<const GpuDisplayImage>& input,
                 const std::vector<Rgba8>& displayBytes) {
    auto params = identityParams(8, 8);
    params.source = GpuPresentSourceWindow{0.0, 0.0, 2.0, 2.0};
    params.background = GpuPresentBackground::Checkerboard;
    params.checkerTilePixels = 2.0F;
    params.checkerOriginX = 1.0F;
    params.checkerOriginY = 1.0F;
    params.checkerColorA = GpuPresentColor{0.1F, 0.2F, 0.3F, 1.0F};
    params.checkerColorB = GpuPresentColor{0.8F, 0.7F, 0.6F, 1.0F};
    const auto rendered =
        renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, input, params, {});
    const auto oracle = renderOracle(displayBytes, 2, 2, params, {}, 0, 0, true);
    const auto delta = compareImages(rendered.pixels, oracle.pixels, 2);
    expectations.expect(rendered.ok && delta.ok,
                        "checkerboard origin/tile matches the QPainter oracle (delta " +
                            std::to_string(delta.maxChannelDelta) + ")");
}

[[nodiscard]] std::vector<Rgba8> makeOverlay(const std::uint32_t width, const std::uint32_t height,
                                             const std::uint8_t red, const std::uint8_t green,
                                             const std::uint8_t blue, const std::uint8_t alpha) {
    std::vector<Rgba8> overlay(static_cast<std::size_t>(width) * height, Rgba8{0, 0, 0, 0});
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            overlay[static_cast<std::size_t>(y) * width + x] = Rgba8{red, green, blue, alpha};
        }
    }
    return overlay;
}

void testOverlay(Expectations& expectations, const std::shared_ptr<DeviceAllocatorState>& control,
                 const std::shared_ptr<const GpuDisplayImage>& input,
                 const std::vector<Rgba8>& displayBytes) {
    const std::vector<Rgba8> overlay = makeOverlay(2, 2, 32, 64, 128, 128);
    auto presenter = std::make_shared<PresentImagePipeline>();
    auto params = identityParams(2, 2);

    for (int pass = 0; pass < 2; ++pass) {
        GpuPresentOverlay overlayRef;
        overlayRef.pixels = reinterpret_cast<const std::uint8_t*>(overlay.data());
        overlayRef.width = 2;
        overlayRef.height = 2;
        overlayRef.rowStrideBytes = 2 * 4;
        overlayRef.token = 7;
        const auto rendered = renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2,
                                                  2, input, params, overlayRef);
        const auto oracle = renderOracle(displayBytes, 2, 2, params, overlay, 2, 2, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 2);
        expectations.expect(rendered.ok && delta.ok,
                            (pass == 0 ? "new overlay token" : "overlay token reuse") +
                                std::string(" matches the QPainter oracle (delta ") +
                                std::to_string(delta.maxChannelDelta) + ")");
        expectations.expect(presenter->overlayImage != nullptr &&
                                presenter->overlayAllocation != VK_NULL_HANDLE &&
                                !presenter->stagedOverlayPending && presenter->overlayToken == 7,
                            "the overlay VMA image is really allocated and committed");
        if (pass == 0) {
            // The staging buffer is retained until the next present proves the fence complete.
            expectations.expect(presenter->stagedOverlayBuffer != VK_NULL_HANDLE &&
                                    presenter->stagedOverlayAllocation != VK_NULL_HANDLE,
                                "the overlay staging buffer is retained after the draw");
        }
    }

    // Removal: token 0 / null pixels must drop the overlay, not silently keep it.
    {
        const GpuPresentOverlay none{};
        const auto rendered = renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2,
                                                  2, input, params, none);
        const auto oracle = renderOracle(displayBytes, 2, 2, params, {}, 0, 0, true);
        const auto delta = compareImages(rendered.pixels, oracle.pixels, 1);
        expectations.expect(rendered.ok && delta.ok,
                            "overlay removal matches the QPainter oracle (delta " +
                                std::to_string(delta.maxChannelDelta) + ")");
    }

    // Failing token: a token without valid pixels must fail the frame instead of presenting an
    // overlay-less frame as success.
    {
        GpuPresentOverlay bad;
        bad.token = 9;
        const auto rendered = renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2,
                                                  2, input, params, bad);
        expectations.expect(!rendered.ok, "a failing overlay token is not silently dropped");
    }
}

void testRejectionsAndBudgets(Expectations& expectations, GpuDevice& device,
                              const std::filesystem::path& loaderPath,
                              const std::shared_ptr<DeviceAllocatorState>& control,
                              const std::shared_ptr<const GpuDisplayImage>& input) {
    auto params = identityParams(2, 2);
    // Null input.
    {
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, nullptr, params, {});
        expectations.expect(!rendered.ok, "a null resident display input is rejected");
    }
    // Non-UNORM attachment (sRGB double-encode path refused).
    {
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_SRGB, 2, 2, input, params, {});
        expectations.expect(!rendered.ok, "an _SRGB attachment is rejected");
    }
    // Overlay byte budget: the check happens before any allocation, so a huge request is safe.
    {
        GpuPresentOverlay huge;
        static std::uint8_t dummy[4] = {0, 0, 0, 0};
        huge.pixels = dummy;
        huge.width = 8193;
        huge.height = 8192;
        huge.rowStrideBytes = 8193 * 4;
        huge.token = 3;
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, input, params, huge);
        expectations.expect(!rendered.ok, "an over-budget overlay is rejected");
    }
    // Overlay short stride.
    {
        GpuPresentOverlay shortStride;
        static std::uint8_t pixels[4] = {0, 0, 0, 0};
        shortStride.pixels = pixels;
        shortStride.width = 2;
        shortStride.height = 2;
        shortStride.rowStrideBytes = 2;
        shortStride.token = 4;
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, input, params, shortStride);
        expectations.expect(!rendered.ok, "an overlay row stride smaller than one row is rejected");
    }
    // Wrong device: a display image minted on a second device must be rejected before any draw.
    {
        GpuDeviceCreationOptions options;
        options.loader_path = loaderPath;
        auto second = GpuDevice::create(options);
        if (second) {
            auto secondUploader = GpuImageUpload::create(*second.device);
            auto secondDisplay = GpuResidentDisplay::create(*second.device);
            if (secondUploader && secondDisplay) {
                auto image = makeImage(*window(0, 0, 2, 2), sentinelPixels());
                auto resident =
                    image ? upload(*secondUploader.upload,
                                   std::make_shared<const Rgba32fImage>(std::move(*image)))
                          : std::nullopt;
                if (resident) {
                    auto foreignInput = std::make_shared<const GpuImage>(std::move(*resident));
                    const auto bytes = displayReadback(*secondDisplay.display, foreignInput);
                    if (bytes) {
                        auto foreign = std::make_shared<const GpuDisplayImage>(
                            secondDisplay.display->takeImage());
                        const auto rendered = renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 2,
                                                              2, foreign, params, {});
                        expectations.expect(!rendered.ok,
                                            "a foreign-device display image is rejected");
                    }
                }
            }
            static_cast<void>(device);
        }
    }
}

// A failed overlay preparation must not leave a cached display view without its strong source pin.
// The presenter commits view + pin together; a failing overlay token fails the frame but the cached
// view for the new input still owns a strong reference, so releasing the caller's reference cannot
// dangle or let a recycled view be reused.
void testFailedOverlayRetainsPin(Expectations& expectations,
                                 const std::shared_ptr<DeviceAllocatorState>& control,
                                 GpuImageUpload& uploader, GpuResidentDisplay& display,
                                 const std::shared_ptr<const GpuDisplayImage>& inputA) {
    auto presenter = std::make_shared<PresentImagePipeline>();
    const auto params = identityParams(2, 2);
    const auto first =
        renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, inputA, params, {});
    expectations.expect(first.ok, "the pin-probe first frame succeeds");

    // Build a second display image and drop the caller's original A reference.
    auto second = makeDisplayBundle(uploader, display, sentinelPixels(), 2, 2);
    expectations.expect(second.has_value(), "the second display bundle builds");
    if (!second) {
        return;
    }
    const std::shared_ptr<const GpuDisplayImage> inputB = second->input;

    // A failing overlay token must fail the frame, but the new view+pin must already be committed.
    GpuPresentOverlay bad;
    bad.token = 9;
    const auto failed = renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2, 2,
                                            inputB, params, bad);
    expectations.expect(!failed.ok, "a failing overlay token fails the frame");
    expectations.expect(presenter->pinnedInput.get() == inputB.get(),
                        "the failed frame still pins the input whose cached view was committed");
    expectations.expect(presenter->pinnedInput.use_count() >= 2,
                        "the committed view keeps the source image alive after a failure");

    // Now the caller releases every external reference and the same input is presented again with a
    // valid overlay; the cached view must be valid and its identity must match, not recycled.
    const VkImage cachedImage = presenter->displayImage;
    const std::uint32_t cachedGeneration = presenter->displayGeneration;
    const auto recovered =
        renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, inputB, params, {});
    expectations.expect(recovered.ok, "a successful frame follows the failed overlay update");
    expectations.expect(presenter->displayImage == cachedImage &&
                            presenter->displayGeneration == cachedGeneration &&
                            *presenter->displayView,
                        "the cached display view identity survives the failed update");
}

// A partial device-resource creation failure (e.g. after the vertex shader) must never mark the
// pipeline ready, and a later attempt must safely rebuild.
void testPartialDeviceResourcesFailClosed(Expectations& expectations,
                                          const std::shared_ptr<DeviceAllocatorState>& control,
                                          const std::shared_ptr<const GpuDisplayImage>& input) {
    using bloom::render::present_image_detail::clearPresentDeviceResourceFaultForTesting;
    using bloom::render::present_image_detail::setPresentDeviceResourceFaultForTesting;
    auto presenter = std::make_shared<PresentImagePipeline>();
    const auto params = identityParams(2, 2);

    for (const std::uint32_t stage : {0U, 1U, 2U}) {
        setPresentDeviceResourceFaultForTesting(stage);
        const auto faulted = renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2, 2,
                                                 input, params, {});
        clearPresentDeviceResourceFaultForTesting();
        expectations.expect(!faulted.ok, "a partial device-resource creation fails the frame");
        expectations.expect(!presenter->deviceResourcesReady,
                            "a partial creation never marks the pipeline ready");
    }

    const auto rebuilt =
        renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, input, params, {});
    expectations.expect(rebuilt.ok && presenter->deviceResourcesReady,
                        "a later attempt safely rebuilds the device resources");
}

// Invalid caller-derived parameters must be rejected as InvalidArgument before any presenter state
// is mutated or a command recorded, so a stale uniform cannot be rendered.
void testParamValidation(Expectations& expectations,
                         const std::shared_ptr<DeviceAllocatorState>& control,
                         const std::shared_ptr<const GpuDisplayImage>& input) {
    const auto expectRejected = [&](const GpuPresentImageParams& params, const char* label) {
        const auto rendered =
            renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, input, params, {});
        expectations.expect(!rendered.ok &&
                                rendered.code == GpuPresentationTargetCode::InvalidArgument,
                            std::string(label) + " is rejected as InvalidArgument");
    };

    auto targetMismatch = identityParams(2, 2);
    targetMismatch.targetWidth = 3;
    expectRejected(targetMismatch, "a target extent that does not match the attachment");

    auto zeroDestination = identityParams(2, 2);
    zeroDestination.destination.width = 0.0F;
    expectRejected(zeroDestination, "a zero-width destination rectangle");

    auto nanSource = identityParams(2, 2);
    nanSource.source.x = std::numeric_limits<double>::quiet_NaN();
    expectRejected(nanSource, "a NaN source origin");

    auto infiniteColor = identityParams(2, 2);
    infiniteColor.backgroundColor.red = std::numeric_limits<float>::infinity();
    expectRejected(infiniteColor, "an infinite background color");

    auto badChannel = identityParams(2, 2);
    badChannel.channel = static_cast<GpuPresentChannel>(99);
    expectRejected(badChannel, "an out-of-range channel mode");

    // A lawful crop/pan that extends beyond the display image must still be accepted.
    auto cropped = identityParams(2, 2);
    cropped.source = GpuPresentSourceWindow{-0.5, -0.5, 3.0, 3.0};
    const auto rendered =
        renderOffscreen(control, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, input, cropped, {});
    expectations.expect(rendered.ok, "a lawful clipped/cropped source window is accepted");
}

void testLifetimePin(Expectations& expectations,
                     const std::shared_ptr<DeviceAllocatorState>& control,
                     const std::shared_ptr<const GpuDisplayImage>& input) {
    auto presenter = std::make_shared<PresentImagePipeline>();
    const auto rendered = renderOffscreenWith(control, presenter, VK_FORMAT_R8G8B8A8_UNORM, 2, 2,
                                              input, identityParams(2, 2), {});
    expectations.expect(rendered.ok, "the pin probe render succeeds");
    expectations.expect(presenter->pinnedInput.get() == input.get(),
                        "the presenter strongly pins the input until the render fence is known");
    expectations.expect(presenter->pinnedInput.use_count() >= 2,
                        "the pin keeps the shared resident image alive");
}

[[nodiscard]] std::vector<Rgba32f>
patternPixels(const std::uint32_t width, const std::uint32_t height, const bool secondPattern) {
    std::vector<Rgba32f> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx =
                width > 1U ? static_cast<float>(x) / static_cast<float>(width - 1U) : 0.0F;
            const float fy =
                height > 1U ? static_cast<float>(y) / static_cast<float>(height - 1U) : 0.0F;
            float alpha = 1.0F;
            float straightRed = 0.0F;
            float straightGreen = 0.0F;
            float straightBlue = 0.0F;
            if (!secondPattern) {
                alpha = 0.25F + 0.75F * (static_cast<float>((x * 3U + y * 5U) % 8U) / 7.0F);
                straightRed = 2.0F * fx - 0.5F; // signed range
                straightGreen = 1.5F * fy;      // HDR range
                straightBlue = 3.0F * fx * fy;  // HDR range
            } else {
                alpha = ((x / 7U + y / 5U) % 3U == 0U) ? 0.5F : 1.0F;
                const bool even = ((x + y) & 1U) == 0U;
                straightRed = even ? 1.2F : 0.05F;
                straightGreen = even ? 0.3F : 0.8F;
                straightBlue = even ? -0.1F : 1.5F;
            }
            if ((x + y) % 97U == 0U) {
                alpha = 0.0F;
                straightRed = 0.0F;
                straightGreen = 0.0F;
                straightBlue = 0.0F;
            }
            const auto value = Rgba32f::fromPremultiplied(
                straightRed * alpha, straightGreen * alpha, straightBlue * alpha, alpha);
            if (!value) {
                return {};
            }
            pixels.push_back(*value.value());
        }
    }
    return pixels;
}

// Non-uniform, larger-than-one-workgroup resident display proof against the CPU OCIO oracle. This
// is the fixture that catches the 1D-dispatch defect the tiny uniform tests missed: every pixel
// must be processed, in two consecutive jobs with different patterns.
void testResidentDisplayNonUniform(
    Expectations& expectations, GpuImageUpload& uploader, GpuResidentDisplay& display,
    const bloom::color::PreparedCpuDisplayProcessorHandle& processor) {
    struct Case final {
        std::uint32_t width;
        std::uint32_t height;
        bool secondPattern;
    };
    const Case cases[] = {{257, 19, false}, {257, 19, true}, {1280, 720, false}, {1280, 720, true}};
    for (const Case& testCase : cases) {
        const auto imageWindow = *window(0, 0, testCase.width, testCase.height);
        const auto pixels = patternPixels(testCase.width, testCase.height, testCase.secondPattern);
        expectations.expect(pixels.size() ==
                                static_cast<std::size_t>(testCase.width) * testCase.height,
                            "non-uniform pattern builds");
        if (pixels.size() != static_cast<std::size_t>(testCase.width) * testCase.height) {
            continue;
        }
        auto image = makeImage(imageWindow, pixels);
        if (!image) {
            expectations.expect(false, "non-uniform CPU image builds");
            continue;
        }
        const auto cpu = cpuDisplayFrame(processor, *image);
        expectations.expect(cpu.has_value(), "the CPU OCIO oracle produces a frame");
        if (!cpu) {
            continue;
        }
        auto resident = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*image)));
        if (!resident) {
            expectations.expect(false, "non-uniform input uploads");
            continue;
        }
        auto shared = std::make_shared<const GpuImage>(std::move(*resident));
        const auto gpu = displayReadback(display, shared);
        expectations.expect(gpu.has_value(), "non-uniform resident display reads back");
        if (!gpu) {
            continue;
        }
        const std::size_t mismatch = firstParityMismatch(*gpu, *cpu);
        const std::string label = std::to_string(testCase.width) + "x" +
                                  std::to_string(testCase.height) +
                                  (testCase.secondPattern ? " pattern B" : " pattern A");
        expectations.expect(mismatch == std::numeric_limits<std::size_t>::max(),
                            label +
                                ": every pixel matches the CPU OCIO oracle (first mismatch at " +
                                std::to_string(mismatch) + ")");
        std::cout << "resident non-uniform " << label
                  << (mismatch == std::numeric_limits<std::size_t>::max()
                          ? ": all " + std::to_string(gpu->size()) + " pixels match"
                          : ": MISMATCH at " + std::to_string(mismatch))
                  << '\n';
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
            return expectations.failures() == 0 ? 0 : 1;
        }
        auto uploader = GpuImageUpload::create(*device.device);
        auto display = GpuResidentDisplay::create(*device.device);
        expectations.expect(uploader.hasValue() && display.hasValue(),
                            "the upload and resident-display hosts are created");
        if (!uploader || !display) {
            return 1;
        }
        auto control = bloom::render::GpuRendererAccess::state(*device.device);
        expectations.expect(control != nullptr, "device allocator state is available");
        auto sentinel =
            makeDisplayBundle(*uploader.upload, *display.display, sentinelPixels(), 2, 2);
        expectations.expect(sentinel.has_value(), "sentinel display bundle builds");
        if (control == nullptr || !sentinel) {
            return 1;
        }
        testIdentity(expectations, *device.device, *uploader.upload, *display.display);
        testOracleChannels(expectations, control, sentinel->input, sentinel->bytes);
        testOracleZoomCrop(expectations, control, *uploader.upload, *display.display,
                           sentinel->input, sentinel->bytes);
        testChecker(expectations, control, sentinel->input, sentinel->bytes);
        testOverlay(expectations, control, sentinel->input, sentinel->bytes);
        testRejectionsAndBudgets(expectations, *device.device, options.loader_path, control,
                                 sentinel->input);
        testFailedOverlayRetainsPin(expectations, control, *uploader.upload, *display.display,
                                    sentinel->input);
        testPartialDeviceResourcesFailClosed(expectations, control, sentinel->input);
        testParamValidation(expectations, control, sentinel->input);
        testLifetimePin(expectations, control, sentinel->input);
        auto processor = buildCpuDisplayProcessor();
        expectations.expect(processor.has_value(), "the CPU OCIO display processor builds");
        if (processor) {
            testResidentDisplayNonUniform(expectations, *uploader.upload, *display.display,
                                          *processor);
        }
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " offscreen present expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU present offscreen native proof\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
