// Included in the Layer Output vector arm. Raster operations terminate VectorChain propagation.
std::vector<render::Path> paths;
render::PathStroke stroke;
std::optional<render::PathBounds> clip;
const auto pixel = [&](core::Color4d color) {
    const auto result = render::solidPixelFromStraightLinearRec709Scene(color);
    if (!result) {
        operationFailure =
            imageDiagnostic(*result.error(), operationSubject, "Vector color is invalid");
        return render::Rgba32f::transparent();
    }
    return *result.value();
};
const auto& sourceOperation = plan->operations()[chain.source];
if (const auto* shape = std::get_if<CompiledShape>(&sourceOperation)) {
    const auto size = detail::resolveParameter(shape->size, *plan, resolved);
    const auto fill = detail::resolveParameter(shape->fillColor, *plan, resolved);
    const auto color = detail::resolveParameter(shape->strokeColor, *plan, resolved);
    const auto width = detail::resolveParameter(shape->strokeWidth, *plan, resolved);
    if (!size || !fill || !color || !width)
        return;
    paths.push_back(shapePath(*shape, size->value));
    vectorHasFill = shape->fillEnabled && shape->kind != document::ShapeKind::Line;
    vectorHasStroke = shape->strokeEnabled && width->value > 0;
    stroke = {vectorHasStroke ? width->value : 0,
              static_cast<render::PathStrokeAlign>(shape->strokeAlign),
              static_cast<render::PathStrokeJoin>(shape->strokeJoin),
              static_cast<render::PathStrokeCap>(shape->strokeCap)};
    vectorRule = static_cast<render::PathFillRule>(shape->fillRule);
    vectorFill = pixel(fill->value);
    vectorStroke = pixel(color->value);
} else if (const auto* solid = std::get_if<CompiledSolid>(&sourceOperation)) {
    const auto width = detail::resolveParameter(solid->width, *plan, resolved);
    const auto height = detail::resolveParameter(solid->height, *plan, resolved);
    const auto color = detail::resolveParameter(solid->color, *plan, resolved);
    if (!width || !height || !color)
        return;
    paths.push_back(render::rectanglePath(width->value, height->value));
    vectorHasFill = true;
    vectorFill = pixel(color->value);
} else if (const auto* text = std::get_if<CompiledText>(&sourceOperation)) {
    const auto size = detail::resolveParameter(text->size, *plan, resolved);
    const auto color = detail::resolveParameter(text->color, *plan, resolved);
    const auto content = detail::resolveParameter(text->contentParameterId, text->content,
                                                  text->drivenContent, resolved);
    const auto lineHeight = detail::resolveParameter(text->layout.lineHeight, *plan, resolved);
    const auto spacing = detail::resolveParameter(text->layout.letterSpacing, *plan, resolved);
    const auto alignment = detail::resolveParameter(
        text->layout.alignmentId, text->layout.alignment, text->layout.drivenAlignment, resolved);
    if (!size || !color || !content || !lineHeight || !spacing || !alignment)
        return;
    const auto parameters = render::TextRasterParameters::create(size->value, size->value);
    if (!parameters)
        return;
    render::TextLayoutOptions layout;
    layout.alignment = static_cast<render::TextAlignment>(alignment->value);
    layout.lineHeight = lineHeight->value;
    layout.letterSpacing = spacing->value;
    layout.boxWidth = text->layout.box.x;
    layout.boxHeight = text->layout.box.y;
    layout.wrap = text->layout.wrap;
    layout.verticalAlignment =
        static_cast<render::TextLayoutOptions::VerticalAlignment>(text->layout.verticalAlignment);
    layout.anchorMode = static_cast<render::TextLayoutOptions::AnchorMode>(text->layout.anchorMode);
    layout.overflow = static_cast<render::TextLayoutOptions::Overflow>(text->layout.overflow);
    auto outlines = render::textOutlines(text->font, content->value, *parameters.value(), layout,
                                         [&] { return cancellation.isCancellationRequested(); });
    if (!outlines) {
        operationCancelled = cancellation.isCancellationRequested();
        if (!operationCancelled)
            operationFailure = imageDiagnostic(*outlines.error(), operationSubject,
                                               "Text outlines are unavailable");
        return;
    }
    paths = std::move(*outlines.value());
    if (layout.boxWidth > 0 && layout.overflow == render::TextLayoutOptions::Overflow::Clip)
        clip = render::PathBounds{0, 0, layout.boxWidth, layout.boxHeight};
    vectorHasFill = true;
    vectorFill = pixel(color->value);
}
auto raster = render::PathRaster::transformed(
    paths, stroke, {m.a, m.b, m.c, m.d, m.x, m.y}, resolved.horizontalScale, resolved.verticalScale,
    [&] { return cancellation.isCancellationRequested(); }, clip);
if (!raster) {
    operationCancelled = cancellation.isCancellationRequested();
    if (!operationCancelled)
        operationFailure =
            imageDiagnostic(*raster.error(), operationSubject, "Vector geometry is invalid");
    return;
}
vectorRaster = std::move(*raster.value());
