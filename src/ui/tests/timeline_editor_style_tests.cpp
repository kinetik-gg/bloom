// Task U7 (issue #122): Kinetik restyle tests for TimelineEditor's track rows and transport,
// covering the decisions timeline_ruler_tests.cpp does not (that file owns the ruler/keyframe-panel
// interaction contract; this file owns the layer-row chrome and transport restyle). Offscreen,
// matching every other widget test in this suite.

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QString>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

namespace {

using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool near(const QColor& left, const QColor& right, const int tolerance) {
    return std::abs(left.red() - right.red()) <= tolerance &&
           std::abs(left.green() - right.green()) <= tolerance &&
           std::abs(left.blue() - right.blue()) <= tolerance;
}

bloom::runtime::TaskSchedulerConfig testSchedulerConfig() {
    return {.cpuWorkerCount = 1,
            .blockingIoWorkerCount = 1,
            .cpuQueueCapacity = 16,
            .blockingIoQueueCapacity = 4,
            .terminalHistoryCapacity = 32,
            .diagnosticsPerTask = 8,
            .groupRegistryCapacity = 8};
}

bloom::document::CompositionFormat smallFormat() {
    const auto format = bloom::document::CompositionFormat::create(4, 3);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

[[nodiscard]] bloom::core::RationalTime time(const std::int64_t numerator) {
    const auto value = bloom::core::RationalTime::create(numerator, 1);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

bloom::document::NewProject makeTestProject(std::string projectName) {
    return bloom::document::makeNewProject(std::move(projectName), "Main", time(10), smallFormat());
}

// Mirrors timeline_ruler_tests.cpp's own PipelineFixture/SessionFixture exactly -- no shared test
// fixture header exists in this suite (every widget test file in src/ui/tests owns a private copy,
// e.g. composition_projection_test.cpp, playback_controller_tests.cpp), so this duplication follows
// the established per-file idiom rather than inventing a new cross-file dependency.
struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions)) {
            std::abort();
        }
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             qualifiedProcessorProvider);
    }
};

struct SessionFixture final {
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::ui::CompositionSession session;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    bloom::ui::CompositionPreviewController controller;

    explicit SessionFixture(bloom::document::NewProject newProject)
        : document(std::move(newProject.project)), commands(document),
          session(document, commands, newProject.initialCompositionId),
          scheduler(testSchedulerConfig()), bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline) {}
};

