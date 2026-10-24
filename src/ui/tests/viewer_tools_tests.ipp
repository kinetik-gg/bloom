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
    if (type == QEvent::MouseButtonPress)
        (void)fixture.viewer.grab();
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
bool selectToolActive(ViewerFixture& fixture) {
    return fixture.viewer.findChild<QToolButton*>("viewerSelectTool")->isChecked();
}
std::size_t layerCount(ViewerFixture& fixture) {
    return fixture.session.composition()->graph().layerOutputs().size();
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
    expectations.expect(selectToolActive(fixture),
                        "releasing a rectangle returns the Viewer to Select");
    // The next click outside the new shape deselects; it never draws another object.
    {
        const auto layers = layerCount(fixture);
        const auto history = fixture.commands.size();
        toolDrag(fixture, {300, 250}, {300, 250});
        expectations.expect(layerCount(fixture) == layers && fixture.commands.size() == history &&
                                fixture.session.selectedNodes().empty(),
                            "a click outside after creation deselects and creates nothing");
    }
    // ADR 0028: with a layer selected the primitives draw masks; these cases create shape layers.
    fixture.session.clearSelection();
    toolKey(fixture, Qt::Key_E);
    toolDrag(fixture, {20, 30}, {90, 70}, Qt::ShiftModifier);
    expectations.expect(toolVector(fixture, "size", 70, 70), "Shift ellipse makes a circle");
    expectations.expect(selectToolActive(fixture),
                        "releasing an ellipse returns the Viewer to Select");
    {
        fixture.session.clearSelection();
        const auto layers = layerCount(fixture);
        toolDrag(fixture, {380, 290}, {380, 290});
        expectations.expect(layerCount(fixture) == layers,
                            "a click after the ellipse creates no second ellipse");
    }
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
    fixture.session.clearSelection();
    toolKey(fixture, Qt::Key_R);
    {
        const auto layers = layerCount(fixture);
        const auto history = fixture.commands.size();
        toolDrag(fixture, {160, 120}, {160, 120});
        expectations.expect(layerCount(fixture) == layers && fixture.commands.size() == history &&
                                !selectToolActive(fixture),
                            "a zero-size click creates nothing and keeps the Rectangle tool");
        toolMouse(fixture, QEvent::MouseButtonPress, {200, 200});
        toolKey(fixture, Qt::Key_Escape);
        toolMouse(fixture, QEvent::MouseButtonRelease, {200, 200});
        expectations.expect(layerCount(fixture) == layers && fixture.commands.size() == history &&
                                selectToolActive(fixture),
                            "Escape on a zero-size press creates nothing and returns to Select");
    }
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_R);
    const auto beforeEscape = fixture.commands.size();
    toolMouse(fixture, QEvent::MouseButtonPress, {20, 30});
    toolMouse(fixture, QEvent::MouseMove, {100, 100});
    toolKey(fixture, Qt::Key_Escape);
    toolMouse(fixture, QEvent::MouseButtonRelease, {100, 100});
    expectations.expect(fixture.commands.size() == beforeEscape + 1 &&
                            toolVector(fixture, "size", 80, 70) && selectToolActive(fixture),
                        "Escape mid-drag commits the shape at its current size and returns to "
                        "Select");
    toolReady(fixture, expectations);
    fixture.session.clearSelection();
    const auto beforeCancel = fixture.commands.size();
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
    expectations.expect(selectToolActive(fixture), "closing a Pen path returns to Select");
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
    {
        const auto undoLabel = fixture.session.undoLabel();
        toolMouse(fixture, QEvent::MouseButtonPress, {80, 80});
        toolMouse(fixture, QEvent::MouseMove, {95, 95});
        toolKey(fixture, Qt::Key_Escape);
        toolMouse(fixture, QEvent::MouseButtonRelease, {95, 95});
        const auto kept = std::get<document::PathValue>(toolParameter(fixture, "path"));
        expectations.expect(
            near(kept.anchors.front().point.x, 95) && near(kept.anchors.front().point.y, 95) &&
                !fixture.session.valueEditActive() && fixture.session.undoLabel() != undoLabel,
            "Escape keeps an anchor drag as a command");
        expectations.expect(fixture.session.undo() && std::get<document::PathValue>(
                                                          toolParameter(fixture, "path")) == path,
                            "exactly one undo discards the kept anchor drag");
        toolReady(fixture, expectations);
    }
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
    expectations.expect(selectToolActive(fixture), "Enter finishing a Pen path returns to Select");
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
                            selectToolActive(fixture),
                        "Escape on a one-anchor pen draft creates nothing and returns to Select");
    // Escape confirms: three placed anchors finish as one open path.
    toolReady(fixture, expectations);
    fixture.session.clearSelection();
    toolKey(fixture, Qt::Key_P);
    {
        const auto history = fixture.commands.size();
        toolDrag(fixture, {40, 40}, {40, 40});
        toolDrag(fixture, {140, 40}, {140, 40});
        toolDrag(fixture, {90, 120}, {90, 120});
        toolKey(fixture, Qt::Key_Escape);
        const auto kept = std::get<document::PathValue>(toolParameter(fixture, "path"));
        expectations.expect(!kept.closed && kept.anchors.size() == 3 &&
                                fixture.commands.size() == history + 1 && selectToolActive(fixture),
                            "Escape finishes a three-anchor Pen path open, in one command");
        const auto layers = layerCount(fixture);
        fixture.session.clearSelection();
        toolDrag(fixture, {300, 250}, {300, 250});
        expectations.expect(layerCount(fixture) == layers,
                            "a click after the Pen path creates nothing");
    }
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
    expectations.expect(
        waitUntil([&] { return fixture.viewer.textEditing() && fixture.viewer.hasFocus(); }),
        "Text tool enters canvas edit mode");
    expectations.expect(field && field->isVisible(), "Text field is visible after focus");
    expectations.expect(selectToolActive(fixture), "placing text returns the Viewer to Select");
    // Escape commits what was typed, exactly like click-away, and the new layer stays one step.
    {
        QKeyEvent typed(QEvent::KeyPress, 0, Qt::NoModifier, QStringLiteral("Hello"));
        QCoreApplication::sendEvent(&fixture.viewer, &typed);
        toolKey(fixture, Qt::Key_Escape);
        expectations.expect(
            !fixture.viewer.textEditing() && fixture.commands.size() == before + 1 &&
                fixture.session.effectiveStringValue(
                    fixture.session.parameterForSelection(document::kTextParameterRole)->id) ==
                    QStringLiteral("Hello"),
            "Escape keeps the typed text in the one Add Text Layer step");
        const auto layers = layerCount(fixture);
        toolDrag(fixture, {350, 280}, {350, 280});
        expectations.expect(layerCount(fixture) == layers && !fixture.viewer.textEditing(),
                            "a click after the text commit creates no second text layer");
        expectations.expect(fixture.session.undo() && layerCount(fixture) == layers - 1,
                            "one undo removes the typed text layer");
    }
    // Empty text creates nothing.
    {
        toolReady(fixture, expectations);
        const auto layers = layerCount(fixture);
        const auto undoLabel = fixture.session.undoLabel();
        toolKey(fixture, Qt::Key_T);
        toolMouse(fixture, QEvent::MouseButtonPress, {60, 200});
        expectations.expect(waitUntil([&] { return fixture.viewer.textEditing(); }),
                            "a second text placement enters edit mode");
        toolKey(fixture, Qt::Key_Backspace);
        toolKey(fixture, Qt::Key_Escape);
        expectations.expect(layerCount(fixture) == layers &&
                                fixture.session.undoLabel() == undoLabel &&
                                !fixture.viewer.textEditing() && selectToolActive(fixture),
                            "Escape on emptied new text leaves no layer and no undo step");
        expectations.expect(!fixture.session.redo() && layerCount(fixture) == layers,
                            "Redo never re-creates the emptied text layer");
    }
    toolShutdown(fixture, expectations);
    fixture.viewer.setParent(nullptr);
    properties.setParent(nullptr);
}

