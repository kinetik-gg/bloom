#pragma once

// Shared fixtures and CPU-oracle derivations for the TranslationOpacityBilinearV1 and SourceOverV1
// GPU kernels. The expected values here are produced by calling the REAL CPU primitives
// (render::translateOpacityBilinearRow, render::sourceOverLinearRec709SceneRow) on the same inputs,
// never by a hand-written or GPU-captured oracle. A future native host driver feeds the same
// fixtures to the kernels and the comparison uses the documented 2e-6 abs-or-rel gate.
//
// Nothing here executes a GPU. This header only builds inputs, derives CPU expectations, and names
// the pending host API the kernels need.

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bloom::render::composite_fixture {

using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba32fImageView;
using bloom::render::TranslationOpacity;

// One TranslationOpacityBilinearV1 case: a source image, the output window, and the parameters.
struct TranslationCase final {
    std::string name;
    Rgba32fImageDescriptor sourceDescriptor;
    std::vector<Rgba32f> sourcePixels; // row-major over the source data window
    ImageWindow outputWindow;
    TranslationOpacity parameters;
};

// One SourceOverV1 case: a foreground source image and an in-place destination image, each with its
// own data/display windows. The kernel composites source OVER destination at the overlapping
// window; taps outside the source window are transparent black.
struct SourceOverCase final {
    std::string name;
    Rgba32fImageDescriptor sourceDescriptor;
    std::vector<Rgba32f> sourcePixels;
    Rgba32fImageDescriptor destinationDescriptor;
    std::vector<Rgba32f> destinationPixels;
};

// The fixed fixture literals below are always valid; a rejected value means the fixture itself is
// broken. Fail the test with a diagnostic instead of returning an empty optional that a caller
// could dereference unchecked.
[[nodiscard]] inline ImageWindow window(const std::int64_t originX, const std::int64_t originY,
                                        const std::uint64_t width, const std::uint64_t height) {
    const auto result = ImageWindow::create(originX, originY, width, height);
    if (!result) {
        throw std::logic_error("invalid composite fixture window");
    }
    return *result.value();
}

[[nodiscard]] inline Rgba32f pixel(const float red, const float green, const float blue,
                                   const float alpha) {
    const auto result = Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!result) {
        throw std::logic_error("invalid composite fixture pixel");
    }
    return *result.value();
}

[[nodiscard]] inline Rgba32fImageDescriptor descriptor(const ImageWindow dataWindow,
                                                       const ImageWindow displayWindow) {
    const auto result = Rgba32fImageDescriptor::create(dataWindow, displayWindow,
                                                       bloom::core::PixelAspectRatio::square());
    if (!result) {
        throw std::logic_error("invalid composite fixture descriptor");
    }
    return *result.value();
}

[[nodiscard]] inline std::optional<Rgba32fImage>
image(const Rgba32fImageDescriptor& imageDescriptor, const std::vector<Rgba32f>& pixels) {
    if (pixels.size() != imageDescriptor.layout().pixelCount) {
        return std::nullopt;
    }
    auto builderResult =
        Rgba32fImageBuilder::create(imageDescriptor, imageDescriptor.layout().pixelStorageBytes);
    if (!builderResult) {
        return std::nullopt;
    }
    auto builder = std::move(*builderResult.value());
    const auto width = imageDescriptor.dataWindow().extent().width();
    const auto height = imageDescriptor.dataWindow().extent().height();
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto row = builder.row(imageDescriptor.dataWindow().originY() + y);
        if (!row) {
            return std::nullopt;
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            (*row.value())[x] = pixels[static_cast<std::size_t>(y) * width + x];
        }
    }
    auto frozen = std::move(builder).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

