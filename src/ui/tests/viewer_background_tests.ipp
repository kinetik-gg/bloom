// Included inside viewer_editor_tests.cpp's anonymous namespace (after viewer_chrome_tests.ipp):
// the Viewer's two backdrops (docs/ux/ui-grammar.md, "Viewer canvas"). View > Background is the
// surround outside the composition frame only; inside the frame the Viewer shows the composition's
// own background colour, or the checkerboard while the Transparency Grid is on. Both are
// presentation: a background edit re-presents the frame it already has and never re-evaluates.

void clearViewerBackdropSettings() {
    QSettings().remove("viewer/background");
    QSettings().remove("viewer/transparency-grid");
}

[[nodiscard]] bool setCompositionBackground(ViewerFixture& fixture, const bloom::core::Color4d color) {
    bloom::commands::Transaction edit("Background", fixture.session.snapshot().revision());
    edit.emplace<bloom::commands::SetCompositionBackgroundColor>(fixture.session.compositionId(),
                                                                 color);
    return fixture.session.executeTransaction(std::move(edit)).succeeded();
}

// The colour the frame must show for an authored background: the chip's own 8-bit quantization.
[[nodiscard]] QColor expectedFrameColor(const bloom::core::Color4d color) {
    return bloom::ui::kit::KColor{static_cast<float>(color.red), static_cast<float>(color.green),
                                  static_cast<float>(color.blue), 1.0F}
        .toQColor();
}

[[nodiscard]] QColor presentColor(const bloom::render::GpuPresentColor color) {
    const auto byteOf = [](const float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return QColor(byteOf(color.red), byteOf(color.green), byteOf(color.blue));
}

struct BackdropProbe final {
    QImage image;
    QRectF content;
    QRectF canvas;
    [[nodiscard]] QColor at(const QPointF point) const {
        return image.pixelColor(QPointF(point * image.devicePixelRatio()).toPoint());
    }
    [[nodiscard]] QColor corner() const { return at(content.topLeft() + QPointF(3, 3)); }
    [[nodiscard]] QColor farCorner() const { return at(content.bottomRight() - QPointF(3, 3)); }
    [[nodiscard]] QColor centre() const { return at(canvas.center()); }
};

[[nodiscard]] BackdropProbe probeBackdrop(bloom::ui::ViewerEditor& viewer) {
    QCoreApplication::processEvents();
    return BackdropProbe{viewer.grab().toImage(), viewer.contentRectForTest(),
                         viewer.canvasRectForTest()};
}

void testViewerSurroundNeverDrawsInsideTheFrame(Expectations& expectations) {
    using namespace bloom;
    clearViewerBackdropSettings();
    {
        ViewerFixture fixture(makeTestProject("Surround only"));
        fixture.viewer.show();
        expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                            "the surround fixture reaches a ready frame");
        expectations.expect(fixture.viewer.backgroundForTest() == ui::ViewerBackground::Solid &&
                                !fixture.viewer.transparencyGridForTest(),
                            "the defaults are the Solid surround with the Transparency Grid off");
        const core::Color4d authored{0.8, 0.2, 0.4, 1.0};
        expectations.expect(setCompositionBackground(fixture, authored),
                            "the composition background edit commits");
        const QColor frame = expectedFrameColor(authored);
        auto* background =
            fixture.viewer.findChild<ui::kit::KDropdown*>("viewerBackgroundDropdown");
        expectations.expect(background != nullptr && background->count() == 4 &&
                                background->itemText(0) == QStringLiteral("Solid") &&
                                background->itemText(1) == QStringLiteral("Gray") &&
                                background->itemText(2) == QStringLiteral("Black") &&
                                background->itemText(3) == QStringLiteral("White"),
                            "View > Background offers Solid, Gray, Black and White, no checker");
        if (background == nullptr)
            return reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler,
                                   expectations);
        struct Choice final {
            ui::ViewerBackground mode;
            QColor surround;
            const char* name;
        };
        const std::array<Choice, 4> choices{{
            {ui::ViewerBackground::Grey, ui::kit::color(ui::kit::Color::CanvasGrey), "Gray"},
            {ui::ViewerBackground::Black, QColor(Qt::black), "Black"},
            {ui::ViewerBackground::Solid, ui::kit::color(ui::kit::Color::Canvas), "Solid"},
            {ui::ViewerBackground::White, QColor(Qt::white), "White"},
        }};
        for (const auto& choice : choices) {
            background->setCurrentIndex(static_cast<int>(choice.mode));
            const auto probe = probeBackdrop(fixture.viewer);
            const std::string label = std::string(" (") + choice.name + ")";
            expectations.expect(fixture.viewer.backgroundForTest() == choice.mode &&
                                    probe.corner() == choice.surround &&
                                    probe.farCorner() == choice.surround,
                                "the surround fills the content area around the frame" + label);
            expectations.expect(probe.centre() == frame,
                                "inside the frame the composition background shows, never the "
                                "surround" +
                                    label);
            const auto request = fixture.viewer.buildResidentPresentRequestForTest();
            expectations.expect(presentColor(request.surroundColor) == choice.surround &&
                                    presentColor(request.frameColor) == frame &&
                                    !request.transparencyGrid,
                                "the GPU present request carries the same surround and frame "
                                "colors as the CPU paint" +
                                    label);
            expectations.expect(QSettings().value("viewer/background").toString() ==
                                    QLatin1String(choice.name),
                                "the surround choice is persisted by name" + label);
        }
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
    }
    {
        ViewerFixture restored(makeTestProject("Surround restore"));
        expectations.expect(restored.viewer.backgroundForTest() == ui::ViewerBackground::White,
                            "a new viewer restores the persisted surround");
        reachQuiescence(restored.controller, restored.bridge, restored.scheduler, expectations);
    }
    clearViewerBackdropSettings();
}

