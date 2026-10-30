// Included inside viewer_editor_tests.cpp's anonymous namespace (after viewer_tools_tests.ipp): the
// Viewer's chrome -- the footer's three groups and priority collapse, the header's empty states,
// the no-composition invitation, the absent tool column (ADR 0031), and the menu structure with its
// shared actions (docs/ux/ui-grammar.md, "Viewer header and footer", "Viewer menus").

// The footer's collapse ranks, lowest first (viewer_editor_internal.hpp).
struct FooterRank {
    const char* name;
    int rank;
};
constexpr std::array<FooterRank, 12> kFooterRanks{{{"viewerLookToggle", 1},
                                                   {"viewerDisplayView", 2},
                                                   {"viewerExposure", 3},
                                                   {"viewerGamma", 3},
                                                   {"viewerRoiToggle", 4},
                                                   {"viewerTransparencyGridToggle", 5},
                                                   {"viewerChannelDropdown", 6},
                                                   {"timelineRamPreviewButton", 8},
                                                   {"viewerStepToStartButton", 9},
                                                   {"viewerStepToEndButton", 9},
                                                   {"viewerZoomDropdown", 10},
                                                   {"viewerResolutionDropdown", 10}}};
// After every collapse rank, the frame field drops its duration (CompactFrameField).
constexpr int kCompactFrameFieldRank = 11;
static_assert(kFooterRanks.back().rank < kCompactFrameFieldRank);
// Chrome controls that are not ranked on their own: the overflow button itself, and Clear ROI,
// which shares the ROI toggle's rank and is hidden while no region exists.
constexpr std::array<const char*, 2> kUnrankedFooterControls{"viewerFooterOverflowButton",
                                                             "viewerRoiClear"};
constexpr std::array<const char*, 5> kNeverCollapses{"timelineStepBackButton", "playPauseButton",
                                                     "timelineStepForwardButton",
                                                     "timelineLoopIndicator", "viewerTimeReadout"};

