// Runtime-side native proof that GpuAffine's composed-matrix form matches the ACTUAL runtime
// parented CPU row (detail::ParentedLayerTransform::row). This lives in src/runtime/tests because
// runtime may depend on render, while production render may not depend on runtime.
//
// The test builds real composed parent*child LayerMatrix values exactly as the CPU composition
// evaluator does (parent.times(child) of LayerMatrix::authored), uploads a decoded RGBA32F source,
// runs GpuAffine::beginAffineMatrix, reads back, and compares every component to
// ParentedLayerTransform::row under the documented 2e-6 abs-or-rel gate. It covers parent
// negative/nonuniform scale with rotations (which produce shear), tiny transforms, a collapsed
// (zero-scale) transform, nonzero source origin, a non-square pixel aspect, and clipping.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/gpu_affine.hpp>

#include "../layer_parent_transform.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::document::Vec2d;
using bloom::render::GpuAffine;
using bloom::render::GpuAffineDiagnosticCode;
using bloom::render::GpuAffineMatrix;
using bloom::render::GpuAffinePollResult;
using bloom::render::Rgba32f;
using bloom::runtime::detail::LayerMatrix;
using bloom::runtime::detail::ParentedLayerTransform;

// The exact conversion from the runtime's device-space composed LayerMatrix (composition
// coordinates, pixel-centre + 0.5 convention) to GpuAffine's local-0-based-pixel-centre to
// absolute-output-index convention:
//   X = a*(lx + ox + 0.5) + b*(ly + oy + 0.5) + m.x - 0.5
// where o is the source data-window origin.
[[nodiscard]] GpuAffineMatrix toGpuAffineMatrix(const LayerMatrix& matrix,
                                                const ImageWindow sourceWindow) {
    const double ox = static_cast<double>(sourceWindow.originX()) + 0.5;
    const double oy = static_cast<double>(sourceWindow.originY()) + 0.5;
    return GpuAffineMatrix{.a = matrix.a,
                           .b = matrix.b,
                           .tx = matrix.a * ox + matrix.b * oy + matrix.x - 0.5,
                           .c = matrix.c,
                           .d = matrix.d,
                           .ty = matrix.c * ox + matrix.d * oy + matrix.y - 0.5};
}

void runParented(Expectations& expectations, GpuAffine& affine, GpuImageUpload& uploader,
                 const std::string& name, const LayerMatrix& matrix, const ImageWindow dataWindow,
                 const ImageWindow displayWindow, const PixelAspectRatio aspect,
                 const float opacity, const bool expectAllTransparent,
                 const std::vector<Rgba32f>& sourcePixels) {
    auto sourceImage = makeImage(dataWindow, displayWindow, aspect, sourcePixels);
    expectations.expect(sourceImage.has_value(), name + ": source builds");
    if (!sourceImage) {
        return;
    }
    auto resident = upload(uploader, std::make_shared<const Rgba32fImage>(std::move(*sourceImage)));
    expectations.expect(resident.has_value(), name + ": source uploads");
    if (!resident) {
        return;
    }
    auto source = std::make_shared<const GpuImage>(std::move(*resident));

    const ParentedLayerTransform parented(matrix, dataWindow, 1.0, 1.0, opacity);
    const auto clip = window(-96, -96, 192, 192);
    const auto bounds = parented.supportBounds(clip);
    expectations.expect(bounds.has_value(), name + ": support bounds non-empty");
    if (!bounds) {
        return;
    }
    const auto outputWindow = *bounds;

    const auto begin = affine.beginAffineMatrix(
        {source, outputWindow, toGpuAffineMatrix(matrix, dataWindow), opacity}, kBudget);
    expectations.expect(begin.code == GpuAffineDiagnosticCode::None,
                        name + ": begin accepted: " + begin.message);
    if (begin.code != GpuAffineDiagnosticCode::None) {
        return;
    }
    GpuAffinePollResult poll = GpuAffinePollResult::Pending;
    while (poll == GpuAffinePollResult::Pending) {
        poll = affine.poll();
    }
    expectations.expect(poll == GpuAffinePollResult::Ready,
                        name + ": completes: " + affine.diagnostic().message);
    if (poll != GpuAffinePollResult::Ready) {
        return;
    }
    auto result = affine.takeImage();
    expectations.expect(result.isValid(), name + ": resident output published");
    expectations.expect(result.displayWindow() == source->displayWindow() &&
                            result.pixelAspect() == source->pixelAspect(),
                        name + ": output preserves source display window and pixel aspect");

    const auto oracleImage = makeImage(dataWindow, displayWindow, aspect, sourcePixels);
    if (!oracleImage) {
        return;
    }
    const auto view = oracleImage->view();
    if (!view) {
        return;
    }
    const auto outWidth = outputWindow.extent().width();
    const auto outHeight = outputWindow.extent().height();
    std::vector<Rgba32f> expected(static_cast<std::size_t>(outWidth) * outHeight,
                                  Rgba32f::transparent());
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        auto row =
            std::span<Rgba32f>(expected).subspan(static_cast<std::size_t>(y) * outWidth, outWidth);
        const auto status =
            parented.row(*view.value(), outputWindow,
                         outputWindow.originY() + static_cast<std::int64_t>(y), row);
        expectations.expect(!status.has_value(), name + ": parented oracle row succeeds");
    }
    if (expectAllTransparent) {
        bool allTransparent = true;
        for (const auto& pixel : expected) {
            if (pixel.alpha() != 0.0F) {
                allTransparent = false;
            }
        }
        expectations.expect(allTransparent, name + ": the parented oracle is the empty result");
    }
    const auto readback = bloom::render::readbackResidentImage(result, kBudget);
    expectations.expect(readback.hasValue(), name + ": readback for oracle");
    if (readback) {
        static_cast<void>(pixelsMatch(readback.pixels, expected, expectations, name));
        static_cast<void>(endpointsAlphaExact(readback.pixels, expected, expectations, name));
    }
}