void finishFixture(SessionFixture& fixture) {
    fixture.controller.beginShutdown();
    fixture.bridge.beginShutdown();
    QElapsedTimer timer;
    timer.start();
    while (!fixture.scheduler.isQuiescent() && timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

// Forces the tree's real item-widget/column geometry to materialize (setItemWidget() sizes are
// only assigned once the view actually lays itself out), the same "show a real top-level window,
// pump events" idiom playback_controller_tests.cpp's tree-focus test already uses.
void layoutTree(QWidget& host) {
    host.resize(720, 320);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// Decision 1's honesty rule: no user-facing layer-visibility command exists anywhere in
// src/commands (verified by reading operations.hpp), so the eye column must render a permanently
// dimmed, non-interactive icon with a tooltip that says exactly why -- never a toggle that would
// silently do nothing.
void testVisibilityColumnRendersDisabledWithHonestTooltip(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Visibility Column Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    ui::TimelineEditor editor(fixture.session, fixture.controller);
    auto* tree = editor.findChild<QTreeWidget*>("layerStackView");
    expectations.expect(tree != nullptr && tree->topLevelItemCount() == 1,
                        "the layer row exists");
    if (tree == nullptr || tree->topLevelItemCount() != 1) {
        finishFixture(fixture);
        return;
    }
    auto* row = tree->topLevelItem(0);

    const auto dimmedIcon = ui::kit::iconPixmap(
        ui::kit::IconId::Visible, ui::kit::Size::IconSmall,
        ui::kit::withOpacity(ui::kit::color(ui::kit::Color::Muted), ui::kit::kDisabledOpacity));
    const auto brightIcon = ui::kit::iconPixmap(ui::kit::IconId::Visible, ui::kit::Size::IconSmall,
                                                ui::kit::color(ui::kit::Color::Muted));
    expectations.expect(dimmedIcon.toImage() != brightIcon.toImage(),
                        "the fixture's own dimmed/bright renderings are genuinely different (test "
                        "sanity)");

    const auto rowIcon = row->icon(ui::TimelineEditor::kVisibilityColumn);
    expectations.expect(!rowIcon.isNull(), "the visibility column carries an icon");
    expectations.expect(
        rowIcon.pixmap(dimmedIcon.size()).toImage() == dimmedIcon.toImage(),
        "the visibility icon is the SAME permanently dimmed rendering, not the bright/active one");

    const auto toolTip = row->toolTip(ui::TimelineEditor::kVisibilityColumn);
    expectations.expect(!toolTip.isEmpty() && toolTip.contains(QStringLiteral("no command"),
                                                                Qt::CaseInsensitive),
                        "the tooltip honestly explains there is no visibility command yet");

    finishFixture(fixture);
}

// Decision 1: Blending shows "Normal" disabled, Parent shows "None" disabled -- one honest value,
// no fake choices, because neither feature exists.
void testBlendingAndParentColumnsAreDisabledPlaceholders(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Blending Parent Column Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutTree(host);

    auto* tree = editor->findChild<QTreeWidget*>("layerStackView");
    expectations.expect(tree != nullptr && tree->topLevelItemCount() == 1,
                        "the layer row exists");
    if (tree == nullptr || tree->topLevelItemCount() != 1) {
        finishFixture(fixture);
        return;
    }
    auto* row = tree->topLevelItem(0);

    auto* blending =
        qobject_cast<ui::kit::KDropdown*>(tree->itemWidget(row, ui::TimelineEditor::kBlendingColumn));
    auto* parent =
        qobject_cast<ui::kit::KDropdown*>(tree->itemWidget(row, ui::TimelineEditor::kParentColumn));
    expectations.expect(blending != nullptr && parent != nullptr,
                        "the Blending and Parent columns each carry a KDropdown");
    if (blending == nullptr || parent == nullptr) {
        finishFixture(fixture);
        return;
    }

    expectations.expect(!blending->isEnabled() && blending->currentText() == QStringLiteral("Normal"),
                        "Blending shows the one honest value \"Normal\", disabled");
    expectations.expect(!blending->toolTip().isEmpty(), "Blending's disabled state is explained");
    expectations.expect(!parent->isEnabled() && parent->currentText() == QStringLiteral("None"),
                        "Parent shows the one honest value \"None\", disabled");
    expectations.expect(!parent->toolTip().isEmpty(), "Parent's disabled state is explained");

    finishFixture(fixture);
}

// Decision 2: the lane bar's color comes from the ONE documented data-type-palette mapping
// (layerLaneColorToken() in composition_editors.cpp: DataComposition, for every layer kind that
// exists today).
void testLaneBarUsesTheDocumentedDataTypeColor(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Lane Bar Color Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addTextLayer(QStringLiteral("B"), QStringLiteral("Text"));

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutTree(host);

    auto* tree = editor->findChild<QTreeWidget*>("layerStackView");
    expectations.expect(tree != nullptr && tree->topLevelItemCount() == 2,
                        "both layer rows exist");
    if (tree == nullptr || tree->topLevelItemCount() != 2) {
        finishFixture(fixture);
        return;
    }

    const QColor expected = ui::kit::color(ui::kit::Color::DataComposition);
    for (int index = 0; index < 2; ++index) {
        auto* row = tree->topLevelItem(index);
        auto* bar = tree->itemWidget(row, ui::TimelineEditor::kLaneColumn);
        expectations.expect(bar != nullptr && bar->width() > 4 && bar->height() > 4,
                            "the lane bar widget exists with real geometry");
        if (bar == nullptr || bar->width() <= 4 || bar->height() <= 4) {
            continue;
        }
        const QImage image = bar->grab().toImage();
        const QColor sampled = image.pixelColor(bar->width() / 2, bar->height() / 2);
        expectations.expect(near(sampled, expected, 6),
                            "the lane bar's interior paints the documented DataComposition color, "
                            "for both a Solid and a Text layer alike (one uniform honest mapping)");
    }

    finishFixture(fixture);
}

// Decision 1: "selection = Accent inset edge per States" -- a 2px Accent border on the selected
// row, not a full fill (a fill would hide the row's own name/kind text).
void testSelectedRowPaintsAccentInsetEdgeNotAFill(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Selection Edge Test"));
    (void)fixture.session.addSolidLayer(QStringLiteral("A"), core::Color4d{0.2, 0.3, 0.4, 1.0});
    (void)fixture.session.addSolidLayer(QStringLiteral("B"), core::Color4d{0.2, 0.3, 0.4, 1.0});

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addWidget(editor);
    layoutTree(host);

    auto* tree = editor->findChild<QTreeWidget*>("layerStackView");
    expectations.expect(tree != nullptr && tree->topLevelItemCount() == 2, "both rows exist");
    if (tree == nullptr || tree->topLevelItemCount() != 2) {
        finishFixture(fixture);
        return;
    }
    // addSolidLayer selects the newly added layer, so row 1 ("B") is already selected; row 0 ("A")
    // is not.
    const auto selectedRect = tree->visualItemRect(tree->topLevelItem(1));
    const auto unselectedRect = tree->visualItemRect(tree->topLevelItem(0));
    expectations.expect(tree->topLevelItem(1)->isSelected() && !tree->topLevelItem(0)->isSelected(),
                        "row 1 is selected, row 0 is not (test precondition)");
    expectations.expect(selectedRect.height() > 4 && unselectedRect.height() > 4,
                        "both rows have real painted geometry");
    if (selectedRect.height() <= 4 || unselectedRect.height() <= 4) {
        finishFixture(fixture);
        return;
    }

    const QImage viewport = tree->viewport()->grab().toImage();
    const QColor accent = ui::kit::color(ui::kit::Color::Accent);
    const int sampleX = selectedRect.left() + 12;
    const QColor selectedTopEdge = viewport.pixelColor(sampleX, selectedRect.top() + 1);
    const QColor unselectedTopEdge =
        viewport.pixelColor(unselectedRect.left() + 12, unselectedRect.top() + 1);
    expectations.expect(near(selectedTopEdge, accent, 24),
                        "the selected row's top edge paints the Accent inset border");
    expectations.expect(!near(unselectedTopEdge, accent, 24),
                        "an unselected row's own top edge does NOT paint the Accent border");

    // The interior (well below the 2px edge) is the striped background, never a full Accent fill --
    // this is what "inset edge, not a fill" means concretely.
    const QColor selectedInterior =
        viewport.pixelColor(sampleX, selectedRect.top() + selectedRect.height() / 2);
    expectations.expect(!near(selectedInterior, accent, 24),
                        "the selected row's INTERIOR is not accent-filled -- only the inset edge is");

    finishFixture(fixture);
}

// Decision 5: the play/pause button's ICON swaps alongside its already-pinned text()/isChecked()
// contract (playback_controller_tests.cpp owns that contract byte-for-byte; this test adds the
// icon assertion on top of the SAME public API/idiom rather than duplicating the whole contract).
void testPlayPauseButtonIconSwapsWithState(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Play Pause Icon Test"));

    auto* editor = new ui::TimelineEditor(fixture.session, fixture.controller);
    auto* button = editor->findChild<QToolButton*>("playPauseButton");
    expectations.expect(button != nullptr, "the play/pause button is reachable by name");
    if (button == nullptr) {
        delete editor;
        finishFixture(fixture);
        return;
    }

    const auto playIcon = ui::kit::icon(ui::kit::IconId::Play, ui::kit::Size::IconMedium);
    const auto pauseIcon = ui::kit::icon(ui::kit::IconId::Pause, ui::kit::Size::IconMedium);
    const auto size = QSize(ui::kit::px(ui::kit::Size::IconMedium),
                            ui::kit::px(ui::kit::Size::IconMedium));
    expectations.expect(
        button->icon().pixmap(size).toImage() == playIcon.pixmap(size).toImage(),
        "the button starts showing the Play glyph");

    button->click();
    expectations.expect(button->isChecked() && button->text() == QStringLiteral("Pause"),
                        "the existing text()/isChecked() contract still flips on click");
    expectations.expect(
        button->icon().pixmap(size).toImage() == pauseIcon.pixmap(size).toImage(),
        "clicking swaps the icon to Pause alongside the text");

    button->click();
    expectations.expect(
        button->icon().pixmap(size).toImage() == playIcon.pixmap(size).toImage(),
        "clicking again swaps the icon back to Play");

    delete editor;
    finishFixture(fixture);
}

// Non-goal guard (decision 5): the loop indicator is a status glyph, never a clickable control --
// playback always loops with no command to disable it, so a QToolButton here would dishonestly
// imply a toggle that does not exist.
void testLoopIndicatorIsNonInteractiveAndHonest(Expectations& expectations) {
    using namespace bloom;
    SessionFixture fixture(makeTestProject("Loop Indicator Test"));

    ui::TimelineEditor editor(fixture.session, fixture.controller);
    auto* indicator = editor.findChild<QLabel*>("timelineLoopIndicator");
    expectations.expect(indicator != nullptr, "the loop indicator exists");
    if (indicator == nullptr) {
        finishFixture(fixture);
        return;
    }
    expectations.expect(editor.findChild<QToolButton*>("timelineLoopIndicator") == nullptr,
                        "the loop indicator is a QLabel, never a clickable QToolButton");
    expectations.expect(!indicator->pixmap().isNull(), "the loop indicator carries a glyph");
    expectations.expect(indicator->toolTip().contains(QStringLiteral("loop"), Qt::CaseInsensitive),
                        "the tooltip honestly names the always-on looping behavior");

    finishFixture(fixture);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testVisibilityColumnRendersDisabledWithHonestTooltip(expectations);
    testBlendingAndParentColumnsAreDisabledPlaceholders(expectations);
    testLaneBarUsesTheDocumentedDataTypeColor(expectations);
    testSelectedRowPaintsAccentInsetEdgeNotAFill(expectations);
    testPlayPauseButtonIconSwapsWithState(expectations);
    testLoopIndicatorIsNonInteractiveAndHonest(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
