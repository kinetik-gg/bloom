#include "timeline_property_rows.hpp"
#include <QDir>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <bloom/commands/command_stack.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace bloom;
void require(const bool value, const char* message) {
    if (!value) {
        std::cerr << "Live value edit: " << message << '\n';
        std::abort();
    }
}
template <typename Predicate> void waitFor(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        std::this_thread::yield();
    }
    require(predicate(), "asynchronous operation completes within five seconds");
}
template <typename Value> Value checked(std::optional<Value> value) {
    if (!value)
        std::abort();
    return *value;
}
struct Fixture {
    document::NewProject project =
        document::makeNewProject("Live values", "Main", core::RationalTime::fromInteger(10),
                                 checked(document::CompositionFormat::create(64, 36)));
    document::Document document{std::move(project.project)};
    commands::CommandStack history{document};
    ui::CompositionSession session{document, history, project.initialCompositionId};
    Fixture() {
        require(session.addSolidLayer("Solid", {0.2, 0.3, 0.4, 1}), "add fixture layer");
        history.clear();
    }
    document::ParameterId position() const {
        return session.parameterForSelection(document::kPositionParameterRole)->id;
    }
};

void sessionEdits() {
    Fixture fixture;
    auto& session = fixture.session;
    const auto id = fixture.position();
    const auto revision = session.snapshot().revision();
    const auto base = session.effectiveVec2Value(id);
    require(session.beginValueEdit(id), "begin position edit");
    require(session.updateValueEdit(document::Vec2d{30, 40}), "update position live");
    require(session.updateValueEdit(document::Vec2d{50, 60}), "replace position live");
    require(session.snapshot().revision() == revision && fixture.history.size() == 0,
            "live updates do not transact");
    require(std::get<document::Vec2d>(checked(session.liveValue(id))) == document::Vec2d{50, 60},
            "live readback returns the override");
    require(session.commitValueEdit() && fixture.history.size() == 1,
            "one edit produces exactly one history entry");
    require(session.undo() && session.effectiveVec2Value(id) == base, "undo restores base");
    fixture.history.clear();
    require(session.beginValueEdit(id), "begin cancelled edit");
    require(session.updateValueEdit(document::Vec2d{70, 80}), "update cancelled edit");
    session.cancelValueEdit();
    require(fixture.history.size() == 0 && session.effectiveVec2Value(id) == base,
            "cancel restores readback without history");
    require(session.beginValueEdit(id) && session.commitValueEdit() && fixture.history.size() == 0,
            "unchanged edit creates no history");
    require(session.toggleKeyframe(id, document::AnimationComponent::X, session.currentTime()),
            "animate X only");
    require(session.setCurrentTime(core::RationalTime::fromInteger(1)), "advance time");
    fixture.history.clear();
    require(session.beginValueEdit(id, document::AnimationComponent::X), "edit animated component");
    require(session.updateValueEdit(123.0), "update component scalar");
    require(checked(session.effectiveVec2Value(id)).y == checked(base).y,
            "untouched component stays exact");
    require(session.commitValueEdit() && fixture.history.size() == 1, "component commits one key");
    require(
        session.keyframeDiamondState(id, document::AnimationComponent::Y, session.currentTime()) ==
            ui::KeyframeDiamondState::AnimatedWithoutKey,
        "component edit never keys the other axis");
    fixture.history.clear();
    require(session.beginValueEdit(id) && session.updateValueEdit(document::Vec2d{1, 2}),
            "start edit before time invalidation");
    require(session.setCurrentTime(core::RationalTime::fromInteger(2)) &&
                !session.valueEditActive(),
            "time change cancels frozen edit");
    require(fixture.history.size() == 0, "invalidation adds no history");
}