void testViewerRoi(Expectations& expectations) {
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(900, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    const auto revision = fixture.session.snapshot().revision();
    toolDrag(fixture, {50, 40}, {180, 120}, Qt::ControlModifier);
    toolReady(fixture, expectations);
    const auto roi = fixture.controller.regionOfInterest();
    expectations.expect(roi.has_value() && fixture.session.snapshot().revision() == revision,
                        "Ctrl-drag defines a session-only ROI without a document edit");
    expectations.expect(fixture.controller.state().frame->desiredIdentity().roi == roi,
                        "ROI reaches the prepared request identity");
    auto* toggle = fixture.viewer.findChild<QToolButton*>("viewerRoiToggle");
    toggle->click();
    toolReady(fixture, expectations);
    expectations.expect(!fixture.controller.regionOfInterest(),
                        "ROI toggle restores full evaluation");
    toggle->click();
    toolReady(fixture, expectations);
    expectations.expect(fixture.controller.regionOfInterest() == roi,
                        "ROI toggle preserves its rectangle");
    fixture.viewer.findChild<QToolButton*>("viewerRoiClear")->click();
    toolReady(fixture, expectations);
    expectations.expect(!fixture.controller.regionOfInterest() && !toggle->isChecked(),
                        "clear forgets ROI and disables it");
    toolShutdown(fixture, expectations);
}

void testViewerProbe(Expectations& expectations) {
    using namespace bloom;
    QSettings().remove("viewer/analysis/default");
    ViewerFixture fixture(makeTestProject("Pixel probe"));
    fixture.controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Full);
    expectations.expect(
        fixture.session.addSolidLayer(QStringLiteral("Solid"), {0.125, 0.25, 0.5, 1}),
        "probe fixture adds a known solid");
    fixture.viewer.resize(900, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    ui::WindowStatusBar status(fixture.session, &fixture.controller);
    status.setDisplayedViewer(&fixture.viewer);
    status.resize(1800, ui::kit::px(ui::kit::Size::Control));
    status.show();
    ui::ProbeReadout latest;
    QObject::connect(&fixture.viewer, &ui::ViewerEditor::probeChanged, &fixture.viewer,
                     [&](const ui::ProbeReadout& value) { latest = value; });
    toolMouse(fixture, QEvent::MouseMove, {1.5, 1.5});
    expectations.expect(
        latest.valid && latest.reference && std::abs(latest.reference->red - 0.125) < 1e-6 &&
            std::abs(latest.reference->green - 0.25) < 1e-6 &&
            std::abs(latest.reference->blue - 0.5) < 1e-6 && latest.reference->alpha == 1,
        "retained process probe reads authored reference RGBA within 1e-6");
    expectations.expect(latest.display == render::Rgba8{99, 137, 188, 255} &&
                            std::abs(latest.normalized.red - 99.0 / 255.0) < 1e-12 &&
                            latest.working && std::abs(latest.working->red - 0.125) < 1e-6 &&
                            near(latest.coordinate.x, 1.5) && near(latest.coordinate.y, 1.5),
                        "probe reports working/display values and composition coordinates");
    auto* cell = status.findChild<QLabel*>("windowStatusBarProbe");
    expectations.expect(cell && cell->toolTip().contains("Working space") &&
                            cell->toolTip().contains("Display"),
                        "viewer probe signal feeds the status cell with a complete readout");
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&fixture.viewer, &leave);
    expectations.expect(!latest.valid && cell->text().isEmpty(),
                        "leaving the viewer clears the probe cell");

    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(1));
    toolReady(fixture, expectations);
    (void)fixture.session.setCurrentTime({});
    toolReady(fixture, expectations);
    expectations.expect(!fixture.controller.state().frame->hasProcessFrame(),
                        "returning to the cached frame drops its process image");
    toolMouse(fixture, QEvent::MouseMove, {1.5, 1.5});
    expectations.expect(waitUntil([&] { return latest.reference.has_value(); }) &&
                            fixture.pipeline.probeCalls > 0 && fixture.pipeline.probeOffUi,
                        "display-only cache probe evaluates a 1x1 ROI off the UI thread");
    expectations.expect(latest.reference && std::abs(latest.reference->red - 0.125) < 1e-6,
                        "1x1 reevaluation retains exact reference values");
    const auto calls = fixture.pipeline.probeCalls.load();
    toolMouse(fixture, QEvent::MouseMove, {1.5, 1.5});
    expectations.expect(fixture.pipeline.probeCalls == calls,
                        "repeated frame and pixel probe uses its cached reference value");
    auto* exposure = fixture.viewer.findChild<ui::kit::KValueField*>("viewerExposure");
    exposure->stepBy(10);
    expectations.expect(waitUntil([&] { return latest.display.red == 137; }) && latest.reference &&
                            std::abs(latest.reference->red - 0.125) < 1e-6,
                        "exposure changes the display probe without changing its reference value");
    exposure->stepBy(-10);
    toolMouse(fixture, QEvent::MouseMove, {2.5, 1.5});
    QCoreApplication::sendEvent(&fixture.viewer, &leave);
    expectations.expect(waitUntil([&] { return fixture.scheduler.isQuiescent(); }) && !latest.valid,
                        "late probe completion cannot repopulate a cleared cursor readout");
    toolShutdown(fixture, expectations);
    QSettings().remove("viewer/analysis/default");
}
// ADR 0028: with a layer selected, Rectangle, Ellipse and Pen draw masks in the layer's content
// space; the selected mask's anchors drag into one SetLayerMaskPath.
void testMaskTools(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(800, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    expectations.expect(fixture.session.addSolidLayer("Plate", {0.2, 0.3, 0.4, 1}),
                        "mask fixture adds a solid layer");
    const auto* selectedLayer =
        std::get_if<document::LayerId>(&fixture.session.selection().primary);
    if (selectedLayer == nullptr) {
        expectations.expect(false, "mask fixture selects the new layer");
        toolShutdown(fixture, expectations);
        return;
    }
    const auto layer = *selectedLayer;
    toolReady(fixture, expectations);
    const auto layersBefore = fixture.session.composition()->graph().layerOutputs().size();
    const auto boundsOf = [&] { return fixture.controller.selectedLayerBounds(); };
    const auto contentPoint = [&](document::Vec2d content) {
        const auto bounds = boundsOf();
        const auto world = ui::masks::contentToComposition(bounds.front());
        const auto mapped = world->map(QPointF(content.x, content.y));
        return mapped;
    };
    expectations.expect(!boundsOf().empty(), "the selected layer has evaluated bounds");
    if (boundsOf().empty()) {
        toolShutdown(fixture, expectations);
        return;
    }

    const auto before = fixture.commands.size();
    toolKey(fixture, Qt::Key_R);
    toolDrag(fixture, {50, 60}, {150, 110});
    const auto* boundary = fixture.session.composition()->graph().findLayer(layer);
    expectations.expect(fixture.commands.size() == before + 1 &&
                            fixture.session.composition()->graph().layerOutputs().size() ==
                                layersBefore &&
                            boundary->masks.size() == 1,
                        "a rectangle with a selected layer adds one mask, not a shape layer");
    const auto mask = boundary->masks.front();
    const auto* record = fixture.session.composition()->parameters().find(mask.path);
    const auto* constant =
        record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    const auto* path = constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
    expectations.expect(path && path->closed && path->anchors.size() == 4,
                        "a closed rectangle path");
    if (path && path->anchors.size() == 4) {
        const auto first = contentPoint(path->anchors[0].point);
        const auto third = contentPoint(path->anchors[2].point);
        expectations.expect(near(first.x(), 50) && near(first.y(), 60) && near(third.x(), 150) &&
                                near(third.y(), 110),
                            "the mask path is in content space: it maps back to the drawn corners");
    }
    expectations.expect(fixture.session.selectedMask() &&
                            fixture.session.selectedMask()->mask == mask.id,
                        "the new mask is the selected mask");
    expectations.expect(selectToolActive(fixture),
                        "drawing a mask on the selected layer returns to Select");

    // Ellipse: four anchors with handles.
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_E);
    toolDrag(fixture, {200, 100}, {300, 200});
    boundary = fixture.session.composition()->graph().findLayer(layer);
    expectations.expect(boundary->masks.size() == 2, "the ellipse adds a second mask");
    if (boundary->masks.size() == 2) {
        const auto* ellipseRecord =
            fixture.session.composition()->parameters().find(boundary->masks[1].path);
        const auto* ellipseConstant =
            std::get_if<document::ConstantValueSource>(&ellipseRecord->source);
        const auto& ellipse = std::get<document::PathValue>(ellipseConstant->value);
        expectations.expect(ellipse.closed && ellipse.anchors.size() == 4 &&
                                ellipse.anchors[0].inHandle && ellipse.anchors[0].outHandle,
                            "an ellipse is four anchors with cubic handles");
    }

    // Dragging an anchor of the selected mask commits one SetLayerMaskPath.
    toolReady(fixture, expectations);
    fixture.session.selectMask(layer, mask.id);
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_V);
    const auto beforeDrag = fixture.commands.size();
    const auto revisionBeforeDrag = fixture.session.snapshot().revision();
    toolMouse(fixture, QEvent::MouseButtonPress, {50, 60});
    toolMouse(fixture, QEvent::MouseMove, {70, 80});
    expectations.expect(fixture.commands.size() == beforeDrag &&
                            fixture.session.snapshot().revision() == revisionBeforeDrag,
                        "a mask anchor drag never changes the document before release");
    {
        // The live Viewer frame renders the drag from a session-only override of the mask path.
        const auto overrides = fixture.session.valueEditOverrides();
        expectations.expect(
            overrides.size() == 1 && overrides.front().parameterId == mask.path &&
                std::holds_alternative<document::PathValue>(overrides.front().value),
            "a mask anchor drag previews through a typed Path override");
        const auto alphaAt = [&](const std::vector<runtime::SnapshotParameterOverride>& values) {
            const auto plan = fixture.pipeline.compiler.compile(
                {fixture.document.snapshot(), fixture.session.compositionId(), values}, {});
            if (!plan.plan)
                return -1.0F;
            const auto result = fixture.pipeline.evaluator.evaluate(
                plan.plan,
                {fixture.session.currentTime(), plan.plan->output(),
                 runtime::CompositionFormatResolution{}, runtime::EvaluationQuality::Reference,
                 runtime::EvaluationColorIntent::LinearRec709Scene, 32U << 20U},
                {});
            if (!result.frame())
                return -1.0F;
            const auto pixel = result.frame()->processImage().read(55, 65);
            return pixel ? pixel.value()->alpha() : -1.0F;
        };
        expectations.expect(alphaAt({}) == 1.0F && alphaAt(overrides) == 0.0F,
                            "the live preview shows the dragged outline while the document keeps "
                            "the original");
        expectations.expect(waitUntil([&] {
                                return fixture.controller.state().frame &&
                                       isReady(fixture.controller);
                            }),
                            "the Viewer preview settles during the drag");
    }
    toolMouse(fixture, QEvent::MouseButtonRelease, {70, 80});
    expectations.expect(fixture.commands.size() == beforeDrag + 1,
                        "a mask anchor drag is exactly one transaction");
    {
        const auto* moved = fixture.session.composition()->parameters().find(mask.path);
        const auto& value = std::get<document::PathValue>(
            std::get<document::ConstantValueSource>(moved->source).value);
        const auto corner = contentPoint(value.anchors[0].point);
        expectations.expect(near(corner.x(), 70) && near(corner.y(), 80),
                            "the dragged anchor lands where it was released");
    }
    expectations.expect(fixture.session.undo(), "the anchor drag undoes");

    // Pen closes into a mask as well.
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_P);
    toolDrag(fixture, {20, 200}, {20, 200});
    toolDrag(fixture, {120, 200}, {120, 200});
    toolDrag(fixture, {70, 280}, {70, 280});
    toolDrag(fixture, {20, 200}, {20, 200});
    boundary = fixture.session.composition()->graph().findLayer(layer);
    expectations.expect(boundary->masks.size() == 3 &&
                            fixture.session.composition()->graph().layerOutputs().size() ==
                                layersBefore,
                        "the Pen adds a mask to the selected layer");
    expectations.expect(selectToolActive(fixture), "closing a mask path returns to Select");

    // Escape confirms a mask too: three Pen anchors close into a mask; a mid-drag rectangle
    // commits at its current size; two anchors are no area and create nothing.
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_P);
    toolDrag(fixture, {200, 220}, {200, 220});
    toolDrag(fixture, {300, 220}, {300, 220});
    toolDrag(fixture, {250, 280}, {250, 280});
    toolKey(fixture, Qt::Key_Escape);
    boundary = fixture.session.composition()->graph().findLayer(layer);
    expectations.expect(boundary->masks.size() == 4 && selectToolActive(fixture),
                        "Escape closes a three-anchor Pen mask");
    if (boundary->masks.size() == 4) {
        const auto* escaped =
            fixture.session.composition()->parameters().find(boundary->masks[3].path);
        const auto& value = std::get<document::PathValue>(
            std::get<document::ConstantValueSource>(escaped->source).value);
        expectations.expect(value.closed && value.anchors.size() == 3,
                            "the Escape-finished mask path is closed");
    }
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_R);
    toolMouse(fixture, QEvent::MouseButtonPress, {30, 30});
    toolMouse(fixture, QEvent::MouseMove, {90, 80});
    toolKey(fixture, Qt::Key_Escape);
    toolMouse(fixture, QEvent::MouseButtonRelease, {90, 80});
    boundary = fixture.session.composition()->graph().findLayer(layer);
    expectations.expect(boundary->masks.size() == 5 && selectToolActive(fixture),
                        "Escape mid-drag commits the mask at its current size");
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_P);
    toolDrag(fixture, {20, 20}, {20, 20});
    toolDrag(fixture, {60, 20}, {60, 20});
    toolKey(fixture, Qt::Key_Escape);
    boundary = fixture.session.composition()->graph().findLayer(layer);
    expectations.expect(boundary->masks.size() == 5 &&
                            fixture.session.composition()->graph().layerOutputs().size() ==
                                layersBefore,
                        "a two-anchor Pen mask has no area and creates nothing");

    // No selection keeps today's behaviour: a shape layer.
    fixture.session.clearSelection();
    toolReady(fixture, expectations);
    toolKey(fixture, Qt::Key_R);
    toolDrag(fixture, {10, 10}, {40, 40});
    expectations.expect(fixture.session.composition()->graph().layerOutputs().size() ==
                            layersBefore + 1,
                        "with no layer selected a rectangle still creates a shape layer");
    toolShutdown(fixture, expectations);
}