void testViewerFooterLayoutAtEveryWidth(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Footer widths"));
    auto* footer = bloom::ui::test::footer(fixture.viewer);
    auto* overflow = footer->findChild<QToolButton*>("viewerFooterOverflowButton");
    expectations.expect(overflow != nullptr && overflow->menu() != nullptr,
                        "the footer has a View options overflow button with a menu");
    if (overflow == nullptr)
        return reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
    const auto control = [footer](const char* name) {
        return footer->findChild<QWidget*>(QString::fromLatin1(name));
    };
    for (const int width : {320, 480, 640, 800, 1200, 1920}) {
        const std::string at = " at " + std::to_string(width) + " px";
        footer->resize(width, ui::kit::px(ui::kit::Size::FooterRow));
        (void)footer->grab();
        QCoreApplication::processEvents();
        // Nothing is clipped or pushed out of the row, and nothing overlaps.
        std::vector<QRect> placed;
        bool inside = true;
        bool disjoint = true;
        for (auto* child : footer->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
            if (child->isHidden() || !child->property("chromeControl").toBool())
                continue;
            inside = inside && footer->rect().contains(child->geometry());
            for (const auto& other : placed)
                disjoint = disjoint && !other.intersects(child->geometry());
            placed.push_back(child->geometry());
        }
        expectations.expect(inside, "every footer control lies inside the row" + at);
        expectations.expect(disjoint, "no two footer controls overlap" + at);
        for (const char* name : kNeverCollapses)
            expectations.expect(control(name) != nullptr && !control(name)->isHidden(),
                                std::string{name} + " never collapses" + at);
        // Last of all, the frame field drops its duration rather than leave the row.
        if (auto* duration = footer->findChild<ui::kit::KLabel*>("viewerTimeReadoutDuration");
            width == 320 || width == 1920)
            expectations.expect(duration != nullptr && duration->isHidden() == (width == 320),
                                "the frame field shows its duration unless the footer is at its "
                                "narrowest" +
                                    at);
        // CompactFrameField is last: the duration drops only once every collapse rank has gone.
        if (auto* duration = footer->findChild<ui::kit::KLabel*>("viewerTimeReadoutDuration");
            duration != nullptr && duration->isHidden())
            expectations.expect(std::ranges::all_of(kFooterRanks,
                                                    [&](const auto& entry) {
                                                        return control(entry.name)->isHidden();
                                                    }),
                                "the frame field drops its duration (rank " +
                                    std::to_string(kCompactFrameFieldRank) +
                                    ") only after every collapse rank" + at);
        // The collapse order: once a rank has left the row, every lower rank has too.
        int highestCollapsed = 0;
        for (const auto& entry : kFooterRanks)
            if (control(entry.name)->isHidden())
                highestCollapsed = std::max(highestCollapsed, entry.rank);
        bool ordered = true;
        for (const auto& entry : kFooterRanks)
            ordered = ordered && (entry.rank > highestCollapsed || control(entry.name)->isHidden());
        expectations.expect(ordered, "the footer collapses Look, Display/View, exposure/gamma, "
                                     "ROI, transparency grid, channel, then the rest, in that "
                                     "order" +
                                         at);
        // Every control the footer places has a rank: a new footer control with none fails here
        // instead of collapsing in an order nobody chose.
        if (width == 1920)
            for (auto* child :
                 footer->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
                if (!child->property("chromeControl").toBool())
                    continue;
                const auto name = child->objectName().toStdString();
                const bool ranked = std::ranges::any_of(
                    kFooterRanks, [&](const auto& entry) { return name == entry.name; });
                const bool fixed = std::ranges::any_of(
                    kNeverCollapses, [&](const char* entry) { return name == entry; });
                expectations.expect(
                    ranked || fixed ||
                        std::ranges::any_of(kUnrankedFooterControls,
                                            [&](const char* entry) { return name == entry; }),
                    "footer control '" + name + "' has a collapse rank");
            }
        expectations.expect(overflow->isHidden() == (highestCollapsed == 0),
                            "View options appears exactly when something collapsed" + at);
        if (width == 1920) {
            expectations.expect(highestCollapsed == 0, "a wide footer shows every control");
            // The time group is centred in the row while there is room for it.
            const int left = control("viewerStepToStartButton")->geometry().left();
            const int right = control("viewerTimeReadout")->geometry().right();
            expectations.expect(std::abs((left + right) / 2 - width / 2) <= 2,
                                "the transport and readout sit centred in a wide footer");
            expectations.expect(control("viewerTimeReadout")->geometry().left() -
                                        control("timelineLoopIndicator")->geometry().right() <=
                                    ui::kit::px(ui::kit::Spacing::ChromeGap) + 1,
                                "the readout sits beside the transport, never stranded");
        }
        if (width == 640) {
            // A collapsed control is offered in View options under the same action View holds.
            auto* viewMenu = fixture.viewer.findChild<QMenu*>("viewerViewMenu");
            auto* look = fixture.viewer.findChild<QAction*>("viewerLookAction");
            const auto offered = overflow->menu()->actions();
            expectations.expect(look != nullptr && offered.contains(look) && viewMenu != nullptr &&
                                    viewMenu->actions().contains(look),
                                "collapsed Look is the View menu's own Look action");
            auto* displayView = fixture.viewer.findChild<QMenu*>("viewerDisplayViewMenu");
            expectations.expect(displayView != nullptr &&
                                    offered.contains(displayView->menuAction()),
                                "collapsed Display / View is View's own Display / View menu");
        }
    }
    // A wide-to-narrow-to-wide sweep restores every control and the full resolution labels.
    auto* resolution = footer->findChild<ui::kit::KDropdown*>("viewerResolutionDropdown");
    footer->resize(1920, ui::kit::px(ui::kit::Size::FooterRow));
    (void)footer->grab();
    resolution->setCurrentIndex(2);
    expectations.expect(resolution->currentText() == QStringLiteral("Half"),
                        "a wide footer shows the full resolution name");
    footer->resize(560, ui::kit::px(ui::kit::Size::FooterRow));
    (void)footer->grab();
    expectations.expect(resolution->isHidden() || resolution->currentText() == QStringLiteral("½"),
                        "a narrow footer compresses resolution to its compact form");
    footer->resize(1920, ui::kit::px(ui::kit::Size::FooterRow));
    (void)footer->grab();
    expectations.expect(resolution->currentText() == QStringLiteral("Half") && overflow->isHidden(),
                        "widening again restores every control and the full label");
    resolution->setCurrentIndex(0);
    QSettings().remove("viewer/resolution");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

void testViewerFooterControlsShareOneVocabulary(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("Footer vocabulary"));
    auto* footer = bloom::ui::test::footer(fixture.viewer);
    footer->resize(1920, ui::kit::px(ui::kit::Size::FooterRow));
    (void)footer->grab();
    for (auto* child : footer->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly)) {
        if (child->isHidden() || !child->property("chromeControl").toBool())
            continue;
        expectations.expect(child->height() == ui::kit::px(ui::kit::Size::Control),
                            child->objectName().toStdString() + " is one Control tall");
    }
    // The toggles are icon buttons with one Selected plate: the same kind as the transport.
    for (const char* name : {"viewerRoiToggle", "viewerTransparencyGridToggle", "viewerLookToggle",
                             "timelineLoopIndicator"}) {
        auto* toggle = footer->findChild<ui::kit::KIconButton*>(QString::fromLatin1(name));
        expectations.expect(toggle != nullptr && toggle->isCheckable() &&
                                qobject_cast<ui::kit::KIconToggle*>(toggle) == nullptr &&
                                toggle->width() == ui::kit::px(ui::kit::Size::Control) &&
                                !toggle->toolTip().isEmpty(),
                            std::string{name} + " is a Control-square icon toggle with help");
    }
    auto* ram = footer->findChild<ui::kit::KButton*>("timelineRamPreviewButton");
    expectations.expect(ram != nullptr && ram->isCheckable() && !ram->text().isEmpty(),
                        "RAM Preview is a labelled toggle");
    // Exposure and gamma reset from their context menu and mark a value that is not the default.
    auto* exposure = footer->findChild<ui::kit::KValueField*>("viewerExposure");
    auto* reset = fixture.viewer.findChild<QAction*>("viewerResetViewAdjustAction");
    exposure->setValue(1.5);
    expectations.expect(exposure->isNonDefault() && reset->isEnabled(),
                        "a changed exposure is marked and can be reset");
    reset->trigger();
    expectations.expect(exposure->value() == 0.0 && !exposure->isNonDefault() &&
                            !reset->isEnabled(),
                        "Reset Exposure and Gamma returns both to their defaults");
    exposure->setValue(-2.0);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, exposure->labelRect().center(),
                            exposure->mapToGlobal(exposure->labelRect().center()), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(exposure, &doubleClick);
    expectations.expect(exposure->value() == 0.0, "a double-click on EV resets exposure");
    // The readout keeps its own format, disabled, with no composition (testViewerHeaderEmpty...).
    QSettings().remove(QStringLiteral("viewer/analysis"));
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// An empty project: the selectors say there is nothing to choose, the canvas shows the plain
// surround (no frame background or grid) under the invitation, and its button runs New
// Composition.
void testViewerEmptyProjectChrome(Expectations& expectations) {
    using namespace bloom;
    QSettings().setValue("viewer/transparency-grid", true); // even with the grid on
    document::Project emptyProject(document::ProjectId::fromRaw(1), "Empty chrome project");
    document::Document document(std::move(emptyProject));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, document::CompositionId::fromRaw(1));
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    runtime::NodeDefinitionRegistry definitions;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                        "empty chrome fixture registers node definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProvider);
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline);
    // Hosted in a window that has the application's New Composition command.
    QWidget host;
    auto* newComposition = new QAction(QStringLiteral("New Composition…"), &host);
    newComposition->setObjectName(QStringLiteral("compositionNewAction"));
    int newCompositionRuns = 0;
    QObject::connect(newComposition, &QAction::triggered, [&] { ++newCompositionRuns; });
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* viewer = new ui::ViewerEditor(session, controller, nullptr, &host);
    layout->addWidget(viewer);
    host.resize(800, 600);
    host.show();
    QCoreApplication::processEvents();

    auto* compositions = viewer->findChild<ui::kit::KDropdown*>("viewerCompositionSelector");
    auto* layers = viewer->findChild<ui::kit::KDropdown*>("viewerObjectSelector");
    expectations.expect(compositions->currentText() == QStringLiteral("No composition") &&
                            !compositions->isEnabled() &&
                            compositions->toolTip().contains(QStringLiteral("Composition")),
                        "the composition selector says there is none, disabled, and names what "
                        "it selects");
    expectations.expect(layers->currentText() == QStringLiteral("No layers") &&
                            !layers->isEnabled() &&
                            layers->toolTip().contains(QStringLiteral("layer")),
                        "the layer selector says there is nothing to select, disabled");
    expectations.expect(
        viewer->timeReadoutTextForTest() == QStringLiteral("00:00:00:00") &&
            !viewer->findChild<QWidget*>("viewerTimeReadout")->isEnabled() &&
            viewer->findChild<ui::kit::KLabel*>("timelineTimeReadout")->tone() ==
                ui::kit::Color::Faint &&
            viewer->findChild<ui::kit::KLabel*>("viewerTimeReadoutDuration")->isHidden(),
        "the readout shows a disabled zero in its own format, never a dash");
    // The transport has nothing to act on: disabled, and each control's tooltip says why.
    for (const char* name :
         {"viewerStepToStartButton", "timelineStepBackButton", "playPauseButton",
          "timelineStepForwardButton", "viewerStepToEndButton", "timelineLoopIndicator"}) {
        auto* transport = viewer->findChild<QToolButton*>(QString::fromLatin1(name));
        expectations.expect(transport != nullptr && !transport->isEnabled() &&
                                transport->toolTip().contains(QStringLiteral("no composition")),
                            std::string{name} + " is disabled with a reason without a composition");
    }

    auto* invitation = viewer->findChild<QWidget*>("viewerEmptyState");
    auto* button = viewer->findChild<ui::kit::KButton*>("viewerNewCompositionButton");
    expectations.expect(invitation != nullptr && invitation->isVisible() && button != nullptr &&
                            button->variant() == ui::kit::KButton::Variant::Primary,
                        "the empty canvas shows the invitation with a primary New Composition");
    const QRectF canvas = viewer->canvasRectForTest();
    expectations.expect(
        invitation != nullptr &&
            std::abs(invitation->geometry().center().x() - canvas.center().x()) <= 1 &&
            std::abs(invitation->geometry().center().y() - canvas.center().y()) <= 1,
        "the invitation is centred on the canvas");
    if (button != nullptr)
        button->click();
    expectations.expect(newCompositionRuns == 1,
                        "the invitation's button runs the application's New Composition command");

    // The plain surround, not the checkerboard, everywhere the invitation is not.
    const QImage image = viewer->grab().toImage();
    const QRect content = viewer->contentRectForTest().toRect();
    const QColor canvasColor = ui::kit::color(ui::kit::Color::Canvas);
    bool plain = true;
    for (const QPoint point :
         {content.topLeft() + QPoint(4, 4), content.topRight() + QPoint(-4, 4),
          content.bottomLeft() + QPoint(4, -4), content.center() - QPoint(content.width() / 3, 0),
          content.center() + QPoint(content.width() / 3, content.height() / 3)}) {
        const auto ratio = image.devicePixelRatio();
        plain = plain && image.pixelColor(QPointF(point * ratio).toPoint()) == canvasColor;
    }
    expectations.expect(plain, "with no composition the canvas paints no checkerboard");
    QSettings().remove("viewer/transparency-grid");
    delete viewer;
    reachQuiescence(controller, bridge, scheduler, expectations);
}