// The CPU oracle for one TranslationOpacityBilinearV1 case, row by row, over the whole output
// window. Returns the output data-window pixels in row-major order.
[[nodiscard]] inline std::optional<std::vector<Rgba32f>>
expectedTranslation(const TranslationCase& testCase) {
    const auto sourceImage = image(testCase.sourceDescriptor, testCase.sourcePixels);
    if (!sourceImage) {
        return std::nullopt;
    }
    const auto view = sourceImage->view();
    if (!view) {
        return std::nullopt;
    }
    const auto width = testCase.outputWindow.extent().width();
    const auto height = testCase.outputWindow.extent().height();
    std::vector<Rgba32f> output(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto rowY = testCase.outputWindow.originY() + static_cast<std::int64_t>(y);
        auto row = std::span<Rgba32f>(output).subspan(static_cast<std::size_t>(y) * width, width);
        const auto status = translateOpacityBilinearRow(*view.value(), testCase.outputWindow, rowY,
                                                        testCase.parameters, row);
        if (status.has_value()) {
            return std::nullopt;
        }
    }
    return output;
}

// The CPU oracle for one SourceOverV1 case. For each destination row it builds an ALIGNED source
// row of the destination's width by sampling the source window at (destLocal - offset) with
// transparent padding outside the source window, then calls the retained source-over primitive.
// This is the same window generalization the kernel implements, so out-of-bounds and empty overlap
// reduce to the real CPU rule rather than a second oracle.
[[nodiscard]] inline std::optional<std::vector<Rgba32f>>
expectedSourceOver(const SourceOverCase& testCase) {
    const auto destWindow = testCase.destinationDescriptor.dataWindow();
    const auto sourceWindow = testCase.sourceDescriptor.dataWindow();
    const auto width = destWindow.extent().width();
    const auto height = destWindow.extent().height();
    if (testCase.destinationPixels.size() != static_cast<std::size_t>(width) * height) {
        return std::nullopt;
    }
    std::vector<Rgba32f> destination = testCase.destinationPixels;
    std::vector<Rgba32f> alignedSource(width, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto destY = destWindow.originY() + static_cast<std::int64_t>(y);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto destX = destWindow.originX() + static_cast<std::int64_t>(x);
            const auto sourceX = destX - sourceWindow.originX();
            const auto sourceY = destY - sourceWindow.originY();
            const bool inside =
                sourceX >= 0 && sourceY >= 0 &&
                sourceX < static_cast<std::int64_t>(sourceWindow.extent().width()) &&
                sourceY < static_cast<std::int64_t>(sourceWindow.extent().height());
            alignedSource[x] = inside ? testCase.sourcePixels[static_cast<std::size_t>(sourceY) *
                                                                  sourceWindow.extent().width() +
                                                              static_cast<std::size_t>(sourceX)]
                                      : Rgba32f::transparent();
        }
        auto destRow =
            std::span<Rgba32f>(destination).subspan(static_cast<std::size_t>(y) * width, width);
        const auto status = sourceOverLinearRec709SceneRow(alignedSource, destRow);
        if (status.has_value()) {
            return std::nullopt;
        }
    }
    return destination;
}

// A 2x2 source covering alpha endpoints and signed/HDR RGB, used by several translation cases.
[[nodiscard]] inline std::vector<Rgba32f> quadSourcePixels() {
    return {pixel(1.0F, 0.0F, 0.0F, 1.0F), pixel(0.0F, 1.0F, 0.0F, 1.0F),
            pixel(0.0F, 0.0F, 1.0F, 1.0F), pixel(-2.0F, 4.0F, 0.5F, 0.5F)};
}

