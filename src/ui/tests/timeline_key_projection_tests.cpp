// ADR 0024: the Timeline projects layer-clock keys into composition time. A layer starting at frame
// 10 draws its opacity keys 10 frames later, edits them through the same mapping, and keys at the
// playhead on the layer clock; identity timing is unchanged.

#include "../timeline_key_time.hpp"

#include <QApplication>
#include <QMouseEvent>
#include <QWidget>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace bloom;
using namespace std::chrono_literals;

namespace {
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << message << '\n';
    }
}
core::RationalTime frame(const std::int64_t index, const std::int64_t rate = 24) {
    return *core::RationalTime::create(index, rate);
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction pipeline;
    Pipeline() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Keys", "Main", frame(240),
        *document::CompositionFormat::create(4, 3));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    ui::CompositionSession session{document, commands, initial.initialCompositionId};
    runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                                  .blockingIoWorkerCount = 1,
                                                                  .cpuQueueCapacity = 16,
                                                                  .blockingIoQueueCapacity = 4,
                                                                  .terminalHistoryCapacity = 32,
                                                                  .diagnosticsPerTask = 8,
                                                                  .groupRegistryCapacity = 8}};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    Pipeline pipeline;
    ui::CompositionPreviewController controller{session, scheduler, bridge, pipeline.pipeline};
    ~Fixture() {
        controller.beginShutdown();
        bridge.beginShutdown();
        for (int i = 0; i < 400 && !scheduler.isQuiescent(); ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
};

std::vector<core::RationalTime> storedOpacityKeys(ui::CompositionSession& session,
                                                  const document::ParameterId parameter) {
    std::vector<core::RationalTime> times;
    const auto* record = session.composition()->parameters().find(parameter);
    const auto* source =
        record ? std::get_if<document::AnimationCurveSource>(&record->source) : nullptr;
    const auto* curve =
        source ? session.composition()->animationCurves().findScalar(source->curveId) : nullptr;
    if (curve)
        for (const auto& key : curve->keyframes)
            times.push_back(key.time);
    return times;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // The Qt-free mapping, including a non-unit speed.
    {
        document::LayerTiming timing;
        timing.startTime = frame(10);
        timing.speed = *document::LayerSpeed::create(2, 1);
        const ui::KeyClock clock = timing;
        expect(ui::compositionTimeOfKey(clock, frame(8)) == frame(14),
               "tau 8f at 2x from start 10f lands at 14f");
        expect(ui::keyTimeOfCompositionTime(clock, frame(14)) == frame(8), "and inverts exactly");
        expect(ui::compositionTimeOfKey(std::nullopt, frame(7)) == frame(7),
               "no clock is identity");
    }

    Fixture fixture;
    auto& session = fixture.session;
    expect(session.addSolidLayer("Solid", core::Color4d{0.2, 0.3, 0.4, 1.0}), "solid added");
    const auto* opacity = session.parameterForSelection(document::kOpacityParameterRole);
    expect(opacity != nullptr, "opacity parameter");
    if (!opacity)
        return 1;
    const auto opacityId = opacity->id;
    const auto layerId = *std::get_if<document::LayerId>(&session.selection().primary);

    ui::TimelineEditor editor(session, fixture.controller);
    editor.resize(1600, 500);
    editor.show();
    QCoreApplication::processEvents();

    // (c) identity timing: a key at frame 5 summarises at frame 5.
    expect(session.setCurrentTime(frame(5)), "playhead to 5");
    expect(session.toggleKeyframe(document::kOpacityParameterRole), "key opacity at 5");
    expect(storedOpacityKeys(session, opacityId) == std::vector{frame(5)}, "identity stores 5");
    expect(editor.laneRegionForTest()->keySummaryTimes(0) == std::vector{frame(5)},
           "identity summary unchanged");
    expect(session.keyTimeAtPlayhead(opacityId) == frame(5), "identity playhead key time");

    // The layer starts at frame 10.
    {
        const auto* boundary = session.composition()->graph().findLayer(layerId);
        commands::Transaction edit("Start", session.snapshot().revision());
        edit.emplace<commands::SetLayerTiming>(session.compositionId(), layerId, frame(10),
                                               frame(10), boundary->endPoint(frame(240)));
        expect(session.executeTransaction(std::move(edit)).changed(), "start moved to 10");
    }
    QCoreApplication::processEvents();

    // (a) the stored key (layer time 5) is drawn at 10 + 5.
    expect(storedOpacityKeys(session, opacityId) == std::vector{frame(5)},
           "stored layer-time key is untouched by the timing edit");
    expect(editor.laneRegionForTest()->keySummaryTimes(0) == std::vector{frame(15)},
           "collapsed summary is shifted by the start");

    // (b) keying at the playhead stores the layer-time key.
    expect(session.setCurrentTime(frame(20)), "playhead to 20");
    expect(session.keyTimeAtPlayhead(opacityId) == frame(10), "playhead 20 is layer time 10");
    expect(session.toggleKeyframe(document::kOpacityParameterRole), "key opacity at playhead 20");
    expect((storedOpacityKeys(session, opacityId) == std::vector{frame(5), frame(10)}),
           "the key is stored at layer time 10");
    expect((editor.laneRegionForTest()->keySummaryTimes(0) == std::vector{frame(15), frame(20)}),
           "and drawn under the playhead");
    expect(session.keyframeDiamondState(document::kOpacityParameterRole) ==
               ui::KeyframeDiamondState::AnimatedWithKey,
           "the diamond sees the key at the playhead");
    expect(session.setSelectedOpacity(0.5), "an opacity edit at the playhead");
    expect((storedOpacityKeys(session, opacityId) == std::vector{frame(5), frame(10)}),
           "updates the key at layer time 10 instead of adding one");

    // Editing: double-click insert on the expanded lane at composition frame 30 stores layer 20.
    editor.layerStackForTest()->expansionRequested(layerId);
    QCoreApplication::processEvents();
    QWidget* row = nullptr;
    for (auto* candidate : editor.findChildren<QWidget*>("timelineKeyframeRow"))
        if (candidate->isVisible() && row == nullptr)
            row = candidate;
    expect(row != nullptr, "an opacity key lane exists");
    if (row) {
        const auto axis = editor.rulerForTest()->axisForWidth(row->width());
        expect(axis.has_value(), "axis");
        if (axis) {
            const QPointF local(axis->pixelForTime(frame(30)), row->height() / 2.0);
            QMouseEvent doubleClick(QEvent::MouseButtonDblClick, local, local, Qt::LeftButton,
                                    Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(row, &doubleClick);
            expect((storedOpacityKeys(session, opacityId) ==
                    std::vector{frame(5), frame(10), frame(20)}),
                   "double-click at composition 30 inserts layer time 20");

            // Drag the key drawn at composition 15 (layer 5) to composition 25 (layer 15).
            const auto press = QPointF(axis->pixelForTime(frame(15)), row->height() / 2.0);
            const auto drop = QPointF(axis->pixelForTime(frame(25)), row->height() / 2.0);
            const auto send = [&](QEvent::Type type, const QPointF& at) {
                QMouseEvent event(type, at, row->mapToGlobal(at),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(row, &event);
            };
            send(QEvent::MouseButtonPress, press);
            send(QEvent::MouseMove, drop);
            send(QEvent::MouseButtonRelease, drop);
            expect((storedOpacityKeys(session, opacityId) ==
                    std::vector{frame(10), frame(15), frame(20)}),
                   "dragging the drawn key to composition 25 stores layer time 15");
        }
    }

    // Copy/paste anchors the first key on the playhead in composition time.
    session.selectKeyframes({});
    if (const auto* record = session.composition()->parameters().find(opacityId)) {
        const auto* source = std::get_if<document::AnimationCurveSource>(&record->source);
        const auto* curve = session.composition()->animationCurves().findScalar(source->curveId);
        session.selectKeyframe(source->curveId, curve->keyframes.front().id);
    }
    session.copySelectedKeyframes();
    expect(session.setCurrentTime(frame(60)), "playhead to 60");
    expect(session.pasteCopiedKeyframes(), "paste");
    const auto stored = storedOpacityKeys(session, opacityId);
    expect(std::ranges::find(stored, frame(50)) != stored.end(),
           "a key pasted at composition 60 is stored at layer time 50");

    if (failures == 0)
        std::cout << "timeline key projection: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