void fieldsShareLiveValues() {
    Fixture fixture;
    auto& session = fixture.session;
    ui::PropertiesEditor properties(session);
    ui::NodeGraphEditor nodes(session);
    properties.resize(500, 900);
    nodes.resize(800, 600);
    properties.show();
    nodes.show();
    const auto* node = session.selectedNode();
    require(node != nullptr, "selected layer exposes its boundary node");
    auto* property = properties.findChild<ui::kit::KValueField*>("positionXEditor");
    auto* card = qobject_cast<ui::kit::KValueField*>(
        nodes.graphScene()->nodeFieldForTest(node->id, "nodePositionXEditor"));
    require(property && card, "Properties and node-card position fields exist");
    ui::TimelinePropertyRow timeline(session, nullptr);
    ui::TimelineLayerEntry entry;
    entry.layerId = checked(session.selection().contextualLayer);
    entry.parameterId = fixture.position();
    entry.rowKind = ui::TimelineLayerEntry::Kind::Component;
    entry.component = document::AnimationComponent::X;
    entry.name = "X";
    entry.role = document::kPositionParameterRole;
    timeline.bind(entry);
    timeline.show();
    auto* timelineField = timeline.findChild<ui::kit::KValueField*>("timelinePropertyValue");
    require(timelineField != nullptr, "timeline component field exists");
    const double base = property->value();
    QTest::keyClick(property, Qt::Key_Return);
    QTest::keyClicks(property->lineEdit(), "123");
    require(property->value() == 123 && card->value() == 123 && timelineField->value() == 123 &&
                checked(session.effectiveVec2Value(fixture.position())).x == 123,
            "typed value is projected on all three surfaces before Enter");
    require(fixture.history.size() == 0, "typing has no history while unfinished");
    QTest::keyClick(property->lineEdit(), Qt::Key_Escape);
    require(property->value() == base && card->value() == base && fixture.history.size() == 0,
            "Escape restores both fields without a transaction");
    QTest::keyClick(property, Qt::Key_Return);
    QTest::keyClicks(property->lineEdit(), "42");
    QTest::keyClick(property->lineEdit(), Qt::Key_Return);
    require(fixture.history.size() == 1 && card->value() == 42,
            "Enter commits all typed changes once");
    fixture.history.clear();
    const QPoint start = card->rect().center();
    QTest::mousePress(card, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, start + QPoint(20, 0),
                     card->mapToGlobal(start + QPoint(20, 0)), Qt::NoButton, Qt::LeftButton,
                     Qt::ControlModifier);
    QApplication::sendEvent(card, &move);
    require(card->isScrubbing() && property->value() == card->value() && card->value() == 44,
            "node scrub immediately refreshes Properties with the fine modifier");
    require(fixture.history.size() == 0, "node drag has not committed");
    QTest::mouseRelease(card, Qt::LeftButton, Qt::ControlModifier, start + QPoint(20, 0));
    require(fixture.history.size() == 1, "node scrub release commits once");
    fixture.history.clear();
    QTest::keyPress(property, Qt::Key_Up);
    require(card->value() == property->value() && fixture.history.size() == 0,
            "arrow press is live before key release");
    QTest::keyRelease(property, Qt::Key_Up);
    require(fixture.history.size() == 1, "arrow release commits once");

    fixture.history.clear();
    QTest::keyClick(timelineField, Qt::Key_Return);
    QTest::keyClicks(timelineField->lineEdit(), "88");
    require(property->value() == 88 && card->value() == 88 && fixture.history.size() == 0,
            "timeline typing updates Properties and node card without committing");
    QTest::keyClick(timelineField->lineEdit(), Qt::Key_Escape);
    require(timelineField->value() == property->value() && fixture.history.size() == 0,
            "timeline Escape cancels without history");
    QTest::keyClick(timelineField, Qt::Key_Return);
    QTest::keyClicks(timelineField->lineEdit(), "91");
    QTest::keyClick(timelineField->lineEdit(), Qt::Key_Return);
    require(property->value() == 91 && card->value() == 91 && fixture.history.size() == 1,
            "timeline Enter commits exactly once");

    auto* chip = properties.findChild<ui::kit::KColorChip*>("propertiesSolidColorChip");
    require(chip != nullptr, "Solid chip exists");
    waitFor([&] { return static_cast<bool>(session.colorConverter()); });
    fixture.history.clear();
    chip->openPicker();
    auto* picker = chip->picker();
    const auto at = picker->svSquareRect().center().toPoint();
    QTest::mousePress(picker, Qt::LeftButton, Qt::NoModifier, at);
    require(session.valueEditActive() && fixture.history.size() == 0,
            "picker drag updates an override without transacting");
    QTest::mouseRelease(picker, Qt::LeftButton, Qt::NoModifier, at);
    require(!session.valueEditActive() && fixture.history.size() == 1,
            "picker release commits exactly once");
    const auto color = session.effectiveColorValue(document::kSolidColorParameterRole);
    fixture.history.clear();
    QTest::mousePress(picker, Qt::LeftButton, Qt::NoModifier, at + QPoint(10, 10));
    QTest::keyClick(picker, Qt::Key_Escape);
    require(fixture.history.size() == 0 &&
                session.effectiveColorValue(document::kSolidColorParameterRole) == color,
            "picker Escape cancels without history");
}

