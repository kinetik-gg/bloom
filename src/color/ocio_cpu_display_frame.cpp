#include <bloom/color/ocio_cpu_display_frame.hpp>

#include "ocio_internal.hpp"
#include <bloom/core/floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <optional>
#include <vector>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::ImageStatus;
using bloom::render::Rgba32f;
using bloom::render::Rgba8;

[[nodiscard]] ImageError codeError(const ImageErrorCode code) noexcept {
    return ImageError::codeOnly(code);
}

// Straight-rgba8 sRGB packing: clamp to [0, 1], convert exactly to binary64, multiply by 255, and
// quantize as floor(value + 0.5) -- the same clamp/quantize rule documented for the shared PNG
// straight-RGBA8 packing intent in docs/architecture/frame-output.md.
[[nodiscard]] std::uint8_t clampQuantizeToByte(const float value) noexcept {
    const double clamped = std::clamp(static_cast<double>(value), 0.0, 1.0);
    return static_cast<std::uint8_t>(std::floor(clamped * 255.0 + 0.5));
}

// DisplayProcessorIdentityV1 is move-construct only (see display_processor_identity.hpp), so a
// PreparedDisplayFrame cannot borrow or move out of the const& handle's own identity_ member. It
// gets its own independent identity by copying the canonical bytes and re-adopting them --
// re-validating nothing new, since adoptDisplayProcessorIdentityV1 only checks the bytes came
// from a prior successful validated write, which they did.
[[nodiscard]] std::optional<bloom::color::DisplayProcessorIdentityV1>
cloneIdentity(const bloom::color::DisplayProcessorIdentityV1& identity) {
    try {
        const auto bytes = identity.canonicalBytes();
        std::vector<std::byte> copy(bytes.begin(), bytes.end());
        auto adoption = bloom::color::adoptDisplayProcessorIdentityV1(std::move(copy));
        if (!adoption) {
            return std::nullopt;
        }
        return std::move(adoption).takeIdentity();
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

} // namespace

namespace bloom::color {

ImageStatus applyBloomNeutralDisplayChunk(const PreparedCpuDisplayProcessorHandle& handle,
                                          const std::span<const Rgba32f> sourcePixels,
                                          const std::span<float> scratchRgba,
                                          const std::span<Rgba8> destinationPixels) noexcept {
    const std::size_t n = sourcePixels.size();
    if (destinationPixels.size() != n || scratchRgba.size() != n * 4) {
        return codeError(ImageErrorCode::InvalidParameter);
    }
    if (n == 0) {
        return std::nullopt;
    }
    if (!core::supportsReferenceFloatingPointEnvironment<float>()) {
        return codeError(ImageErrorCode::UnsupportedFloatingPointEnvironment);
    }

    // Steps 1-2: read finite premultiplied RGBA32F; exact-zero alpha canonicalizes to exact
    // positive-zero RGB, otherwise one binary32 round-to-nearest-ties-even divide per component
    // (ordinary IEEE-754 float division under the required environment), rejecting non-finite
    // results. Alpha is carried through unchanged into scratch's fourth lane.
    for (std::size_t i = 0; i < n; ++i) {
        const auto& pixel = sourcePixels[i];
        const float red = pixel.red();
        const float green = pixel.green();
        const float blue = pixel.blue();
        const float alpha = pixel.alpha();
        if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue) ||
            !std::isfinite(alpha)) {
            return codeError(ImageErrorCode::NonFiniteResult);
        }

        float straightRed = 0.0F;
        float straightGreen = 0.0F;
        float straightBlue = 0.0F;
        if (alpha != 0.0F) {
            straightRed = red / alpha;
            straightGreen = green / alpha;
            straightBlue = blue / alpha;
            if (!std::isfinite(straightRed) || !std::isfinite(straightGreen) ||
                !std::isfinite(straightBlue)) {
                return codeError(ImageErrorCode::NonFiniteResult);
            }
        }

        scratchRgba[(i * 4) + 0] = straightRed;
        scratchRgba[(i * 4) + 1] = straightGreen;
        scratchRgba[(i * 4) + 2] = straightBlue;
        scratchRgba[(i * 4) + 3] = alpha;
    }

    // Step 3: the OCIO CPU processor is applied to the RGB lanes only. The custom-stride
    // PackedImageDesc below describes exactly 3 channels (R, G, B) with a 16-byte pixel stride
    // over the 4-float scratch buffer, so OCIO's own code addresses only the R/G/B floats and
    // never sees or writes the 4th (alpha) float of each pixel -- alpha is therefore untouched by
    // construction (step 4), not by a separate copy-back step.
    const auto& cpuProcessor = handle.impl().cpuProcessor();
    try {
        constexpr auto floatBytes = static_cast<std::ptrdiff_t>(sizeof(float));
        OCIO::PackedImageDesc desc(scratchRgba.data(), static_cast<long>(n), 1L, 3L,
                                   OCIO::BIT_DEPTH_F32, floatBytes, floatBytes * 4,
                                   floatBytes * 4 * static_cast<std::ptrdiff_t>(n));
        cpuProcessor->apply(desc);
    } catch (const OCIO::Exception&) {
        return codeError(ImageErrorCode::InvalidState);
    } catch (const std::exception&) {
        return codeError(ImageErrorCode::InvalidState);
    }

    // Steps 5-6: reject non-finite processor RGB output; pack the declared straight-rgba8 sRGB
    // bytes. Alpha (unchanged since step 2) is packed with the identical clamp/quantize rule.
    for (std::size_t i = 0; i < n; ++i) {
        const float red = scratchRgba[(i * 4) + 0];
        const float green = scratchRgba[(i * 4) + 1];
        const float blue = scratchRgba[(i * 4) + 2];
        const float alpha = scratchRgba[(i * 4) + 3];
        if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)) {
            return codeError(ImageErrorCode::NonFiniteResult);
        }
        destinationPixels[i] = Rgba8{clampQuantizeToByte(red), clampQuantizeToByte(green),
                                     clampQuantizeToByte(blue), clampQuantizeToByte(alpha)};
    }

    return std::nullopt; // step 7: nothing is published from here -- the caller owns the frame.
}