void testViewerChromeRoiClearFollowsTheRegion(Expectations& expectations) {
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(1300, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    auto* clear = fixture.viewer.findChild<QToolButton*>("viewerRoiClear");
    auto* clearAction = fixture.viewer.findChild<QAction*>("viewerClearRoiAction");
    expectations.expect(clear->isHidden() && !clearAction->isEnabled(),
                        "Clear ROI is not offered while there is no region");
    toolDrag(fixture, {50, 40}, {180, 120}, Qt::ControlModifier);
    toolReady(fixture, expectations);
    expectations.expect(!clear->isHidden() && clearAction->isEnabled(),
                        "drawing a region offers Clear ROI");
    clearAction->trigger();
    toolReady(fixture, expectations);
    expectations.expect(clear->isHidden() && !fixture.controller.regionOfInterest(),
                        "clearing the region withdraws Clear ROI again");
    toolShutdown(fixture, expectations);
}

// ADR 0031: the tools moved to the context bar; the Viewer has no tool column, its canvas takes
// the full width, and a short Viewer keeps its canvas floor.
void testViewerHasNoToolColumn(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(makeTestProject("No tool column"));
    fixture.viewer.resize(500, ui::kit::px(ui::kit::Size::ViewerMinHeight));
    fixture.viewer.show();
    QCoreApplication::processEvents();
    expectations.expect(fixture.viewer.minimumHeight() ==
                            ui::kit::px(ui::kit::Size::ViewerMinHeight),
                        "the Viewer's minimum height is its canvas floor");
    expectations.expect(fixture.viewer.findChild<QWidget*>("viewerToolColumn") == nullptr,
                        "the Viewer has no tool column");
    expectations.expect(fixture.viewer.contentRectForTest().left() == 0.0 &&
                            fixture.viewer.canvasRectForTest().left() ==
                                ui::kit::px(ui::kit::Size::ViewerWorkPadding),
                        "the canvas runs from the Viewer's left edge, padded only by the work "
                        "padding");
    for (const auto tool : ui::kViewerTools)
        expectations.expect(fixture.viewer.findChild<QWidget*>(
                                QStringLiteral("viewer") +
                                QString::fromLatin1(ui::viewerToolInfo(tool).objectStem.data()) +
                                QStringLiteral("Tool")) == nullptr,
                            "no Viewer-owned button for the " +
                                ui::viewerToolInfo(tool).name.toStdString() + " tool");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

void testViewerMenusAreOrganisedAndShared(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(900, 650);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    auto* view = fixture.viewer.findChild<QMenu*>("viewerViewMenu");
    QStringList submenus;
    for (auto* action : view->actions())
        if (action->menu() != nullptr)
            submenus.push_back(action->menu()->objectName());
    expectations.expect(!submenus.contains(QStringLiteral("viewerCompositionMenu")),
                        "View carries no composition commands");
    expectations.expect(submenus == QStringList{QStringLiteral("viewerZoomMenu"),
                                                QStringLiteral("viewerResolutionMenu"),
                                                QStringLiteral("viewerChannelMenu"),
                                                QStringLiteral("viewerDisplayViewMenu"),
                                                QStringLiteral("viewerGuidesMenu"),
                                                QStringLiteral("viewerBackgroundMenu")},
                        "View groups zoom, resolution, channel, display/view, guides and "
                        "background");
    auto* actualSize = fixture.viewer.findChild<QAction*>("viewerActualSizeAction");
    expectations.expect(actualSize->text() == QStringLiteral("100%"),
                        "the 100% command has one name everywhere");
    auto* guides = fixture.viewer.findChild<QMenu*>("viewerGuidesMenu");
    for (const char* name :
         {"viewerSafeAreasAction", "viewerCentreCrossAction", "viewerThirdsAction", "viewerRulers",
          "viewerPixelGridAction", "viewerCustomSafeAreasAction"})
        expectations.expect(guides->actions().contains(
                                fixture.viewer.findChild<QAction*>(QString::fromLatin1(name))),
                            std::string{name} + " lives under View > Guides");
    // Footer toggles are the View menu's actions, so their checked states cannot disagree.
    auto* roi = fixture.viewer.findChild<QToolButton*>("viewerRoiToggle");
    auto* look = fixture.viewer.findChild<QToolButton*>("viewerLookToggle");
    expectations.expect(
        roi->defaultAction() != nullptr && view->actions().contains(roi->defaultAction()) &&
            look->defaultAction() != nullptr && view->actions().contains(look->defaultAction()),
        "the ROI and Look toggles are View's own actions");
    // A dropdown rebuilt with its signals blocked (as the display/view list is when the colour
    // configuration resolves) still shows every item in its mirror menu once that menu opens.
    {
        auto* displayView = fixture.viewer.findChild<ui::kit::KDropdown*>("viewerDisplayView");
        auto* displayViewMenu = fixture.viewer.findChild<QMenu*>("viewerDisplayViewMenu");
        {
            const QSignalBlocker blocker(displayView);
            displayView->clearItems();
            for (const char* name : {"sRGB", "Display P3", "Rec.1886"})
                displayView->addItem(QString::fromLatin1(name), QStringList{});
            displayView->setCurrentIndex(1);
        }
        Q_EMIT displayViewMenu->aboutToShow();
        const auto mirrored = displayViewMenu->actions();
        expectations.expect(mirrored.size() == 3 && mirrored[1]->isChecked(),
                            "a blocked display/view rebuild reaches its menu when it opens");
    }
    // The Zoom and Resolution submenus mirror their footer dropdowns.
    auto* zoomMenu = fixture.viewer.findChild<QMenu*>("viewerZoomMenu");
    Q_EMIT zoomMenu->aboutToShow();
    expectations.expect(zoomMenu->actions().size() == 6 &&
                            zoomMenu->actions()[3]->text() == QStringLiteral("100%"),
                        "View > Zoom offers the zoom dropdown's presets");
    zoomMenu->actions()[3]->trigger();
    expectations.expect(!fixture.viewer.viewTransformForTest().fitToWindow &&
                            fixture.viewer.zoomDropdownForTest()->currentIndex() == 3,
                        "a Zoom preset from the menu drives the footer dropdown");
    // Select names its target.
    auto* select = fixture.viewer.findChild<QMenu*>("viewerSelectMenu");
    Q_EMIT select->aboutToShow();
    expectations.expect(fixture.viewer.findChild<QAction*>("viewerSelectAllAction")->text() ==
                            QStringLiteral("Select All Layers"),
                        "Select names the layers it acts on");
    // Add: layers, then shapes, then Import and Add.
    auto* add = fixture.viewer.findChild<QMenu*>("viewerAddMenu");
    const auto entries = add->actions();
    expectations.expect(entries.size() == 12 && entries[2]->isSeparator() &&
                            entries[10]->isSeparator() &&
                            entries[11]->objectName() == QStringLiteral("viewerImportAndAddAction"),
                        "Add groups Solid/Text, the shapes, and Import and Add");
    // The canvas context menu reuses the same zoom actions and offers Add and Guides.
    fixture.viewer.findChild<QAction*>("viewerFitAction")->trigger();
    auto context = fixture.viewer.canvasContextMenuForTest(
        fixture.viewer.contentRectForTest().topLeft() + QPointF(4, 4));
    const auto items = context->actions();
    expectations.expect(items.contains(fixture.viewer.findChild<QAction*>("viewerFitAction")) &&
                            items.contains(actualSize) && items.contains(add->menuAction()) &&
                            items.contains(guides->menuAction()),
                        "the canvas menu shares View's zoom actions and offers Add and Guides");
    // A click on a layer offers it first.
    expectations.expect(fixture.session.addSolidLayer("Hit", {0.2, 0.3, 0.4, 1.0}),
                        "the context fixture adds a full-frame layer");
    fixture.session.clearSelection();
    toolReady(fixture, expectations);
    (void)fixture.viewer.grab();
    auto onLayer =
        fixture.viewer.canvasContextMenuForTest(fixture.viewer.canvasRectForTest().center());
    expectations.expect(onLayer->actions().size() > 0 &&
                            onLayer->actions().front()->objectName() ==
                                QStringLiteral("viewerContextSelectLayerAction"),
                        "a right-click on a layer offers to select it first");
    toolShutdown(fixture, expectations);
}
