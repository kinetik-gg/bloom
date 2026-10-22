#pragma once

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/render/gpu_affine.hpp>

namespace bloom::runtime::detail {

// Coordinate convention: image storage uses integer pixel centres. Permute the input data
// rectangle at its own top-left, even when it lies outside the display canvas. The canvas and
// PAR stay unchanged. This is an image operation; it does not resize the composition.
[[nodiscard]] inline render::ImageResult<render::ImageWindow>
discreteOutputWindow(const DiscreteImageKernel kernel, const render::ImageWindow input,
                     const double horizontalScale = 1.0, const double verticalScale = 1.0) {
    const bool swap = (kernel.quarterTurns & 1U) != 0;
    if (swap && horizontalScale != verticalScale) {
        const double width = std::ceil(std::nextafter(input.extent().height() *
            (horizontalScale / verticalScale), 0.0));
        const double height = std::ceil(std::nextafter(input.extent().width() *
            (verticalScale / horizontalScale), 0.0));
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
            width > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::uint32_t>::max())
            return render::ImageWindow::create(0, 0, 0, 0);
        return render::ImageWindow::create(input.originX(), input.originY(),
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    }
    return render::ImageWindow::create(input.originX(), input.originY(),
                                       swap ? input.extent().height() : input.extent().width(),
                                       swap ? input.extent().width() : input.extent().height());
}

[[nodiscard]] inline std::pair<std::uint32_t, std::uint32_t>
discreteSourceCoordinate(const DiscreteImageKernel kernel, const std::uint32_t x,
                         const std::uint32_t y, const std::uint32_t width,
                         const std::uint32_t height) noexcept {
    std::uint32_t sx = x;
    std::uint32_t sy = y;
    switch (kernel.quarterTurns) {
    case 1:
        sx = y;
        sy = height - 1U - x;
        break;
    case 2:
        sx = width - 1U - x;
        sy = height - 1U - y;
        break;
    case 3:
        sx = width - 1U - y;
        sy = x;
        break;
    default:
        break;
    }
    return {kernel.horizontal ? width - 1U - sx : sx, kernel.vertical ? height - 1U - sy : sy};
}

// The production affine sampler's integer-centre convention. Coefficients and translations
// are exact integers: every inverse-map factor is zero, so its bilinear arm selects one tap
// without interpolation or colour/opacity arithmetic. No per-pixel host metadata is prepared.
[[nodiscard]] inline render::GpuAffineMatrix discreteAffineMatrix(const DiscreteImageKernel kernel,
                                                                  const render::ImageWindow input,
                                                                  const double horizontalScale = 1.0,
                                                                  const double verticalScale = 1.0) {
    const double w = static_cast<double>(input.extent().width()) - 1.0;
    const double h = static_cast<double>(input.extent().height()) - 1.0;
    const double sx = kernel.horizontal ? -1.0 : 1.0;
    const double sy = kernel.vertical ? -1.0 : 1.0;
    const double fx = kernel.horizontal ? w : 0.0;
    const double fy = kernel.vertical ? h : 0.0;
    render::GpuAffineMatrix result;
    switch (kernel.quarterTurns) {
    case 1:
        result = {0, -sy, h - fy, sx, 0, fx};
        break;
    case 2:
        result = {-sx, 0, w - fx, 0, -sy, h - fy};
        break;
    case 3:
        result = {0, sy, fy, -sx, 0, w - fx};
        break;
    default:
        result = {sx, 0, fx, 0, sy, fy};
        break;
    }
    result.tx += static_cast<double>(input.originX());
    result.ty += static_cast<double>(input.originY());
    if ((kernel.quarterTurns & 1U) && horizontalScale != verticalScale) {
        const double rx = horizontalScale / verticalScale;
        const double ry = verticalScale / horizontalScale;
        result.a *= rx; result.b *= rx;
        result.c *= ry; result.d *= ry;
        result.tx = (result.tx - static_cast<double>(input.originX()) + 0.5) * rx +
                    static_cast<double>(input.originX()) - 0.5;
        result.ty = (result.ty - static_cast<double>(input.originY()) + 0.5) * ry +
                    static_cast<double>(input.originY()) - 0.5;
    }
    return result;
}

[[nodiscard]] inline ImageEffectResult
applyDiscreteImageEffect(const DiscreteImageKernel kernel, const render::Rgba32fImage& input,
                         const std::size_t pixelBudget, const CancellationToken& cancellation,
                         const OperationIndex operation,
                         const EvaluationProgressCallback& progress,
                         const double horizontalScale = 1.0, const double verticalScale = 1.0) {
    ImageEffectResult result;
    const auto refuse = [&](const EvaluationDiagnosticCode code, const char* message) {
        result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, message, {}};
    };
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    const auto source = input.descriptor()->dataWindow();
    const auto output = discreteOutputWindow(kernel, source, horizontalScale, verticalScale);
    if (!output) {
        refuse(EvaluationDiagnosticCode::InvalidParameter, "Discrete image window overflows");
        return result;
    }
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *output.value(), input.descriptor()->displayWindow(), input.descriptor()->pixelAspect());
    if (!descriptor) {
        refuse(EvaluationDiagnosticCode::InvalidParameter, "Discrete image descriptor is invalid");
        return result;
    }
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), pixelBudget);
    if (!builder) {
        refuse(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
               "Discrete image exceeds the pixel storage budget");
        return result;
    }
    const auto view = input.view();
    if (!view) {
        refuse(EvaluationDiagnosticCode::InvalidPixel, "Discrete image input is invalid");
        return result;
    }
    const bool resample = (kernel.quarterTurns & 1U) && horizontalScale != verticalScale;
    const auto affine = discreteAffineMatrix(kernel, source, horizontalScale, verticalScale);
    const double ox = static_cast<double>(source.originX()) + 0.5;
    const double oy = static_cast<double>(source.originY()) + 0.5;
    const LayerMatrix matrix{affine.a, affine.b, affine.c, affine.d,
        affine.tx + 0.5 - affine.a * ox - affine.b * oy,
        affine.ty + 0.5 - affine.c * ox - affine.d * oy};
    const ParentedLayerTransform transform{matrix, source, 1.0, 1.0, 1.0F};
    const auto width = output.value()->extent().width();
    const auto height = output.value()->extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, height});
    for (std::uint32_t y = 0; y < height; ++y) {
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            return result;
        }
        auto row = builder.value()->row(output.value()->originY() + y);
        if (resample) {
            if (transform.row(*view.value(), *output.value(), output.value()->originY() + y,
                              *row.value())) {
                refuse(EvaluationDiagnosticCode::InvalidPixel, "Discrete proxy resample failed");
                return result;
            }
        } else for (std::uint32_t x = 0; x < width; ++x) {
            if ((x & 4095U) == 0 && cancellation.isCancellationRequested()) {
                result.cancelled = true;
                return result;
            }
            const auto [sx, sy] = discreteSourceCoordinate(kernel, x, y, source.extent().width(),
                                                           source.extent().height());
            const auto sourceRow = view.value()->row(source.originY() + sy);
            (*row.value())[x] = (*sourceRow.value())[sx];
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation, y + 1U, height});
    }
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        refuse(EvaluationDiagnosticCode::InvalidPixel, "Discrete image output is invalid");
        return result;
    }
    result.image = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
    return result;
}

} // namespace bloom::runtime::detail
