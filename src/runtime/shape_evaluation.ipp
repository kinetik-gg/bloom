[&](const CompiledShape& shape) {
    const auto size = detail::resolveParameter(shape.size, *plan, resolved);
    const auto fill = detail::resolveParameter(shape.fillColor, *plan, resolved);
    const auto stroke = detail::resolveParameter(shape.strokeColor, *plan, resolved);
    const auto width = detail::resolveParameter(shape.strokeWidth, *plan, resolved);
    if (!size || !fill || !stroke || !width || !std::isfinite(size->value.x) ||
        !std::isfinite(size->value.y) || size->value.x < 0 || size->value.y < 0 ||
        !std::isfinite(width->value) || width->value < 0) {
        operationFailure = diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                      "Shape parameters are invalid", {}, operationSubject);
        return;
    }
    const bool fillEnabled = shape.fillEnabled && shape.kind != document::ShapeKind::Line;
    const bool strokeEnabled = shape.strokeEnabled && width->value > 0;
    if (!fillEnabled && !strokeEnabled)
        return;
    const auto fillPixel = render::solidPixelFromStraightLinearRec709Scene(fill->value);
    const auto strokePixel = render::solidPixelFromStraightLinearRec709Scene(stroke->value);
    if (!fillPixel || !strokePixel) {
        operationFailure = diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                                      "Shape color is invalid", {}, operationSubject);
        return;
    }
    const auto path = shapePath(shape, size->value);
    const auto cancelled = [&] { return cancellation.isCancellationRequested(); };
    const auto raster = render::PathRaster::create(
        path,
        {strokeEnabled ? width->value : 0, static_cast<render::PathStrokeAlign>(shape.strokeAlign),
         static_cast<render::PathStrokeJoin>(shape.strokeJoin),
         static_cast<render::PathStrokeCap>(shape.strokeCap)},
        resolved.horizontalScale, resolved.verticalScale, cancelled);
    if (!raster) {
        if (cancelled())
            operationCancelled = true;
        else
            operationFailure = imageDiagnostic(*raster.error(), operationSubject,
                                               "Shape geometry could not be rasterized");
        return;
    }
    const auto extent = raster.value()->bounds(fillEnabled, strokeEnabled);
    bounds[index].local = {extent.left, extent.top, extent.right, extent.bottom};
    bounds[index].output = bounds[index].local;
    if (bounds[index].local.empty())
        return;
    const auto left = std::floor(extent.left * resolved.horizontalScale);
    const auto top = std::floor(extent.top * resolved.verticalScale);
    const auto right = std::ceil(extent.right * resolved.horizontalScale);
    const auto bottom = std::ceil(extent.bottom * resolved.verticalScale);
    constexpr double maxCoordinate = 9007199254740991.0;
    const auto finiteCoordinate = [](double value) {
        return std::isfinite(value) && std::abs(value) <= maxCoordinate;
    };
    if (!finiteCoordinate(left) || !finiteCoordinate(top) || !finiteCoordinate(right) ||
        !finiteCoordinate(bottom) || right - left > std::numeric_limits<std::uint32_t>::max() ||
        bottom - top > std::numeric_limits<std::uint32_t>::max()) {
        operationFailure =
            diagnostic(EvaluationDiagnosticCode::InvalidParameter,
                       "Shape bounds exceed the supported coordinate range", {}, operationSubject);
        return;
    }
    const auto window = render::ImageWindow::create(
        static_cast<std::int64_t>(left), static_cast<std::int64_t>(top),
        static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top));
    if (!window) {
        operationFailure =
            imageDiagnostic(*window.error(), operationSubject, "Shape bounds are invalid");
        return;
    }
    const auto clipped = clipWindow(*window.value(), index);
    if (!clipped)
        return;
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *clipped, resolved.imageDescriptor.displayWindow(),
        resolved.imageDescriptor.pixelAspect());
    if (!descriptor) {
        operationFailure =
            imageDiagnostic(*descriptor.error(), operationSubject, "Shape image is invalid");
        return;
    }
    const auto pixelCount = static_cast<std::uint64_t>(clipped->extent().width()) *
                            clipped->extent().height();
    // Reserve one coverage byte and one stroke pixel per output pixel: even if every row runs
    // concurrently, its temporary storage and the process image stay inside the request budget.
    if (pixelCount > remainingPixelBudget() / (2 * sizeof(render::Rgba32f) + 1)) {
        operationFailure = imageDiagnostic(
            render::ImageError::codeOnly(render::ImageErrorCode::PixelStorageBudgetExceeded),
            operationSubject, "Shape exceeds the pixel storage budget");
        return;
    }
    const auto scratchBytes = static_cast<std::size_t>(pixelCount) * (sizeof(render::Rgba32f) + 1);
    auto builder = render::Rgba32fImageBuilder::create(
        *descriptor.value(), remainingPixelBudget() - scratchBytes, render::Rgba32f::transparent());
    if (!builder) {
        operationFailure = imageDiagnostic(*builder.error(), operationSubject,
                                           "Shape image could not be allocated");
        return;
    }
    auto& image = *builder.value();
    const auto height = clipped->extent().height();
    reportRowPassStarted(progress, operationIndex, height);
    const auto outcome = runRowBandPass(
        rowBands, cancellation, height, clipped->originY(),
        [&](std::int64_t y) -> RowFailure {
            auto output = image.row(y);
            if (!output)
                return imageDiagnostic(*output.error(), operationSubject, "Shape row is invalid");
            auto pixels = *output.value();
            std::vector<std::uint8_t> coverage(pixels.size());
            const auto rule = static_cast<render::PathFillRule>(shape.fillRule);
            if (fillEnabled) {
                if (!raster.value()->coverageRow(clipped->originX(), y, coverage, rule,
                                                 false, cancelled))
                    return std::nullopt;
                if (const auto error =
                        render::coverageSolidRow(coverage, *fillPixel.value(), pixels))
                    return imageDiagnostic(*error, operationSubject, "Shape fill failed");
            }
            if (strokeEnabled) {
                if (!raster.value()->coverageRow(clipped->originX(), y, coverage, rule, true,
                                                 cancelled))
                    return std::nullopt;
                std::vector<render::Rgba32f> strokePixels(pixels.size(),
                                                          render::Rgba32f::transparent());
                if (const auto error =
                        render::coverageSolidRow(coverage, *strokePixel.value(), strokePixels))
                    return imageDiagnostic(*error, operationSubject, "Shape stroke failed");
                if (const auto error = render::sourceOverLinearRec709SceneRow(strokePixels, pixels))
                    return imageDiagnostic(*error, operationSubject,
                                           "Shape stroke compositing failed");
            }
            return std::nullopt;
        });
    if (outcome.cancelled || cancelled()) {
        operationCancelled = true;
        return;
    }
    if (outcome.failure || outcome.incomplete) {
        operationFailure = rowPassFailure(outcome, operationSubject);
        return;
    }
    reportRowPassFinished(progress, operationIndex, height);
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        operationFailure = imageDiagnostic(*frozen.error(), operationSubject,
                                           "Shape image could not be published");
        return;
    }
    produced.emplace(std::move(*frozen.value()));
},