void testTransparencyGridSharesOneActionAndPersists(Expectations& expectations) {
    using namespace bloom;
    clearViewerBackdropSettings();
    {
        ViewerFixture fixture(makeTestProject("Transparency grid"));
        fixture.viewer.resize(900, 650);
        fixture.viewer.show();
        expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                            "the grid fixture reaches a ready frame");
        const core::Color4d authored{0.1, 0.5, 0.3, 1.0};
        expectations.expect(setCompositionBackground(fixture, authored),
                            "the grid fixture sets a composition background");
        auto* action = fixture.viewer.findChild<QAction*>("viewerTransparencyGridAction");
        auto* footer = bloom::ui::test::footer(fixture.viewer);
        auto* toggle = footer->findChild<QToolButton*>("viewerTransparencyGridToggle");
        auto* view = fixture.viewer.findChild<QMenu*>("viewerViewMenu");
        expectations.expect(action != nullptr && action->isCheckable() && !action->isChecked() &&
                                toggle != nullptr && toggle->defaultAction() == action &&
                                view != nullptr && view->actions().contains(action),
                            "the footer toggle and View > Transparency Grid are one action, off "
                            "by default");
        if (action == nullptr || toggle == nullptr)
            return reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler,
                                   expectations);
        expectations.expect(probeBackdrop(fixture.viewer).centre() == expectedFrameColor(authored),
                            "with the grid off the frame shows the composition background");
        toggle->click();
        expectations.expect(action->isChecked() && fixture.viewer.transparencyGridForTest(),
                            "the footer toggle turns the grid on through the shared action");
        const auto probe = probeBackdrop(fixture.viewer);
        const int cell = ui::kit::px(ui::kit::Size::ViewerChecker);
        const QColor light = ui::kit::color(ui::kit::Color::ViewerCheckerLight);
        const QColor dark = ui::kit::color(ui::kit::Color::ViewerCheckerDark);
        const QColor a = probe.centre();
        const QColor b = probe.at(probe.canvas.center() + QPointF(cell, 0));
        expectations.expect((a == light || a == dark) && (b == light || b == dark) && a != b,
                            "the grid replaces the composition background inside the frame");
        expectations.expect(probe.corner() == ui::kit::color(ui::kit::Color::Canvas),
                            "the grid never reaches the surround");
        const auto request = fixture.viewer.buildResidentPresentRequestForTest();
        expectations.expect(request.transparencyGrid &&
                                presentColor(request.checkerColorA) == light &&
                                presentColor(request.checkerColorB) == dark &&
                                presentColor(request.frameColor) == expectedFrameColor(authored),
                            "the GPU present request carries the grid and the same checker "
                            "tokens as the CPU paint");
        expectations.expect(QSettings().value("viewer/transparency-grid").toBool(),
                            "the grid is persisted under viewer/transparency-grid");
        // A narrow footer collapses the toggle into View options, as the same action.
        auto* overflow = footer->findChild<QToolButton*>("viewerFooterOverflowButton");
        footer->resize(320, ui::kit::px(ui::kit::Size::FooterRow));
        (void)footer->grab();
        QCoreApplication::processEvents();
        expectations.expect(toggle->isHidden() && overflow != nullptr &&
                                overflow->menu()->actions().contains(action),
                            "a collapsed Transparency Grid is offered in View options");
        footer->resize(1920, ui::kit::px(ui::kit::Size::FooterRow));
        (void)footer->grab();
        reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
    }
    {
        ViewerFixture restored(makeTestProject("Transparency grid restore"));
        auto* action = restored.viewer.findChild<QAction*>("viewerTransparencyGridAction");
        expectations.expect(restored.viewer.transparencyGridForTest() && action != nullptr &&
                                action->isChecked(),
                            "a new viewer restores the persisted grid, action checked");
        ui::ApplicationPreferences preferences;
        preferences.viewerTransparencyGrid = false;
        restored.viewer.applyApplicationPreferences(preferences);
        expectations.expect(!restored.viewer.transparencyGridForTest() && !action->isChecked(),
                            "the Preferences window drives the same action");
        reachQuiescence(restored.controller, restored.bridge, restored.scheduler, expectations);
    }
    clearViewerBackdropSettings();
}