PreparedDisplayFrame::PreparedDisplayFrame(const render::ImageWindow displayWindow,
                                           const core::PixelAspectRatio pixelAspect,
                                           const render::PackedImageLayout layout,
                                           std::vector<Rgba8> pixels,
                                           DisplayProcessorIdentityV1 identity) noexcept
    : displayWindow_(displayWindow), pixelAspect_(pixelAspect), layout_(layout),
      pixels_(std::move(pixels)), identity_(std::move(identity)) {}

PreparedDisplayFrame::PreparedDisplayFrame(PreparedDisplayFrame&&) noexcept = default;

bool PreparedDisplayFrame::isValid() const noexcept { return pixels_.size() == layout_.pixelCount; }

ImageResult<PreparedDisplayFrame> produceBloomNeutralDisplayFrame(
    const PreparedCpuDisplayProcessorHandle& handle, const render::Rgba32fImageView source,
    const std::size_t chunkPixelCount, const std::size_t pixelStorageByteLimit,
    const CancellationPredicateRef isCancelled) noexcept {
    const auto sourceDescriptorValue = source.descriptor();
    if (!sourceDescriptorValue.has_value() ||
        source.pixels().size() != sourceDescriptorValue->layout().pixelCount) {
        return ImageResult<PreparedDisplayFrame>::failure(codeError(ImageErrorCode::InvalidState));
    }
    if (chunkPixelCount == 0) {
        return ImageResult<PreparedDisplayFrame>::failure(
            codeError(ImageErrorCode::InvalidParameter));
    }
    const auto sourceDescriptor = *sourceDescriptorValue;
    const auto displayWindow = sourceDescriptor.dataWindow();

    const auto layoutResult = render::checkedRgba8Layout(displayWindow.extent());
    if (!layoutResult) {
        return ImageResult<PreparedDisplayFrame>::failure(*layoutResult.error());
    }
    const auto layout = *layoutResult.value();
    if (layout.pixelStorageBytes > pixelStorageByteLimit) {
        return ImageResult<PreparedDisplayFrame>::failure(ImageError::pixelStorageBudgetExceeded(
            layout.pixelStorageBytes, pixelStorageByteLimit));
    }

    std::vector<Rgba8> pixels;
    std::vector<float> scratch;
    try {
        pixels.resize(layout.pixelCount, Rgba8{0, 0, 0, 0});
        scratch.resize(
            std::min(chunkPixelCount, layout.pixelCount == 0 ? std::size_t{1} : layout.pixelCount) *
            4);
    } catch (const std::bad_alloc&) {
        return ImageResult<PreparedDisplayFrame>::failure(
            ImageError::allocationFailure(layout.pixelStorageBytes));
    } catch (const std::length_error&) {
        return ImageResult<PreparedDisplayFrame>::failure(
            ImageError::allocationFailure(layout.pixelStorageBytes));
    }

    std::size_t processed = 0;
    while (processed < layout.pixelCount) {
        if (isCancelled()) {
            return ImageResult<PreparedDisplayFrame>::failure(
                codeError(ImageErrorCode::InvalidState));
        }
        const std::size_t remaining = layout.pixelCount - processed;
        const std::size_t thisChunk = std::min(chunkPixelCount, remaining);
        const auto chunkStatus =
            applyBloomNeutralDisplayChunk(handle, source.pixels().subspan(processed, thisChunk),
                                          std::span<float>(scratch).first(thisChunk * 4),
                                          std::span<Rgba8>(pixels).subspan(processed, thisChunk));
        if (chunkStatus.has_value()) {
            return ImageResult<PreparedDisplayFrame>::failure(*chunkStatus);
        }
        processed += thisChunk;
    }

    auto clonedIdentity = cloneIdentity(handle.identity());
    if (!clonedIdentity.has_value()) {
        return ImageResult<PreparedDisplayFrame>::failure(
            ImageError::allocationFailure(handle.identity().canonicalBytes().size()));
    }

    PreparedDisplayFrame frame(displayWindow, sourceDescriptor.pixelAspect(), layout,
                               std::move(pixels), std::move(*clonedIdentity));
    return ImageResult<PreparedDisplayFrame>::success(std::move(frame));
}

} // namespace bloom::color