void runParentedCases(Expectations& expectations, GpuDevice& device) {
    auto affine = GpuAffine::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(affine.hasValue() && uploader.hasValue(), "affine hosts created");
    if (!affine || !uploader) {
        return;
    }
    const auto origin = window(7, -3, 9, 6);
    const auto display = window(7, -3, 9, 6);
    const auto square = PixelAspectRatio::square();
    const auto wide = *PixelAspectRatio::create(2, 1);
    const Vec2d originPosition{4.0, 2.0};
    const Vec2d childPosition{-1.5, 3.25};
    const auto originPixels = semanticPixels(origin.extent().width(), origin.extent().height());

    // 1. Parent rotation + uniform scale composed with a child rotation + nonuniform scale, which
    //    produces shear in the composed matrix. Nonzero source origin and a non-square PAR.
    const auto parent1 = LayerMatrix::authored(originPosition, {1.5, -0.5}, {1.5, 1.5}, 30.0);
    const auto child1 = LayerMatrix::authored(childPosition, {0.25, 0.5}, {2.0, 0.5}, -15.0);
    runParented(expectations, *affine.affine, *uploader.upload, "parent-rot-scale-child-shear",
                parent1.times(child1), origin, display, wide, 1.0F, false, originPixels);

    // 2. Parent NEGATIVE nonuniform scale with rotation composed with a child opposite-sign
    //    nonuniform scale and rotation.
    const auto parent2 = LayerMatrix::authored({-3.0, 5.0}, {0.75, 0.75}, {-1.5, 1.0}, 45.0);
    const auto child2 = LayerMatrix::authored({2.0, -2.0}, {-0.5, 0.25}, {0.75, -0.5}, 10.0);
    runParented(expectations, *affine.affine, *uploader.upload, "parent-negative-scale-shear",
                parent2.times(child2), origin, display, square, 0.75F, false, originPixels);

    // 3. A tiny (near-degenerate but invertible) parent and child scale.
    const auto parent3 = LayerMatrix::authored({0.0, 0.0}, {0.0, 0.0}, {1e-3, 1e-3}, 17.0);
    const auto child3 = LayerMatrix::authored({0.0, 0.0}, {0.0, 0.0}, {1e-3, 2e-3}, -5.0);
    runParented(expectations, *affine.affine, *uploader.upload, "tiny-scale", parent3.times(child3),
                origin, display, square, 1.0F, false, originPixels);

    // 4. A collapsed child (scale X == 0) makes the composed determinant zero: the empty-layer
    //    result. ParentedLayerTransform::invertible() is false and production publishes no image;
    //    the GPU composed-matrix path must be all-transparent here too.
    const auto parent4 = LayerMatrix::authored({0.0, 0.0}, {0.4, 0.4}, {1.2, 1.2}, 12.0);
    const auto child4 = LayerMatrix::authored({1.0, 1.0}, {0.2, 0.2}, {0.0, 0.8}, 3.0);
    runParented(expectations, *affine.affine, *uploader.upload, "zero-scale-empty",
                parent4.times(child4), origin, display, square, 1.0F, true, originPixels);

    // 5. Dense HDR (+/- cancellation, tiny positive alpha) parented fixtures at fractional, exact
    //    90-degree, and near-integer composed boundaries, with the bit-exact endpoint-alpha gate.
    const auto denseWindow = window(3, -2, 64, 16);
    const auto densePixels = denseHdrPixels(64, 16);
    const Vec2d denseParentPosition{0.3, -0.7};
    const Vec2d denseChildPosition{-1.5, 3.25};
    const auto denseParent =
        LayerMatrix::authored(denseParentPosition, {1.5, -0.5}, {1.25, 0.75}, 15.0);
    const auto denseChild =
        LayerMatrix::authored(denseChildPosition, {0.25, 0.5}, {2.0, 0.5}, -15.0);
    runParented(expectations, *affine.affine, *uploader.upload, "dense-hdr-fractional-shear",
                denseParent.times(denseChild), denseWindow, denseWindow, square, 1.0F, false,
                densePixels);
    const auto parent90 = LayerMatrix::authored({0.0, 0.0}, {0.5, 0.5}, {1.0, 1.5}, 90.0);
    runParented(expectations, *affine.affine, *uploader.upload, "dense-hdr-rotate-90",
                parent90.times(denseChild), denseWindow, denseWindow, square, 1.0F, false,
                densePixels);
    const auto parentNear =
        LayerMatrix::authored({2.0000001, -3.9999999}, {1.5, -0.5}, {1.0, 1.0}, 0.0);
    runParented(expectations, *affine.affine, *uploader.upload, "dense-hdr-near-integer",
                parentNear.times(denseChild), denseWindow, denseWindow, square, 1.0F, false,
                densePixels);
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
        runParentedCases(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " parented affine expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU affine vs runtime ParentedLayerTransform::row\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