// A background colour edit is presentation: one non-render-affecting undoable step that repaints
// the frame the Viewer already has, with no new preparation, and the displayed frame stays current.
void testCompositionBackgroundEditRepresentsWithoutEvaluating(Expectations& expectations) {
    using namespace bloom;
    clearViewerBackdropSettings();
    ViewerFixture fixture(makeTestProject("Background re-present"));
    expectations.expect(fixture.session.addSolidLayer("Half", core::Color4d{0.9, 0.9, 0.1, 1.0}) &&
                            fixture.session.setSelectedOpacity(0.5),
                        "the re-present fixture adds a half-transparent layer");
    fixture.session.clearSelection(); // no selection handles over the sampled centre
    fixture.viewer.show();
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the re-present fixture reaches a ready frame");
    const auto before = probeBackdrop(fixture.viewer).centre();
    const auto frame = fixture.controller.state().frame;
    const int preparations = fixture.pipeline.preparations.load();
    const std::size_t undoDepth = fixture.commands.size();

    // The Composition Settings commit for a background-only change.
    auto settings = commands::compositionSettingsOf(*fixture.session.composition());
    settings.background = core::Color4d{0.1, 0.3, 0.9, 1.0};
    expectations.expect(ui::applyCompositionSettings(fixture.session,
                                                     fixture.session.compositionId(), settings),
                        "a background-only Composition Settings edit commits");
    expectations.expect(fixture.commands.size() == undoDepth + 1,
                        "the edit is one undoable step");
    const auto after = probeBackdrop(fixture.viewer).centre();
    // Let any (wrongly) scheduled evaluation run before counting.
    const auto settle = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < settle)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    expectations.expect(after != before && after.blue() > before.blue(),
                        "the frame re-presents with the new background immediately (" +
                            before.name().toStdString() + " -> " + after.name().toStdString() +
                            ")");
    expectations.expect(fixture.pipeline.preparations.load() == preparations &&
                            fixture.controller.state().frame == frame,
                        "a background edit re-presents the cached frame without re-evaluating");
    expectations.expect(fixture.viewer.displayedPreviewState().freshness ==
                            ui::FrameFreshness::Current,
                        "the re-presented frame is current");
    expectations.expect(fixture.session.undo() && probeBackdrop(fixture.viewer).centre() == before,
                        "undo restores the previous background");
    expectations.expect(fixture.pipeline.preparations.load() == preparations,
                        "undo re-presents without re-evaluating too");

    // Any other change still goes through the whole-settings command.
    settings = commands::compositionSettingsOf(*fixture.session.composition());
    settings.name = "Renamed";
    expectations.expect(ui::applyCompositionSettings(fixture.session,
                                                     fixture.session.compositionId(), settings) &&
                            fixture.session.composition()->name() == "Renamed",
                        "a non-background settings edit still commits");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// Behind a half-transparent layer the image composites over the composition background, in the
