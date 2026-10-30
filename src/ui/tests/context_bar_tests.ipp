// Included inside viewer_editor_tests.cpp's anonymous namespace (after viewer_chrome_tests.ipp):
// ADR 0031's one app-wide tool and the context bar -- its tools, the active tool's options, Align
// and Distribute, the overflow at narrow widths and focus (docs/ux/ui-grammar.md, "Context bar").

QToolButton* barButton(bloom::ui::ContextBar& bar, const QString& name) {
    return bar.findChild<QToolButton*>(name);
}

// Review captures: BLOOM_CONTEXT_BAR_CAPTURE_DIR saves the bar as it stands, per device pixel ratio.
void captureBar(bloom::ui::ContextBar& bar, const char* name) {
    const auto directory = qEnvironmentVariable("BLOOM_CONTEXT_BAR_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QCoreApplication::processEvents();
    const auto ratio = bar.devicePixelRatioF() == 1.0 ? QStringLiteral("dpr1") : QStringLiteral("dpr15");
    (void)bar.grab().save(directory + QStringLiteral("/bar-after-") + QString::fromLatin1(name) +
                          QLatin1Char('-') + ratio + QStringLiteral(".png"));
}

bool suppressed(bloom::ui::ContextBar& bar, const char* name) {
    auto* widget = bar.findChild<QWidget*>(QString::fromLatin1(name));
    return widget == nullptr || widget->property("chromeSuppressed").toBool();
}

// One tool for the whole application: a second Viewer of the same session follows a tool chosen
// in the first, by key or by the bar, and a creation in either returns both to Select.
void testOneToolAcrossViewers(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(800, 650);
    fixture.viewer.show();
    ui::ViewerEditor second(fixture.session, fixture.controller);
    second.resize(800, 650);
    second.show();
    toolReady(fixture, expectations);
    auto& tools = fixture.session.tools();
    expectations.expect(tools.tool() == ui::ViewerTool::Select, "the application starts on Select");
    QKeyEvent hand(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
    QCoreApplication::sendEvent(&second, &hand);
    expectations.expect(tools.tool() == ui::ViewerTool::Hand &&
                            fixture.viewer.cursor().shape() == Qt::OpenHandCursor &&
                            second.cursor().shape() == Qt::OpenHandCursor,
                        "H in one Viewer chooses Hand for every Viewer, cursors included");
    for (const auto& [key, tool] :
         std::array{std::pair{Qt::Key_V, ui::ViewerTool::Select}, std::pair{Qt::Key_Z, ui::ViewerTool::Zoom},
                    std::pair{Qt::Key_R, ui::ViewerTool::Rectangle},
                    std::pair{Qt::Key_E, ui::ViewerTool::Ellipse},
                    std::pair{Qt::Key_P, ui::ViewerTool::Pen},
                    std::pair{Qt::Key_T, ui::ViewerTool::Text}}) {
        toolKey(fixture, key);
        expectations.expect(tools.tool() == tool, "each bare tool letter chooses its tool");
    }
    auto& bar = contextBar(fixture);
    // The bar's letters are the same keys (a focused bar field keeps its own typing).
    QKeyEvent zoom(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier);
    QCoreApplication::sendEvent(&bar, &zoom);
    expectations.expect(tools.tool() == ui::ViewerTool::Zoom, "the bar takes the tool letters too");
    toolKey(fixture, Qt::Key_R);
    const auto before = fixture.commands.size();
    toolDrag(fixture, {20, 20}, {120, 90});
    expectations.expect(fixture.commands.size() == before + 1 &&
                            tools.tool() == ui::ViewerTool::Select &&
                            bar.toolButtonForTest(ui::ViewerTool::Select)->isChecked(),
                        "a creation returns the one tool, and the bar, to Select");
    // A zero-size press keeps the tool; choosing a tool alone authors nothing.
    fixture.session.clearSelection();
    const auto chosen = fixture.commands.size();
    tools.setTool(ui::ViewerTool::Ellipse);
    toolDrag(fixture, {200, 150}, {200, 150});
    expectations.expect(fixture.commands.size() == chosen && tools.tool() == ui::ViewerTool::Ellipse,
                        "choosing a tool and a zero-size press create nothing and keep the tool");
    // Escape in either Viewer confirms and returns to Select.
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&second, &escape);
    expectations.expect(tools.tool() == ui::ViewerTool::Select, "Escape returns to Select");
    toolShutdown(fixture, expectations);
}

// The bar's tool buttons show and choose the controller's tool; the four shapes share one slot that
// shows the last one chosen, and its flyout offers all four.
void testContextBarTools(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    auto& bar = contextBar(fixture);
    bar.show();
    QCoreApplication::processEvents();
    auto& tools = fixture.session.tools();
    for (const auto tool : ui::kViewerTools) {
        auto* button = bar.toolButtonForTest(tool);
        expectations.expect(button != nullptr && button->focusPolicy() == Qt::NoFocus &&
                                !button->accessibleName().isEmpty(),
                            "every tool has a slot that never takes focus and has a name");
    }
    expectations.expect(bar.toolButtonForTest(ui::ViewerTool::Select)->toolTip().startsWith(
                            QStringLiteral("Select (V)\n")) &&
                            bar.toolButtonForTest(ui::ViewerTool::Line)->toolTip().startsWith(
                                QStringLiteral("Line\n")),
                        "tooltips name the tool and its key, then how to use it");
    bar.toolButtonForTest(ui::ViewerTool::Pen)->click();
    expectations.expect(tools.tool() == ui::ViewerTool::Pen &&
                            bar.toolButtonForTest(ui::ViewerTool::Pen)->isChecked() &&
                            !bar.toolButtonForTest(ui::ViewerTool::Select)->isChecked(),
                        "a tool button drives the one tool, exclusively");
    tools.setTool(ui::ViewerTool::Hand);
    expectations.expect(bar.toolButtonForTest(ui::ViewerTool::Hand)->isChecked() &&
                            !bar.toolButtonForTest(ui::ViewerTool::Pen)->isChecked(),
                        "the bar reflects a tool chosen elsewhere");
    auto* slot = bar.toolButtonForTest(ui::ViewerTool::Rectangle);
    expectations.expect(slot->menu() == bar.shapeFlyoutForTest() &&
                            slot->popupMode() == QToolButton::DelayedPopup &&
                            bar.shapeFlyoutForTest()->actions().size() == 4,
                        "the shape slot holds the four shapes behind a press and hold");
    for (auto* action : bar.shapeFlyoutForTest()->actions())
        if (action->objectName() == QStringLiteral("contextBarToolStar"))
            action->trigger();
    expectations.expect(tools.tool() == ui::ViewerTool::Star && slot->isChecked() &&
                            slot->glyph() == ui::kit::IconId::Star &&
                            slot->toolTip().startsWith(QStringLiteral("Star\n")),
                        "choosing a shape from the flyout makes the slot show it");
    tools.setTool(ui::ViewerTool::Select);
    slot->click();
    expectations.expect(tools.tool() == ui::ViewerTool::Star,
                        "the slot chooses the last shape again");
    toolKey(fixture, Qt::Key_E);
    expectations.expect(slot->glyph() == ui::kit::IconId::Ellipse && slot->isChecked(),
                        "a shape key also becomes the slot's shape");
    toolShutdown(fixture, expectations);
}

// The middle of the bar holds the active tool's options: stable slots per tool, real state only.
void testContextBarOptions(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(800, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    auto& bar = contextBar(fixture);
    bar.show();
    auto& tools = fixture.session.tools();
    const auto groups = [&](const bool anchors, const bool shape, const bool text) {
        return suppressed(bar, "contextBarAnchorOptions") != anchors &&
               suppressed(bar, "contextBarShapeOptions") != shape &&
               suppressed(bar, "contextBarTextOptions") != text;
    };
    // The handle-mode strip is hidden (not disabled) while no path or mask is selected.
    expectations.expect(groups(false, false, false),
                        "Select with no path selected hides the anchor handle modes");
    captureBar(bar, "select");
    tools.setTool(ui::ViewerTool::Pen);
    expectations.expect(groups(false, true, false),
                        "the Pen with no path selected offers fill/stroke only");
    tools.setTool(ui::ViewerTool::Rectangle);
    expectations.expect(groups(false, true, false), "a shape tool offers fill and stroke");
    captureBar(bar, "rectangle");
    tools.setTool(ui::ViewerTool::Text);
    expectations.expect(groups(false, false, true), "the Text tool offers font, size, alignment");
    captureBar(bar, "text");
    tools.setTool(ui::ViewerTool::Hand);
    expectations.expect(groups(false, false, false), "Hand has no options");

    tools.setTool(ui::ViewerTool::Select);
    expectations.expect(suppressed(bar, "contextBarAnchorOptions"),
                        "the handle modes stay hidden when nothing is editable");

    // New shapes take the bar's style: no shape selected, so the options are the new-shape style.
    tools.setTool(ui::ViewerTool::Rectangle);
    fixture.session.clearSelection();
    auto* stroke = bar.findChild<ui::kit::KCheckBox*>("contextBarShapeStroke");
    auto* width = bar.findChild<ui::kit::KValueField*>("contextBarShapeStrokeWidth");
    const auto history = fixture.commands.size();
    stroke->click();
    width->setValue(6.0);
    emit width->valueChanged(6.0);
    expectations.expect(fixture.commands.size() == history &&
                            tools.newShapeStyle().strokeEnabled &&
                            near(tools.newShapeStyle().strokeWidth, 6.0),
                        "with no shape selected the options set the new-shape style, unauthored");
    toolDrag(fixture, {30, 30}, {130, 100});
    expectations.expect(std::get<bool>(toolParameter(fixture, "strokeEnabled")) &&
                            near(std::get<double>(toolParameter(fixture, "strokeWidth")), 6.0) &&
                            fixture.commands.size() == history + 1,
                        "the new rectangle is drawn with that style in its one creation step");
    // Now the rectangle is selected: the same options edit it through Properties' commands.
    tools.setTool(ui::ViewerTool::Ellipse);
    width->setValue(9.0);
    emit width->valueChanged(9.0);
    expectations.expect(near(std::get<double>(toolParameter(fixture, "strokeWidth")), 9.0) &&
                            fixture.commands.size() == history + 2 &&
                            near(tools.newShapeStyle().strokeWidth, 6.0),
                        "with a shape selected the options edit it as one undoable command");
    // A line has no fill: the slot stays, disabled with the reason.
    fixture.session.clearSelection();
    tools.setTool(ui::ViewerTool::Line);
    auto* fill = bar.findChild<ui::kit::KCheckBox*>("contextBarShapeFill");
    expectations.expect(!fill->isEnabled() && !fill->isHidden() &&
                            fill->toolTip().contains("no fill"),
                        "a line keeps the fill slot, disabled with the reason");

    // Text: the new-text size and alignment reach the next text layer.
    tools.setTool(ui::ViewerTool::Text);
    auto* size = bar.findChild<ui::kit::KValueField*>("contextBarTextSize");
    auto* alignment = bar.findChild<ui::kit::KRadioGroup*>("contextBarTextAlignment");
    size->setValue(40.0);
    emit size->valueChanged(40.0);
    alignment->setCurrentIndex(1);
    expectations.expect(near(tools.newTextStyle().size, 40.0) && tools.newTextStyle().alignment == 1,
                        "with no text selected the text options set the new-text style");
    toolReady(fixture, expectations);
    toolDrag(fixture, {200, 150}, {200, 150});
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"));
    QCoreApplication::sendEvent(&fixture.viewer, &typed);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&fixture.viewer, &escape);
    {
        const auto createdSize = toolParameter(fixture, "size");
        const auto createdAlignment = toolParameter(fixture, "alignment");
        const auto* sizeValue = std::get_if<double>(&createdSize);
        const auto* alignmentValue = std::get_if<std::int64_t>(&createdAlignment);
        expectations.expect(sizeValue && near(*sizeValue, 40.0) && alignmentValue &&
                                *alignmentValue == 1,
                            "new text is created at the bar's size and alignment");
    }
    toolShutdown(fixture, expectations);
}

// Align and Distribute: one undoable command per click, against the chosen target, disabled with a
// reason when they cannot run; the groups collapse into the overflow menu at narrow widths.
void testContextBarAlignment(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(800, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    for (const auto& [x, y, w] : std::array{std::tuple{60.0, 60.0, 40.0}, std::tuple{200.0, 120.0, 80.0},
                                            std::tuple{320.0, 220.0, 20.0}}) {
        commands::ShapeLayerGeometry geometry;
        geometry.position = document::Vec2d{x, y};
        geometry.size = document::Vec2d{w, 30.0};
        (void)fixture.session.addShapeLayer(document::ShapeKind::Rectangle, geometry);
    }
    auto& bar = contextBar(fixture);
    bar.show();
    // These cases are about the selection as the target; Composition is the default.
    fixture.session.tools().setAlignmentTarget(commands::AlignmentTarget::Selection);
    auto& actions = bar.alignment();
    using A = commands::LayerArrangement;
    auto* left = actions.action(A::AlignLeft);
    auto* distribute = actions.action(A::DistributeHorizontalCentres);
    // The last rectangle is selected alone.
    toolReady(fixture, expectations);
    expectations.expect(!left->isEnabled() && left->toolTip().contains("two or more") &&
                            !distribute->isEnabled() && distribute->toolTip().contains("three"),
                        "one layer cannot align to its own selection, and distributing needs three");
    std::set<document::NodeId> nodes;
    for (const auto& boundary : fixture.session.composition()->graph().layerOutputs())
        nodes.insert(boundary.nodeId);
    const auto primary = fixture.session.composition()->graph().layerOutputs().front();
    fixture.session.selectNodes(nodes, primary.nodeId);
    toolReady(fixture, expectations);
    captureBar(bar, "align-enabled");
    expectations.expect(left->isEnabled() && distribute->isEnabled() &&
                            actions.keyLayerName() == QString::fromStdString(primary.name),
                        "three selected layers align and distribute; the primary is the key");
    const auto history = fixture.commands.size();
    auto* button = barButton(bar, QStringLiteral("contextBar-") + left->objectName());
    expectations.expect(button != nullptr && button->focusPolicy() == Qt::NoFocus,
                        "the Align buttons never take focus");
    if (button != nullptr)
        button->click();
    toolReady(fixture, expectations);
    const auto bounds = fixture.controller.currentLayerBounds();
    const auto lefts = [&] {
        std::vector<double> values;
        for (const auto& entry : bounds)
            values.push_back(entry.output.left);
        return values;
    }();
    expectations.expect(fixture.commands.size() == history + 1 && lefts.size() == 3 &&
                            near(lefts[0], 40.0) && near(lefts[1], 40.0) && near(lefts[2], 40.0),
                        "Align Left Edges lines the selection up on its leftmost edge in one step");
    (void)fixture.session.undo();
    toolReady(fixture, expectations);
    std::vector<double> restored;
    for (const auto& entry : fixture.controller.currentLayerBounds())
        restored.push_back(entry.output.left);
    std::ranges::sort(restored);
    expectations.expect(restored.size() == 3 && near(restored[0], 40.0) &&
                            near(restored[1], 160.0) && near(restored[2], 310.0),
                        "one undo reverses the whole alignment");
    // Align To Key Object: the key stays where it is.
    fixture.session.tools().setAlignmentTarget(commands::AlignmentTarget::KeyObject);
    auto* target = bar.findChild<ui::kit::KDropdown*>("contextBarAlignTo");
    expectations.expect(target->currentIndex() == 2 &&
                            target->currentText().contains(QString::fromStdString(primary.name)),
                        "Align To shows the target, naming the key layer");
    actions.action(A::AlignRight)->trigger();
    toolReady(fixture, expectations);
    const auto keyed = fixture.controller.currentLayerBounds();
    bool rightsMatch = keyed.size() == 3;
    for (const auto& entry : keyed)
        rightsMatch = rightsMatch && near(entry.output.right, keyed.front().output.right);
    expectations.expect(rightsMatch, "Align Right to the key object lines up on the key's edge");

    // A narrow bar collapses the selection actions, Distribute first, into one overflow menu.
    auto* overflow = bar.findChild<QToolButton*>("contextBarOverflowButton");
    bar.resize(1600, bar.height());
    QCoreApplication::processEvents();
    expectations.expect(overflow != nullptr && overflow->isHidden(),
                        "a wide bar shows every group and no overflow");
    bar.resize(560, bar.height());
    QCoreApplication::processEvents();
    captureBar(bar, "narrow");
    auto* distributeGroup = bar.findChild<QWidget*>("contextBarDistribute");
    auto* tools = bar.findChild<QWidget*>("contextBarTools");
    bool offered = false;
    if (overflow != nullptr && overflow->menu() != nullptr)
        for (auto* action : overflow->menu()->actions())
            offered = offered || action->text() == QStringLiteral("Distribute");
    expectations.expect(overflow && overflow->isVisible() && distributeGroup->isHidden() &&
                            tools->isVisible() && offered &&
                            overflow->focusPolicy() == Qt::NoFocus,
                        "a narrow bar folds Distribute into the overflow menu, keeping the tools");
    toolShutdown(fixture, expectations);
}

// A field in the bar commits and hands keyboard focus back to the Viewer.
void testContextBarReturnsFocus(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* bar = new ui::ContextBar(fixture.session, &fixture.controller, &window);
    layout->addWidget(bar);
    fixture.viewer.setParent(&window);
    layout->addWidget(&fixture.viewer);
    bar->setViewer(&fixture.viewer);
    window.resize(1400, 700);
    window.show();
    window.activateWindow();
    fixture.session.tools().setTool(ui::ViewerTool::Text);
    auto* size = bar->findChild<ui::kit::KValueField*>("contextBarTextSize");
    size->setFocus();
    QCoreApplication::processEvents();
    emit size->editFinished();
    QCoreApplication::processEvents();
    expectations.expect(fixture.viewer.hasFocus(), "a committed bar field returns focus to the Viewer");
    fixture.viewer.setParent(nullptr);
    toolShutdown(fixture, expectations);
}

// Align To starts on Composition; the Workspace field sits at the right, the alignment group in the
// centre, and the bar's content lines up with the panels below (one Gutter in from each edge).
void testContextBarLayoutAndDefaults(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(800, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    auto& bar = contextBar(fixture);
    bar.resize(1400, bar.height());
    bar.show();
    QCoreApplication::processEvents();
    auto* target = bar.findChild<ui::kit::KDropdown*>("contextBarAlignTo");
    expectations.expect(fixture.session.tools().alignmentTarget() ==
                                commands::AlignmentTarget::Composition &&
                            target != nullptr && target->currentText() == QStringLiteral("Composition"),
                        "Align To defaults to Composition");
    auto* tools = bar.findChild<QWidget*>("contextBarTools");
    auto* align = bar.findChild<QWidget*>("contextBarAlign");
    expectations.expect(tools && align && tools->mapTo(&bar, QPoint()).x() == ui::kit::px(ui::kit::Spacing::Gutter),
                        "the left edge of the tools lines up with the panels (one Gutter in)");
    const int centre = align ? align->mapTo(&bar, QPoint()).x() + align->width() / 2 : 0;
    expectations.expect(align && centre > bar.width() / 4 && centre < bar.width() * 3 / 4,
                        "the alignment group sits in the middle of the bar");
}

// The Workspace field: built-ins, separator, user presets, separator, then the actions.
void testContextBarWorkspaceField(Expectations& expectations) {
    using namespace bloom;
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    ViewerFixture fixture(toolProject());
    ui::EditorRegistry registry;
    for (const auto* id : {"bloom.viewer", "bloom.timeline", "bloom.assets", "bloom.nodes",
                           "bloom.properties", "bloom.export"})
        (void)registry.registerEditor(
            {id, QString::fromLatin1(id), [](QWidget* parent) { return new QWidget(parent); }});
    ui::WorkspaceHost host(registry);
    host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.timeline",
                              "bloom.properties");
    host.resize(1400, 800);
    host.show();
    ui::WorkspacePresets presets(host);
    auto& bar = contextBar(fixture);
    bar.setWorkspacePresets(&presets);
    bar.resize(1400, bar.height());
    bar.show();
    QCoreApplication::processEvents();
    auto* field = bar.workspaceDropdownForTest();
    expectations.expect(field != nullptr && field->isVisible() &&
                            field->mapTo(&bar, QPoint()).x() > bar.width() / 2 &&
                            field->currentText().startsWith(QStringLiteral("Default")),
                        "the Workspace field sits at the right and starts on Default");
    const auto indexOf = [&](const QString& text) {
        for (int index = 0; index < field->count(); ++index)
            if (field->itemText(index) == text)
                return index;
        return -1;
    };
    field->chooseIndex(indexOf(QStringLiteral("Node")));
    expectations.expect(presets.activeName() == QStringLiteral("Node") &&
                            field->currentText() == QStringLiteral("Node") &&
                            indexOf(QStringLiteral("Custom")) < 0 &&
                            indexOf(QStringLiteral("Save current layout as…")) > 0,
                        "choosing a preset applies it and the field names it");
    (void)host.splitActiveArea(Qt::Horizontal);
    expectations.expect(field->currentText() == QStringLiteral("Node (edited)"),
                        "a manual change shows \"Node (edited)\"");
    host.hide();
}