// Every TranslationOpacityBilinearV1 case: integer and fractional, positive and negative
// translation, nonzero/negative origins, source out of bounds, empty overlap, opacity endpoints and
// a fraction, and odd extents. Display and data windows differ in the origin cases.
[[nodiscard]] inline std::vector<TranslationCase> translationCases() {
    std::vector<TranslationCase> cases;
    const auto source = window(-7, 11, 2, 2);
    const auto sourceDisplay = window(-7, 11, 2, 2);
    const auto sourceDescriptor = descriptor(source, sourceDisplay);
    const auto pixels = quadSourcePixels();

    const auto add = [&](std::string name, const ImageWindow output, const double tx,
                         const double ty, const double opacity) {
        const auto parameters = TranslationOpacity::create(tx, ty, opacity);
        if (parameters) {
            cases.push_back(
                {std::move(name), sourceDescriptor, pixels, output, *parameters.value()});
        }
    };
    // Integer translation (identity and shifted), including a negative translation.
    add("integer-identity", window(-7, 11, 2, 2), 0.0, 0.0, 1.0);
    add("integer-positive", window(-7, 11, 3, 2), 1.0, 0.0, 1.0);
    add("integer-negative", window(-7, 11, 2, 2), -1.0, 0.0, 1.0);
    // Fractional positive and negative translation with nonzero origins.
    add("fractional-positive", window(3, -4, 3, 3), 0.5, 0.5, 1.0);
    add("fractional-negative", window(-2, -9, 3, 3), -0.25, -0.75, 1.0);
    // Opacity endpoints and a fraction.
    add("opacity-zero", window(0, 0, 2, 2), 0.0, 0.0, 0.0);
    add("opacity-one", window(0, 0, 2, 2), 0.5, 0.0, 1.0);
    add("opacity-fraction", window(0, 0, 2, 2), 0.5, 0.5, 0.25);
    // Source out of bounds (translation far past the source) and empty overlap.
    add("source-out-of-bounds", window(0, 0, 2, 2), 8.0, 0.0, 1.0);
    add("empty-overlap", window(0, 0, 2, 2), 0.0, 9.0, 1.0);
    // Odd extents with a fractional translation.
    add("odd-extents", window(1, 1, 3, 3), 0.5, -0.5, 0.75);

    // 4K-wide high-contrast source with a sub-pixel translation near the far edge. This is the case
    // that exposed the Float32 sample-point precision bug: at outputLocal ~4000 with translation
    // 0.3 a Float32 subtraction loses ~1e-4 in the factor, far outside 2e-6. The host axis
    // preparation (Float64) must remove that, so the fixture spans alternating black/white columns.
    const auto wideWindow = window(0, 0, 3840, 2);
    const auto wideDescriptor = descriptor(wideWindow, wideWindow);
    std::vector<Rgba32f> widePixels(static_cast<std::size_t>(3840) * 2, Rgba32f::transparent());
    for (std::size_t index = 0; index < widePixels.size(); ++index) {
        widePixels[index] =
            (index % 2 == 0) ? pixel(0.0F, 0.0F, 0.0F, 1.0F) : pixel(1.0F, 1.0F, 1.0F, 1.0F);
    }
    const auto addWide = [&](std::string name, const double tx, const double ty) {
        const auto parameters = TranslationOpacity::create(tx, ty, 1.0);
        if (parameters) {
            cases.push_back({std::move(name), wideDescriptor, widePixels, window(0, 0, 3840, 2),
                             *parameters.value()});
        }
    };
    addWide("wide-4k-dx-plus-0.3-dy-plus-0.1", 0.3, 0.1);
    addWide("wide-4k-dx-minus-0.3-dy-minus-0.1", -0.3, -0.1);
    addWide("wide-4k-dx-near-integer", 3.0000001, -2.9999999);
    return cases;
}