// same display space the GPU present composites in.
void testSemiTransparentLayerCompositesOverTheBackground(Expectations& expectations) {
    using namespace bloom;
    clearViewerBackdropSettings();
    ViewerFixture fixture(makeTestProject("Half over background"));
    expectations.expect(fixture.session.addSolidLayer("Half", core::Color4d{0.5, 0.5, 0.5, 1.0}) &&
                            fixture.session.setSelectedOpacity(0.5),
                        "the composite fixture adds a half-transparent gray layer");
    fixture.session.clearSelection(); // no selection handles over the sampled centre
    fixture.viewer.show();
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the composite fixture reaches a ready frame");
    expectations.expect(setCompositionBackground(fixture, core::Color4d{1.0, 0.0, 0.0, 1.0}),
                        "a red background commits");
    const QColor overRed = probeBackdrop(fixture.viewer).centre();
    expectations.expect(setCompositionBackground(fixture, core::Color4d{0.0, 0.0, 1.0, 1.0}),
                        "a blue background commits");
    const QColor overBlue = probeBackdrop(fixture.viewer).centre();
    expectations.expect(overRed.red() > overRed.blue() && overBlue.blue() > overBlue.red() &&
                            overRed != QColor(Qt::red) && overBlue != QColor(Qt::blue),
                        "a half-transparent layer composites over the composition background (" +
                            overRed.name().toStdString() + ", " + overBlue.name().toStdString() +
                            ")");
    expectations.expect(std::abs(overRed.green() - overBlue.green()) <= 1,
                        "the layer's own contribution is the same over either background");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}

// The pre-split "Checkerboard" background keeps showing a checker: Solid surround plus the grid.
void testLegacyCheckerboardSettingMigrates(Expectations& expectations) {
    using namespace bloom;
    clearViewerBackdropSettings();
    QSettings().setValue("viewer/background", QStringLiteral("Checkerboard"));
    {
        ViewerFixture legacy(makeTestProject("Legacy checker"));
        expectations.expect(legacy.viewer.backgroundForTest() == ui::ViewerBackground::Solid &&
                                legacy.viewer.transparencyGridForTest(),
                            "a stored Checkerboard reads as Solid with the Transparency Grid on");
        reachQuiescence(legacy.controller, legacy.bridge, legacy.scheduler, expectations);
    }
    QSettings().setValue("viewer/transparency-grid", false);
    {
        ViewerFixture later(makeTestProject("Legacy checker, later grid off"));
        expectations.expect(later.viewer.backgroundForTest() == ui::ViewerBackground::Solid &&
                                !later.viewer.transparencyGridForTest(),
                            "a grid choice made since is never overridden by the old value");
        reachQuiescence(later.controller, later.bridge, later.scheduler, expectations);
    }
    clearViewerBackdropSettings();
    QSettings().setValue("viewer/background", QStringLiteral("nonsense"));
    {
        ViewerFixture invalid(makeTestProject("Background fallback"));
        expectations.expect(invalid.viewer.backgroundForTest() == ui::ViewerBackground::Solid &&
                                !invalid.viewer.transparencyGridForTest(),
                            "an unrecognized surround falls back to Solid, grid off");
        reachQuiescence(invalid.controller, invalid.bridge, invalid.scheduler, expectations);
    }
    clearViewerBackdropSettings();
}

// A composition with no layers shows just its frame and background: no invitation text.
void testEmptyCompositionShowsNoLayerInvitation(Expectations& expectations) {
    using namespace bloom;
    clearViewerBackdropSettings();
    ViewerFixture fixture(makeTestProject("Empty composition"));
    fixture.viewer.show();
    expectations.expect(fixture.session.composition() != nullptr &&
                            fixture.session.composition()->graph().layerOutputs().empty(),
                        "the fixture has an active composition with no layers");
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the empty composition reaches a ready frame");
    const core::Color4d authored{0.25, 0.55, 0.75, 1.0};
    expectations.expect(setCompositionBackground(fixture, authored),
                        "the empty composition takes a background");
    const auto probe = probeBackdrop(fixture.viewer);
    const QColor frame = expectedFrameColor(authored);
    const QRectF inner = probe.canvas.adjusted(probe.canvas.width() / 4, probe.canvas.height() / 4,
                                               -probe.canvas.width() / 4,
                                               -probe.canvas.height() / 4);
    bool uniform = true;
    for (int row = 0; row <= 8; ++row)
        for (int column = 0; column <= 8; ++column)
            uniform = uniform && probe.at(QPointF(inner.left() + inner.width() * column / 8.0,
                                                  inner.top() + inner.height() * row / 8.0)) ==
                                     frame;
    expectations.expect(uniform, "the empty frame is the composition background, with no text");
    auto* invitation = fixture.viewer.findChild<QWidget*>("viewerEmptyState");
    expectations.expect(invitation == nullptr || invitation->isHidden(),
                        "the no-composition invitation is not shown for an existing composition");
    reachQuiescence(fixture.controller, fixture.bridge, fixture.scheduler, expectations);
}
