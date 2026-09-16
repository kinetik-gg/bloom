// Recorded pointer gestures use the real mapping and preview worker, not private tool state.
void toolKey(ViewerFixture& fixture, int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &event);
}
QPointF toolScreen(const ViewerFixture& fixture, QPointF point) {
    const auto* composition = fixture.session.composition();
    const auto extent = bloom::render::ImageExtent::create(composition->format().width(),
                                                           composition->format().height());
    const auto rect = bloom::ui::viewTransformedDisplayRect(
        fixture.viewer.canvasRectForTest(), *extent.value(), composition->format().pixelAspect(),
        fixture.viewer.viewTransformForTest());
    return rect.topLeft() + QPointF(point.x() * rect.width() / composition->format().width(),
                                    point.y() * rect.height() / composition->format().height());
}
void toolMouse(ViewerFixture& fixture, QEvent::Type type, QPointF point,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto screen = toolScreen(fixture, point);
    QMouseEvent event(
        type, screen, screen, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(&fixture.viewer, &event);
}
void toolDrag(ViewerFixture& fixture, QPointF start, QPointF end,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    toolMouse(fixture, QEvent::MouseButtonPress, start, modifiers);
    toolMouse(fixture, QEvent::MouseMove, end, modifiers);
    toolMouse(fixture, QEvent::MouseButtonRelease, end, modifiers);
}
void toolReady(ViewerFixture& fixture, Expectations& expectations) {
    expectations.expect(waitUntil([&] {
                            const auto& frame = fixture.controller.state().frame;
                            return frame &&
                                   frame->desiredIdentity().sourceRevision ==
                                       fixture.session.snapshot().revision() &&
                                   frame->desiredIdentity().time == fixture.session.currentTime() &&
                                   isReady(fixture.controller);
                        }),
                        "creation preview reaches current revision and time");
}
bloom::document::ParameterValue toolParameter(ViewerFixture& fixture, std::string_view role) {
    const auto* parameter = fixture.session.parameterForSelection(role);
    const auto value = parameter ? fixture.session.liveValue(parameter->id) : std::nullopt;
    if (!value) {
        std::cerr << "Missing tool parameter: " << role << '\n';
        std::abort();
    }
    return *value;
}
bool toolVector(ViewerFixture& fixture, std::string_view role, double x, double y) {
    const auto value = toolParameter(fixture, role);
    const auto* vector = std::get_if<bloom::document::Vec2d>(&value);
    return vector && near(vector->x, x) && near(vector->y, y);
}
bloom::document::NewProject toolProject() {
    const auto format = bloom::document::CompositionFormat::create(400, 300);
    if (!format)
        std::abort();
    return bloom::document::makeNewProject("Creation", "Main",
                                           bloom::core::RationalTime::fromInteger(10), *format);
}
void toolShutdown(ViewerFixture& fixture, Expectations& expectations) {
    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }),
                        "creation worker shuts down");
}
void testCreationTools(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(800, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    const auto initial = fixture.commands.size();
    toolKey(fixture, Qt::Key_R);
    toolMouse(fixture, QEvent::MouseButtonPress, {10, 20});
    toolMouse(fixture, QEvent::MouseMove, {110, 70});
    expectations.expect(fixture.commands.size() == initial,
                        "primitive outline creates no history before release");
    toolMouse(fixture, QEvent::MouseButtonRelease, {110, 70});
    expectations.expect(fixture.commands.size() == initial + 1,
                        "rectangle creation is one transaction");
    expectations.expect(
        toolVector(fixture, "size", 100, 50) && toolVector(fixture, "position", 60, 45),
        "100 by 50 rectangle starts at composition (10,20), with centre-based position");
    toolReady(fixture, expectations);
    expectations.expect(!fixture.controller.selectedLayerBounds().empty(),
                        "new shape selected with gizmo geometry");
    toolKey(fixture, Qt::Key_Escape);
    expectations.expect(fixture.viewer.findChild<QToolButton*>("viewerSelectTool")->isChecked(),
                        "Escape returns to Select");
    toolKey(fixture, Qt::Key_E);
    toolDrag(fixture, {20, 30}, {90, 70}, Qt::ShiftModifier);
    expectations.expect(toolVector(fixture, "size", 70, 70), "Shift ellipse makes a circle");
    toolReady(fixture, expectations);
    fixture.viewer.findChild<QToolButton*>("viewerStarTool")->click();
    toolDrag(fixture, {160, 120}, {210, 140}, Qt::ShiftModifier | Qt::AltModifier);
    expectations.expect(toolVector(fixture, "size", 100,
                                   100 * (1 + std::cos(std::numbers::pi / 5)) /
                                       (2 * std::cos(std::numbers::pi / 10))) &&
                            toolVector(fixture, "position", 160, 120) &&
                            std::get<std::int64_t>(toolParameter(fixture, "points")) == 5,
                        "star defaults to five points and supports centred regular drag");
    toolReady(fixture, expectations);
    fixture.viewer.findChild<QToolButton*>("viewerPolygonTool")->click();
    toolDrag(fixture, {80, 80}, {150, 130});
    expectations.expect(std::get<std::int64_t>(toolParameter(fixture, "kind")) ==
                                static_cast<std::int64_t>(document::ShapeKind::Polygon) &&
                            std::get<std::int64_t>(toolParameter(fixture, "points")) == 5,
                        "polygon creates five sides");
    toolReady(fixture, expectations);
    fixture.viewer.findChild<QToolButton*>("viewerLineTool")->click();
    toolDrag(fixture, {40, 40}, {110, 90}, Qt::ShiftModifier);
    const auto end = std::get<document::Vec2d>(toolParameter(fixture, "lineEnd"));
    expectations.expect(toolVector(fixture, "lineStart", 40, 40) && near(end.x - 40, end.y - 40),
                        "Shift line snaps to 45 degrees");
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_R);
    toolDrag(fixture, {160, 120}, {160, 120});
    expectations.expect(toolVector(fixture, "size", 100, 100) &&
                            toolVector(fixture, "position", 160, 120),
                        "click creates default size centred on click");
    toolReady(fixture, expectations);
    const auto beforeCancel = fixture.commands.size();
    toolMouse(fixture, QEvent::MouseButtonPress, {20, 30});
    toolMouse(fixture, QEvent::MouseMove, {100, 100});
    toolKey(fixture, Qt::Key_Escape);
    toolMouse(fixture, QEvent::MouseButtonRelease, {100, 100});
    expectations.expect(fixture.commands.size() == beforeCancel,
                        "Escape cancels creation without history");
    toolKey(fixture, Qt::Key_R);
    toolMouse(fixture, QEvent::MouseButtonPress, {20, 30});
    fixture.viewer.resize(810, 650);
    toolMouse(fixture, QEvent::MouseButtonRelease, {100, 100});
    expectations.expect(fixture.commands.size() == beforeCancel,
                        "resize cancels frozen creation mapping");
    toolShutdown(fixture, expectations);
}
void testPenTools(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(800, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_P);
    const auto before = fixture.commands.size();
    toolDrag(fixture, {80, 80}, {80, 80});
    toolDrag(fixture, {220, 80}, {220, 80});
    toolDrag(fixture, {150, 220}, {150, 220});
    expectations.expect(fixture.commands.size() == before,
                        "pen anchors are session-only before finish");
    toolDrag(fixture, {80, 80}, {80, 80});
    const auto path = std::get<document::PathValue>(toolParameter(fixture, "path"));
    expectations.expect(path.closed && path.anchors.size() == 3 &&
                            fixture.commands.size() == before + 1,
                        "closing pen triangle commits three anchors in one transaction");
    toolReady(fixture, expectations);
    const auto plan = fixture.pipeline.compiler.compile(
        {fixture.document.snapshot(), fixture.session.compositionId()}, {});
    expectations.expect(plan.plan != nullptr, "created triangle compiles");
    if (plan.plan) {
        const auto result =
            fixture.pipeline.evaluator.evaluate(plan.plan,
                                                {{},
                                                 plan.plan->output(),
                                                 runtime::CompositionFormatResolution{},
                                                 runtime::EvaluationQuality::Reference,
                                                 runtime::EvaluationColorIntent::LinearRec709Scene,
                                                 32U << 20U},
                                                {});
        expectations.expect(result.frame() != nullptr, "created triangle rasterises");
        if (result.frame()) {
            const auto center = result.frame()->processImage().read(150, 140);
            const auto outside = result.frame()->processImage().read(10, 10);
            expectations.expect(center && outside && center.value()->alpha() > 0 &&
                                    outside.value()->alpha() == 0,
                                "pen triangle coverage is positive at centre and zero outside");
        }
    }
    toolKey(fixture, Qt::Key_V);
    const auto dragBefore = fixture.commands.size();
    toolMouse(fixture, QEvent::MouseButtonPress, {80, 80});
    toolMouse(fixture, QEvent::MouseMove, {90, 90});
    expectations.expect(fixture.commands.size() == dragBefore && fixture.session.valueEditActive(),
                        "anchor drag publishes without history");
    const auto overrides = fixture.session.valueEditOverrides();
    expectations.expect(!overrides.empty() &&
                            std::holds_alternative<document::PathValue>(overrides.front().value),
                        "anchor drag uses typed Path override");
    if (!overrides.empty())
        expectations.expect(fixture.pipeline.compiler
                                    .compile({fixture.document.snapshot(),
                                              fixture.session.compositionId(), overrides},
                                             {})
                                    .plan != nullptr,
                            "live Path override compiles");
    toolMouse(fixture, QEvent::MouseButtonRelease, {90, 90});
    expectations.expect(fixture.commands.size() == dragBefore + 1,
                        "anchor drag commits exactly one transaction");
    const auto moved = std::get<document::PathValue>(toolParameter(fixture, "path"));
    expectations.expect(near(moved.anchors.front().point.x, 90) &&
                            near(moved.anchors.front().point.y, 90),
                        "anchor moves through inverse mapping");
    toolReady(fixture, expectations);
    const auto bounds = fixture.controller.selectedLayerBounds();
    if (!bounds.empty()) {
        const auto& b = bounds.front();
        const auto p = moved.anchors[1].point;
        const double x = (p.x - b.local.left) / (b.local.right - b.local.left);
        const double y = (p.y - b.local.top) / (b.local.bottom - b.local.top);
        expectations.expect(near(b.polygon[0].x + x * (b.polygon[1].x - b.polygon[0].x) +
                                     y * (b.polygon[3].x - b.polygon[0].x),
                                 220) &&
                                near(b.polygon[0].y + x * (b.polygon[1].y - b.polygon[0].y) +
                                         y * (b.polygon[3].y - b.polygon[0].y),
                                     80),
                            "moving an outer anchor preserves other anchors in composition space");
    }
    expectations.expect(fixture.session.undo(), "anchor drag undo succeeds");
    expectations.expect(std::get<document::PathValue>(toolParameter(fixture, "path")) == path,
                        "one undo restores complete path");
    toolReady(fixture, expectations);
    toolMouse(fixture, QEvent::MouseButtonPress, {80, 80});
    toolMouse(fixture, QEvent::MouseMove, {95, 95});
    toolKey(fixture, Qt::Key_Escape);
    expectations.expect(std::get<document::PathValue>(toolParameter(fixture, "path")) == path &&
                            !fixture.session.valueEditActive(),
                        "Escape cancels path override");
    toolDrag(fixture, {80, 80}, {80, 80});
    toolKey(fixture, Qt::Key_Delete);
    expectations.expect(
        std::get<document::PathValue>(toolParameter(fixture, "path")).anchors.size() == 2,
        "Delete removes selected anchor instead of layer");
    toolReady(fixture, expectations);
    fixture.session.clearSelection();
    toolKey(fixture, Qt::Key_P);
    toolDrag(fixture, {40, 160}, {70, 160});
    toolDrag(fixture, {180, 160}, {180, 160});
    toolKey(fixture, Qt::Key_Return);
    auto curve = std::get<document::PathValue>(toolParameter(fixture, "path"));
    const auto inHandle = curve.anchors.front().inHandle;
    const auto outHandle = curve.anchors.front().outHandle;
    expectations.expect(!curve.closed && curve.anchors.size() == 2 && inHandle && outHandle &&
                            near(inHandle->x, 10) && near(inHandle->y, 160) &&
                            near(outHandle->x, 70) && near(outHandle->y, 160),
                        "dragged pen anchor has symmetric handles and Enter finishes open path");
    toolReady(fixture, expectations);
    // This horizontal open path has symmetric stroke bounds, so local and composition coordinates
    // match.
    toolKey(fixture, Qt::Key_V);
    toolDrag(fixture, {70, 160}, {70, 190}, Qt::AltModifier);
    const auto broken = std::get<document::PathValue>(toolParameter(fixture, "path"));
    expectations.expect(broken.anchors.front().inHandle == curve.anchors.front().inHandle &&
                            broken.anchors.front().outHandle != curve.anchors.front().outHandle,
                        "Alt handle drag leaves opposite handle unchanged");
    toolReady(fixture, expectations);
    fixture.session.clearSelection();
    toolKey(fixture, Qt::Key_P);
    const auto cancelRevision = fixture.session.snapshot().revision();
    toolDrag(fixture, {40, 40}, {60, 60});
    toolKey(fixture, Qt::Key_Escape);
    expectations.expect(fixture.session.snapshot().revision() == cancelRevision &&
                            fixture.viewer.findChild<QToolButton*>("viewerSelectTool")->isChecked(),
                        "Escape discards an unfinished pen draft without a transaction");
    toolShutdown(fixture, expectations);
}
void testTextTool(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    QWidget host;
    QVBoxLayout layout(&host);
    ui::PropertiesEditor properties(fixture.session);
    layout.addWidget(&fixture.viewer);
    layout.addWidget(&properties);
    host.resize(900, 1000);
    host.show();
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_T);
    const auto before = fixture.commands.size();
    toolMouse(fixture, QEvent::MouseButtonPress, {110, 120});
    expectations.expect(fixture.commands.size() == before + 1 &&
                            toolVector(fixture, "position", 110, 120),
                        "Text tool commits layer and click position together");
    auto* field = properties.findChild<QLineEdit*>("textContentEditor");
    expectations.expect(waitUntil([&] { return field && field->hasFocus(); }),
                        "Text tool focuses Properties text field");
    expectations.expect(field && field->isVisible(), "Text field is visible after focus");
    toolShutdown(fixture, expectations);
    fixture.viewer.setParent(nullptr);
    properties.setParent(nullptr);
}