void interactiveBeforeCommit() {
    Fixture fixture;
    runtime::NodeDefinitionRegistry definitions;
    require(runtime::registerBuiltInNodeDefinitions(definitions), "register definitions");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer display;
    runtime::QualifiedDisplayProcessorProvider provider;
    const auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, display, provider);
    runtime::TaskScheduler scheduler({.cpuWorkerCount = 1, .blockingIoWorkerCount = 1});
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1000});
    std::atomic<bool> slowFrames{false};
    std::mutex mutex;
    std::vector<runtime::SnapshotParameterOverride> observed;
    ui::CompositionPreviewController controller(
        fixture.session, scheduler, bridge,
        [&](const auto& snapshot, const auto& identity, auto limit, const auto& overrides,
            auto& context) {
            {
                const std::lock_guard lock(mutex);
                observed = overrides;
            }
            if (slowFrames.load())
                std::this_thread::sleep_for(std::chrono::milliseconds{25});
            return pipeline(snapshot, identity, limit, overrides, context);
        });
    const auto ready = [&] { return controller.state().activity == ui::PreviewActivity::Ready; };
    waitFor(ready);
    const auto revision = fixture.session.snapshot().revision();
    QElapsedTimer latency;
    latency.start();
    require(fixture.session.beginValueEdit(fixture.position()), "begin preview edit");
    require(fixture.session.updateValueEdit(document::Vec2d{8, 9}), "update preview edit");
    require(controller.state().taskId.has_value(), "first value preview submits immediately");
    waitFor(ready);
    require(latency.elapsed() < 500, "live frame latency stays under generous offscreen bound");
    {
        const std::lock_guard lock(mutex);
        require(observed.size() == 1 && observed[0].parameterId == fixture.position() &&
                    std::get<document::Vec2d>(observed[0].value) == document::Vec2d{8, 9},
                "Interactive worker receives override before commit");
    }
    const auto tasks = scheduler.snapshots();
    const auto task =
        std::ranges::find(tasks, checked(controller.state().taskId), &runtime::TaskSnapshot::id);
    require(task != tasks.end() && task->priority == runtime::TaskPriority::Interactive,
            "live request has Interactive priority");
    require(fixture.history.size() == 0 && fixture.session.snapshot().revision() == revision,
            "rendered live frame precedes every transaction");
    require(fixture.session.commitValueEdit() && fixture.history.size() == 1,
            "rendered edit commits once");
    waitFor(ready);
    const auto previousFrame = controller.state().frame;
    const auto historySize = fixture.history.size();
    slowFrames.store(true);
    require(fixture.session.beginValueEdit(fixture.position()), "begin continuous input");
    QTimer updates;
    int value = 10;
    QObject::connect(&updates, &QTimer::timeout, &updates, [&] {
        require(fixture.session.updateValueEdit(document::Vec2d{static_cast<double>(value++), 9}),
                "continuous update remains session-only");
    });
    updates.start(2);
    waitFor([&] { return controller.state().frame != previousFrame; });
    require(updates.isActive() && fixture.history.size() == historySize,
            "completed live frames present during continuous newer input without a transaction");
    updates.stop();
    slowFrames.store(false);
    fixture.session.cancelValueEdit();
    waitFor(ready);
    controller.beginShutdown();
    bridge.beginShutdown();
    waitFor([&] { return scheduler.isQuiescent(); });
}