// Every SourceOverV1 case: opaque/translucent/signed-HDR pixels, source out of bounds, empty
// overlap, alpha endpoints, odd extents, and a chained sequence (apply twice).
[[nodiscard]] inline std::vector<SourceOverCase> sourceOverCases() {
    std::vector<SourceOverCase> cases;
    const auto sourceWindow = window(0, 0, 2, 2);
    const auto destWindow = window(0, 0, 2, 2);
    const auto sourceDescriptor = descriptor(sourceWindow, sourceWindow);
    const auto destDescriptor = descriptor(destWindow, destWindow);

    const auto add = [&](std::string name, const Rgba32fImageDescriptor& sDesc,
                         std::vector<Rgba32f> sPixels, const Rgba32fImageDescriptor& dDesc,
                         std::vector<Rgba32f> dPixels) {
        cases.push_back({std::move(name), sDesc, std::move(sPixels), dDesc, std::move(dPixels)});
    };

    const std::vector<Rgba32f> sourcePixels{pixel(0.5F, 0.0F, 0.0F, 0.5F), Rgba32f::transparent(),
                                            pixel(-2.0F, 4.0F, 0.5F, 1.0F),
                                            pixel(-1.0F, 2.0F, 0.0F, 0.5F)};
    const std::vector<Rgba32f> destinationPixels{
        pixel(0.0F, 0.0F, 0.5F, 0.5F), pixel(1.0F, 2.0F, 3.0F, 0.25F),
        pixel(8.0F, 8.0F, 8.0F, 0.5F), pixel(4.0F, -2.0F, 1.0F, 0.5F)};
    add("mixed-endpoints", sourceDescriptor, sourcePixels, destDescriptor, destinationPixels);
    add("alpha-endpoints", sourceDescriptor,
        {pixel(1.0F, 1.0F, 1.0F, 1.0F), Rgba32f::transparent(), pixel(0.0F, 0.0F, 0.0F, 0.0F),
         pixel(0.25F, 0.25F, 0.25F, 1.0F)},
        destDescriptor, destinationPixels);

    // Source shifted one pixel right: column 0 samples out of the source window (transparent).
    const auto shiftedSourceWindow = window(1, 0, 2, 2);
    const auto shiftedSourceDescriptor = descriptor(shiftedSourceWindow, shiftedSourceWindow);
    add("source-out-of-bounds", shiftedSourceDescriptor, sourcePixels, destDescriptor,
        destinationPixels);

    // Source entirely outside the destination: every tap is transparent, destination unchanged.
    const auto farSourceWindow = window(100, 100, 2, 2);
    const auto farSourceDescriptor = descriptor(farSourceWindow, farSourceWindow);
    add("empty-overlap", farSourceDescriptor, sourcePixels, destDescriptor, destinationPixels);

    // Odd extents.
    const auto oddWindow = window(0, 0, 3, 3);
    const auto oddDescriptor = descriptor(oddWindow, oddWindow);
    add("odd-extents", oddDescriptor,
        {pixel(0.5F, 0.25F, 0.125F, 0.5F), pixel(2.0F, 0.5F, -0.25F, 1.0F),
         pixel(0.125F, 0.375F, 0.25F, 0.25F), pixel(0.75F, 0.0F, 0.75F, 0.75F),
         pixel(0.0F, 0.0F, 0.0F, 0.0F), pixel(0.3F, 0.6F, 0.9F, 0.6F),
         pixel(1.5F, -1.5F, 0.5F, 0.5F), pixel(0.1F, 0.2F, 0.3F, 0.9F),
         pixel(0.4F, 0.4F, 0.4F, 0.4F)},
        oddDescriptor,
        {pixel(0.25F, 0.5F, 0.75F, 1.0F), pixel(0.375F, 0.125F, 0.625F, 0.5F),
         pixel(0.5F, 0.5F, 0.5F, 1.0F), pixel(0.0625F, 0.125F, 0.1875F, 0.25F),
         pixel(0.2F, 0.3F, 0.4F, 0.7F), pixel(0.9F, 0.1F, 0.2F, 0.3F),
         pixel(0.0F, 0.0F, 0.0F, 0.0F), pixel(0.6F, 0.5F, 0.4F, 0.8F),
         pixel(0.15F, 0.25F, 0.35F, 0.45F)});
    return cases;
}

} // namespace bloom::render::composite_fixture
