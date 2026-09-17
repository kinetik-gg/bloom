void testNativeShapeAndTextResize() {
    const auto format = requiredValue(document::CompositionFormat::create(400, 400));
    auto project = document::makeNewProject("Native", "Main", time(10), format);
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto mapping = makeMapping(QRectF(0, 0, 400, 400), format);
    require(session.addShapeLayer(document::ShapeKind::Rectangle,
                                  {.position = document::Vec2d{200, 200},
                                   .size = document::Vec2d{100, 100},
                                   .lineStart = {},
                                   .lineEnd = {},
                                   .path = {}}),
            "add native square");
    const auto layer = std::get<document::LayerId>(session.selection().primary);
    const auto original = evaluatedBounds(session, layer);
    int refreshes = 0;
    QObject::connect(&session, &ui::CompositionSession::liveValueChanged, &session,
                     [&] { ++refreshes; });
    for (int handle = 0; handle < 8; ++handle) {
        const auto origin =
            ui::viewerHandlePoints(mapping, original)[static_cast<std::size_t>(handle)];
        require(!session.beginTransformInteraction(
                    {ui::TransformGesture::Kind::Scale, handle, origin, original}, mapping),
                "begin native handle");
        const QPointF delta(handle == 0 || handle == 6 || handle == 7 ? -20 : 20,
                            handle == 0 || handle == 1 || handle == 2 ? -20 : 20);
        session.updateTransformInteraction(origin + delta);
        requireNear(requiredValue(session.effectiveVec2Value("size")),
                    {handle == 1 || handle == 5 ? 100.0 : 120.0,
                     handle == 3 || handle == 7 ? 100.0 : 120.0},
                    "Properties and timeline native-size readbacks consume live overrides");
        requireNear(requiredValue(session.effectiveVec2Value(document::kScaleParameterRole)),
                    {1, 1}, "live Scale readback stays at 100 percent during native resize");
        const auto overrides = session.transformInteractionOverrides();
        const auto* size = std::get_if<document::Vec2d>(&overrides.front().value);
        require(size != nullptr, "native size override exists");
        requireNear(*size,
                    {handle == 1 || handle == 5 ? 100.0 : 120.0,
                     handle == 3 || handle == 7 ? 100.0 : 120.0},
                    "edges resize one dimension and corners resize both");
        const auto* scale = session.parameterForSelection(document::kScaleParameterRole);
        require(std::ranges::none_of(
                    overrides, [&](const auto& item) { return item.parameterId == scale->id; }),
                "no native handle emits a transform Scale override");
        const auto changed = evaluatedBounds(session, layer);
        const auto fixed = static_cast<std::size_t>((handle + 4) % 8);
        requireNear(mapping.toComposition(ui::viewerHandlePoints(mapping, changed)[fixed]),
                    mapping.toComposition(ui::viewerHandlePoints(mapping, original)[fixed]),
                    "opposite native handle stays pinned");
        require(session.commitTransformInteraction(), "commit native dimensions and placement");
        requireNear(requiredValue(session.effectiveVec2Value(document::kScaleParameterRole)),
                    {1, 1}, "100 percent Scale survives every native resize");
        require(session.undo(), "native resize is one undo transaction");
        requireNear(requiredValue(session.effectiveVec2Value("size")), {100, 100},
                    "undo restores native size");
    }
    const auto topHandle = ui::viewerHandlePoints(mapping, original)[1];
    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Scale, 1, topHandle, original}, mapping),
            "begin aspect-preserving edge resize");
    session.updateTransformInteraction(topHandle + QPointF(0, -20), {.shift = true});
    const auto uniform = evaluatedBounds(session, layer);
    requireNear(mapping.toComposition(ui::viewerHandlePoints(mapping, uniform)[5]),
                mapping.toComposition(ui::viewerHandlePoints(mapping, original)[5]),
                "Shift edge resize keeps the opposite midpoint fixed on both axes");
    requireNear(requiredValue(session.effectiveVec2Value("size")), {120, 120},
                "Shift preserves native aspect");
    session.cancelTransformInteraction();
    requireNear(requiredValue(session.effectiveVec2Value("size")), {100, 100},
                "cancel restores native readback");
    require(session.addShapeLayer(document::ShapeKind::Line, {.position = document::Vec2d{200, 200},
                                                              .size = {},
                                                              .lineStart = document::Vec2d{0, 0},
                                                              .lineEnd = document::Vec2d{100, 0},
                                                              .path = {}}),
            "add native line");
    const auto line = std::get<document::LayerId>(session.selection().primary);
    const auto lineBounds = evaluatedBounds(session, line);
    const auto lineCorner = ui::viewerHandlePoints(mapping, lineBounds)[4];
    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Scale, 4, lineCorner, lineBounds}, mapping),
            "begin native line resize");
    session.updateTransformInteraction(lineCorner + QPointF(20, 20));
    require(session.commitTransformInteraction(), "commit native line endpoints");
    requireNear(requiredValue(session.effectiveVec2Value("lineStart")), {0, 0},
                "line keeps native start");
    requireNear(requiredValue(session.effectiveVec2Value("lineEnd")), {120, 20},
                "line handles edit endpoints even from a zero-height line");
    requireNear(requiredValue(session.effectiveVec2Value(document::kScaleParameterRole)), {1, 1},
                "line handles preserve Scale");
    require(refreshes >= 16, "native gestures notify live property and timeline readers");
    session.selectLayer(layer);
    const auto* scaleField = session.parameterForSelection(document::kScaleParameterRole);
    require(scaleField && session.beginValueEdit(scaleField->id),
            "begin transform Scale field edit");
    require(session.updateValueEdit(document::Vec2d{2, 3}), "preview Scale field edit");
    requireNear(requiredValue(session.effectiveVec2Value(document::kScaleParameterRole)), {2, 3},
                "Scale field readbacks are live");
    requireNear(requiredValue(session.effectiveVec2Value("size")), {100, 100},
                "Scale field preserves native size readback");
    session.cancelValueEdit();
    requireNear(requiredValue(session.effectiveVec2Value(document::kScaleParameterRole)), {1, 1},
                "cancel restores Scale readback");
    require(session.addTextLayer(QStringLiteral("Text"), QStringLiteral("Hello"), 20),
            "add point text");
    const auto text = std::get<document::LayerId>(session.selection().primary);
    auto bounds = evaluatedBounds(session, text);
    auto handles = ui::viewerHandlePoints(mapping, bounds);
    require(session
                .beginTransformInteraction(
                    {ui::TransformGesture::Kind::Scale, 1, handles[1], bounds}, mapping)
                .has_value(),
            "point text refuses edge resize");
    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Scale, 4, handles[4], bounds}, mapping),
            "point text accepts corner resize");
    session.updateTransformInteraction(handles[4] + QPointF(20, 10));
    require(session.commitTransformInteraction(), "commit point font resize");
    require(requiredValue(session.effectiveScalarValue("size")) > 20,
            "corner increases native font size");
    requireNear(requiredValue(session.effectiveVec2Value(document::kScaleParameterRole)), {1, 1},
                "point text Scale is unchanged");
    require(session.undo(), "undo font resize");
    const auto* box = session.parameterForSelection(document::kTextBoxParameterRole);
    require(box && session.setParameterValue(box->id, document::Vec2d{100, 80},
                                             QStringLiteral("Set text box")),
            "set text box");
    bounds = evaluatedBounds(session, text);
    handles = ui::viewerHandlePoints(mapping, bounds);
    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Scale, 3, handles[3], bounds}, mapping),
            "box text accepts edge resize");
    session.updateTransformInteraction(handles[3] + QPointF(20, 0));
    require(session.commitTransformInteraction(), "commit text box resize");
    requireNear(requiredValue(session.effectiveVec2Value("box")), {120, 80},
                "box edge changes width only");
    require(requiredValue(session.effectiveScalarValue("size")) == 20,
            "box resize preserves font size");
    requireNear(evaluatedBounds(session, text).polygon[0], bounds.polygon[0],
                "text box opposite corner is pinned");
}