// GitHub #197: the timeline's expanded RGBA numeric rows obey the same display/reference boundary
// Properties already implements. This pin fails against the old adapter, which showed and committed
// the raw stored reference numbers.
void timelineColorBoundary() {
    Fixture fixture;
    auto& session = fixture.session;
    waitFor([&] { return static_cast<bool>(session.colorConverter()); });
    const auto* parameter = session.parameterForSelection(document::kSolidColorParameterRole);
    require(parameter != nullptr, "the solid layer exposes its colour parameter");
    const auto parameterId = parameter->id;
    const auto layer = checked(session.selection().contextualLayer);

    ui::PropertiesEditor properties(session);
    properties.resize(500, 900);
    properties.show();
    QCoreApplication::processEvents();
    auto* propertyRed = properties.findChild<ui::kit::KValueField*>("solidColorRedEditor");
    require(propertyRed != nullptr, "the Properties red field exists");

    // The parent colour row shows the swatch (already display-correct); binding it authors nothing
    // and does not alter the stored numbers.
    const auto revision = session.snapshot().revision();
    ui::TimelinePropertyRow parent(session, nullptr);
    ui::TimelineLayerEntry parentEntry;
    parentEntry.layerId = layer;
    parentEntry.parameterId = parameterId;
    parentEntry.rowKind = ui::TimelineLayerEntry::Kind::Parameter;
    parentEntry.depth = 2;
    parentEntry.name = "Color";
    parentEntry.role = document::kSolidColorParameterRole;
    parent.bind(parentEntry);
    parent.show();
    auto* parentChip = parent.findChild<ui::kit::KColorChip*>("timelinePropertyColor");
    require(session.snapshot().revision() == revision && fixture.history.size() == 0 &&
                parentChip != nullptr && parentChip->isEnabled(),
            "binding the timeline colour parent row authors nothing and keeps the swatch enabled");

    // An expanded component row shows the same normalized display red as Properties for the same
    // parameter and no "reference" suffix.
    ui::TimelinePropertyRow row(session, nullptr);
    ui::TimelineLayerEntry entry;
    entry.layerId = layer;
    entry.parameterId = parameterId;
    entry.rowKind = ui::TimelineLayerEntry::Kind::Component;
    entry.depth = 3;
    entry.component = document::AnimationComponent::Red;
    entry.name = "R";
    entry.role = document::kSolidColorParameterRole;
    row.bind(entry);
    row.show();
    auto* field = row.findChild<ui::kit::KValueField*>("timelinePropertyValue");
    require(field != nullptr, "the timeline component field exists");
    require(std::abs(field->value() - propertyRed->value()) < 1e-6 && field->unit().isEmpty(),
            "in-range timeline red matches the Properties display number with no suffix");

    // Committing a display number converts through the session converter; untouched channels stay
    // bit-exact and undo is one coherent edit.
    const auto base = checked(session.constantColorValue(parameterId));
    const auto converter = session.colorConverter(document::kSolidColorParameterSchemaKey);
    const auto baseDisplay = checked(
        ui::kit::KColor::fromRgba(static_cast<float>(base.red), static_cast<float>(base.green),
                                  static_cast<float>(base.blue), static_cast<float>(base.alpha),
                                  ui::kit::ColorSpace::Reference)
            .converted(ui::kit::ColorSpace::Display, converter));
    QTest::keyClick(field, Qt::Key_Return);
    QTest::keyClicks(field->lineEdit(), "0.5");
    QTest::keyClick(field->lineEdit(), Qt::Key_Return);
    const auto committed = checked(session.constantColorValue(parameterId));
    const auto expected =
        checked(ui::kit::KColor::fromRgba(0.5F, baseDisplay.green, baseDisplay.blue,
                                          baseDisplay.alpha, ui::kit::ColorSpace::Display)
                    .converted(ui::kit::ColorSpace::Reference, converter));
    require(std::abs(committed.red - static_cast<double>(expected.red)) < 1e-5 &&
                fixture.history.size() == 1,
            "a timeline display edit commits the converted reference number as one transaction");
    require(committed.green == base.green && committed.blue == base.blue &&
                committed.alpha == base.alpha,
            "editing one timeline channel preserves the untouched reference channels exactly");
    require(session.undo() && session.constantColorValue(parameterId) == base,
            "undo restores the timeline colour edit as one coherent step");

    // Pointer scrub of the same in-range field: the live preview converts the display number
    // through the boundary, untouched channels stay exact, dragging authors no history, release
    // commits once, and undo restores.
    fixture.history.clear();
    require(session.constantColorValue(parameterId) == base,
            "the scrub starts from the base colour");
    row.bind(entry);
    row.resize(600, 28);
    QCoreApplication::processEvents();
    require(field->width() > 0, "the timeline field has real geometry for a pointer gesture");
    const double scrubBase = field->value();
    const QPoint pressPoint = field->rect().center();
    const QPoint movePoint = pressPoint + QPoint(20, 0);
    QTest::mousePress(field, Qt::LeftButton, Qt::NoModifier, pressPoint);
    QMouseEvent move(QEvent::MouseMove, movePoint, field->mapToGlobal(movePoint), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(field, &move);
    require(field->isScrubbing(), "a timeline colour field scrubs under the pointer");
    const double displayDuringDrag = field->value();
    require(std::abs(displayDuringDrag - scrubBase) > 1e-9, "the scrub moved the display number");
    const auto live = checked(session.effectiveColorValue(parameterId));
    const auto liveExpected = checked(
        ui::kit::KColor::fromRgba(static_cast<float>(displayDuringDrag), baseDisplay.green,
                                  baseDisplay.blue, baseDisplay.alpha, ui::kit::ColorSpace::Display)
            .converted(ui::kit::ColorSpace::Reference, converter));
    require(
        std::abs(live.red - static_cast<double>(liveExpected.red)) < 1e-5 &&
            live.green == base.green && live.blue == base.blue && live.alpha == base.alpha,
        "the live scrub preview converts the display number and keeps untouched channels exact");
    require(fixture.history.size() == 0, "dragging the timeline colour authors no history");
    QTest::mouseRelease(field, Qt::LeftButton, Qt::NoModifier, movePoint);
    require(!session.valueEditActive() && fixture.history.size() == 1,
            "releasing the scrub commits exactly one colour transaction");
    require(session.undo() && session.constantColorValue(parameterId) == base,
            "undo restores the scrubbed colour as one coherent step");
    fixture.history.clear();

    // Escape cancels the same pointer gesture without history.
    row.bind(entry);
    const double cancelBase = field->value();
    QTest::mousePress(field, Qt::LeftButton, Qt::NoModifier, pressPoint);
    QApplication::sendEvent(field, &move);
    require(field->isScrubbing() && fixture.history.size() == 0, "the scrub is live before cancel");
    QTest::keyClick(field, Qt::Key_Escape);
    require(field->value() == cancelBase && session.constantColorValue(parameterId) == base &&
                fixture.history.size() == 0,
            "Escape cancels the timeline colour scrub without history");

    // Typing an out-of-range RGB number into a display-mode field authors it as a reference channel
    // and switches the row to honest reference presentation, preserving the other channels.
    QTest::keyClick(field, Qt::Key_Return);
    QTest::keyClicks(field->lineEdit(), "4.5");
    QTest::keyClick(field->lineEdit(), Qt::Key_Return);
    const auto outOfRange = checked(session.constantColorValue(parameterId));
    require(outOfRange.red == 4.5 && outOfRange.green == base.green &&
                outOfRange.blue == base.blue && outOfRange.alpha == base.alpha,
            "an out-of-range display edit authors the reference channel and preserves the others");
    fixture.history.clear();
    row.bind(entry);
    require(field->value() == 4.5 && field->unit() == QStringLiteral("reference"),
            "the out-of-range timeline value is labelled reference");

    // A signed/HDR reference switches the whole colour row to exact reference numbers with the
    // existing "reference" suffix.
    constexpr core::Color4d hdr{4.123456789, -0.25, 0.125, 0.75};
    require(session.setSelectedSolidColor(hdr), "an HDR reference colour is authored");
    fixture.history.clear();
    row.bind(entry);
    require(field->value() == hdr.red && field->unit() == QStringLiteral("reference"),
            "HDR timeline red shows its exact reference number under the reference suffix");
    entry.component = document::AnimationComponent::Green;
    row.bind(entry);
    require(field->value() == hdr.green && field->unit() == QStringLiteral("reference"),
            "the whole HDR colour row switches to reference presentation");
    require(field->value() == hdr.green && session.constantColorValue(parameterId) == hdr,
            "presenting an HDR row authors no data change");

    // A reference-mode edit commits the typed number directly and preserves the other channels.
    entry.component = document::AnimationComponent::Red;
    row.bind(entry);
    QTest::keyClick(field, Qt::Key_Return);
    QTest::keyClicks(field->lineEdit(), "8.25");
    QTest::keyClick(field->lineEdit(), Qt::Key_Return);
    const auto edited = checked(session.constantColorValue(parameterId));
    require(edited.red == 8.25 && edited.green == hdr.green && edited.blue == hdr.blue &&
                edited.alpha == hdr.alpha,
            "a reference-mode edit keeps the other reference channels exact");

    // Escape cancels a live edit and restores the readback without a transaction.
    fixture.history.clear();
    const auto beforeCancel = checked(session.constantColorValue(parameterId));
    QTest::keyClick(field, Qt::Key_Return);
    QTest::keyClicks(field->lineEdit(), "0.9");
    QTest::keyClick(field->lineEdit(), Qt::Key_Escape);
    require(session.constantColorValue(parameterId) == beforeCancel && fixture.history.size() == 0,
            "Escape on a timeline colour field restores without history");
}
} // namespace
int main(int argc, char** argv) {
    try {
        QApplication application(argc, argv);
        QTemporaryDir settings(QDir::currentPath() + "/feedback-settings-XXXXXX");
        require(settings.isValid(), "test settings stay inside the build tree");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        application.setOrganizationName("BloomTests");
        application.setApplicationName("LiveValues");
        sessionEdits();
        fieldsShareLiveValues();
        timelineColorBoundary();
        interactiveBeforeCommit();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
