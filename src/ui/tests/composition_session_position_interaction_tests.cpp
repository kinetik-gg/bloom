// Tests for CompositionSession's direct-manipulation position interaction (docs/architecture/
// animation-and-time.md, "Direct Manipulation And Preview Overrides"; issue #82, task D1).
// Deliberately pure-session (QCoreApplication, no widgets, no preview pipeline) mirroring
// composition_session_animation_tests.cpp's idiom: ViewerMapping is supplied directly
// rather than sourced from a rendered frame, since only ViewerEditor is responsible for computing
// it from real display geometry (covered separately by the viewer gesture tests).

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/ui/composition_session.hpp>
#include <cmath>

#include <QCoreApplication>
#include <QRectF>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace bloom;

[[noreturn]] void fail(const std::string_view message) {
    std::cerr << "composition session position interaction test failed: " << message << '\n';
    std::exit(1);
}

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto result = core::RationalTime::create(numerator, denominator);
    if (!result.has_value()) {
        fail("test time must be valid");
    }
    return *result;
}

// A non-square display rectangle (2:1) mapped against a non-square, differently-proportioned
// composition format (10:1) so X and Y scale factors genuinely differ -- this exercises the
// per-axis formulas independently rather than a uniform scale that would hide an axis-swap bug.
[[nodiscard]] document::CompositionFormat wideFormat() {
    const auto format = document::CompositionFormat::create(1000, 100);
    if (!format.has_value()) {
        fail("wide composition format fixture must be valid");
    }
    return *format;
}

[[nodiscard]] ui::ViewerMapping makeMapping(const QRectF& displayRect,
                                            const document::CompositionFormat compositionFormat) {
    const auto window =
        render::ImageWindow::create(0, 0, compositionFormat.width(), compositionFormat.height());
    if (!window) {
        fail("mapping fixture display window must be valid");
    }
    const auto descriptor = render::ReferenceDisplayBufferDescriptor::create(
        *window.value(), compositionFormat.pixelAspect());
    if (!descriptor) {
        fail("mapping fixture display descriptor must be valid");
    }
    return ui::ViewerMapping{
        .displayRect = displayRect,
        .compositionFormat = compositionFormat,
        .resolution = runtime::CompositionFormatResolution{},
        .pixelAspect = compositionFormat.pixelAspect(),
        .displayDescriptor = *descriptor.value(),
    };
}

struct LayerIds final {
    document::LayerId layer;
    document::ParameterId position;
};

[[nodiscard]] LayerIds addSolidLayer(document::Document& document, commands::CommandStack& stack,
                                     const document::CompositionId compositionId,
                                     const document::Vec2d position) {
    commands::Transaction transaction("Add test layer", document.snapshot().revision());
    transaction.emplace<commands::AddSolidLayer>(compositionId, "Solid",
                                                 core::Color4d{0.2, 0.3, 0.4, 1.0}, position);
    const auto result = stack.execute(std::move(transaction));
    const auto layer = result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    const auto positionId =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput);
    if (!(result.changed() && layer.has_value() && positionId.has_value())) {
        fail("solid layer command must expose its stable IDs");
    }
    return {*layer, *positionId};
}

void testDisplacementMathNonSquareNegativeAndBaseTotal() {
    auto newProject = document::makeNewProject("Displacement Math", "Main", time(10), wideFormat());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{100.0, 10.0});

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);

    // displayRect is 200x100 (2:1); compositionFormat is 1000x100 (10:1): scaleX = 1000/200 = 5,
    // scaleY = 100/100 = 1 -- deliberately different per-axis scale factors.
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 200.0, 100.0), wideFormat());
    require(!session.beginTransformInteraction({}, mapping).has_value(),
            "begin succeeds for a selected layer with a resolvable constant position");

    session.updateTransformInteraction({20.0, 10.0});
    const auto override1 = session.transformInteractionOverrides();
    require(!override1.empty(), "an active interaction always reports an override");
    if (!override1.empty()) {
        const auto* value1 = std::get_if<document::Vec2d>(&override1.front().value);
        require(value1 != nullptr && value1->x == 100.0 + 20.0 / 200.0 * 1000.0 &&
                    value1->y == 10.0 + 10.0 / 100.0 * 100.0,
                "compositionDx/Dy = screenDx/Dy / displayWidth/Height * compositionWidth/Height, "
                "per axis independently");
    }

    // Negative displacement.
    session.updateTransformInteraction({-40.0, -5.0});
    const auto override2 = session.transformInteractionOverrides();
    require(!override2.empty(), "an active interaction always reports an override");
    if (!override2.empty()) {
        const auto* value2 = std::get_if<document::Vec2d>(&override2.front().value);
        require(value2 != nullptr && value2->x == 100.0 + (-40.0) / 200.0 * 1000.0 &&
                    value2->y == 10.0 + (-5.0) / 100.0 * 100.0,
                "negative screen displacement maps to negative composition displacement");
    }

    // Base + TOTAL displacement, never a chain of already-rounded intermediates: two sequential
    // updates must land exactly where one single combined update lands, not at
    // base + f(first) + f(second).
    session.cancelTransformInteraction();
    require(!session.beginTransformInteraction({}, mapping).has_value(), "restart the interaction");
    session.updateTransformInteraction({5.0, 5.0});
    session.updateTransformInteraction({37.0, -13.0});
    const auto chained = session.transformInteractionOverrides();

    session.cancelTransformInteraction();
    require(!session.beginTransformInteraction({}, mapping).has_value(),
            "restart the interaction again");
    session.updateTransformInteraction({37.0, -13.0});
    const auto direct = session.transformInteractionOverrides();

    require(!chained.empty() && !direct.empty(), "both restarted interactions report overrides");
    if (!chained.empty() && !direct.empty()) {
        const auto* chainedValue = std::get_if<document::Vec2d>(&chained.front().value);
        const auto* directValue = std::get_if<document::Vec2d>(&direct.front().value);
        require(chainedValue != nullptr && directValue != nullptr && *chainedValue == *directValue,
                "two updates land exactly where one combined update lands (base + TOTAL "
                "displacement)");
    }
    session.cancelTransformInteraction();
}

void testBeginRejectionsAndFreezing() {
    auto newProject = document::makeNewProject("Begin Rejections", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{1.0, 2.0});
    const auto format = document.snapshot().project().findComposition(compositionId)->format();
    const auto validMapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    ui::CompositionSession session(document, stack, compositionId);

    // No selection.
    require(session.beginTransformInteraction({}, validMapping) ==
                ui::TransformInteractionRejection::NoLayerSelected,
            "no selection is a typed NoLayerSelected rejection");
    require(!session.transformInteractionActive(), "a rejected begin leaves no active interaction");

    // Empty mapping.
    session.selectLayer(ids.layer);
    require(session.beginTransformInteraction({}, makeMapping(QRectF(), format)) ==
                ui::TransformInteractionRejection::EmptyMapping,
            "an empty display rectangle is a typed EmptyMapping rejection");
    require(!session.transformInteractionActive(),
            "an empty-mapping rejection leaves no active interaction");

    // Successful begin freezes the base value/revision.
    require(!session.beginTransformInteraction({}, validMapping).has_value(),
            "a selected layer with a resolvable position and a non-empty mapping begins");
    require(session.transformInteractionActive(), "begin leaves the interaction active");
    const auto freshOverride = session.transformInteractionOverrides();
    require(!freshOverride.empty() && freshOverride.front().parameterId == ids.position &&
                freshOverride.front().sourceRevision == document.snapshot().revision() &&
                std::get_if<document::Vec2d>(&freshOverride.front().value) != nullptr &&
                *std::get_if<document::Vec2d>(&freshOverride.front().value) ==
                    document::Vec2d{1.0, 2.0},
            "a fresh begin's override starts at exactly the base value/revision/target");
    session.cancelTransformInteraction();
}

void testDrivenAndAnimatedParameterRejections() {
    auto newProject = document::makeNewProject("Driven And Animated", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{5.0, 6.0});
    const auto format = document.snapshot().project().findComposition(compositionId)->format();
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    // Driven.
    {
        // ADAPTED (task S7): a driver names the value-graph output it reads, and the kinds have to
        // connect -- so the fixture adds the Vector2 value node a position can be driven from.
        commands::Transaction addValue("Add value node", document.snapshot().revision());
        addValue.emplace<commands::AddNode>(
            compositionId, std::string(document::kVector2ValueNodeType), document::Vec2d{9.0, 9.0});
        const auto valueResult = stack.execute(std::move(addValue));
        const auto valueNode = valueResult.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!valueNode.has_value()) {
            require(false, "the driven fixture adds its Vector2 value node");
            return;
        }
        commands::Transaction drive("Drive position", document.snapshot().revision());
        drive.emplace<commands::SetParameterSource>(
            compositionId, ids.position,
            document::DriverBindingSource{*valueNode, std::string(document::kValuePortName)});
        require(stack.execute(std::move(drive)).changed(), "test position becomes driver-backed");
    }
    ui::CompositionSession drivenSession(document, stack, compositionId);
    drivenSession.selectLayer(ids.layer);
    require(drivenSession.beginTransformInteraction({}, mapping) ==
                ui::TransformInteractionRejection::DrivenParameter,
            "a driven position parameter never begins a gesture (never silently disconnects it)");
    require(!drivenSession.transformInteractionActive(),
            "the driven rejection leaves no interaction");
}

// D1's relaxation (issue #86, task E1; docs/architecture/animation-and-time.md): an animated
// position with NO exact key at the current time now begins from the exact sampled interpolated
// base instead of refusing with the former AnimatedWithoutExactKey rejection. Commit still goes
// through executePositionCommand() -> SetKeyframeAtTime(), which INSERTS a key at the current time
// since none exists there yet.
void testAnimatedParameterSampledBaseInsertsKeyAtCurrentTimeAndUndoRemovesOnlyIt() {
    auto newProject = document::makeNewProject("Animated Sampled Base", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{3.0, 4.0});
    const auto format = document.snapshot().project().findComposition(compositionId)->format();
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    commands::Transaction animate("Animate position", document.snapshot().revision());
    animate.emplace<commands::CreateAnimationForParameter>(compositionId, ids.position, time(0));
    const auto animateResult = stack.execute(std::move(animate));
    const auto curveId =
        animateResult.outputId<document::AnimationCurveId>(commands::kAnimationCurveOutput);
    require(animateResult.changed() && curveId.has_value(), "position becomes animated");
    if (!curveId.has_value()) {
        return;
    }

    // A second exact key at t=10 gives a genuinely interpolated midpoint at t=5 (factor exactly
    // 0.5, endpoints chosen so Linear Mix is exact in binary64: (3,4) -> (8,14) -> (13,24)),
    // exercising the sampled base rather than degenerating to a single-key clamp.
    commands::Transaction insertSecond("Insert second position key",
                                       document.snapshot().revision());
    insertSecond.emplace<commands::InsertVec2Keyframe>(compositionId, *curveId, time(10),
                                                       document::Vec2d{13.0, 24.0});
    require(stack.execute(std::move(insertSecond)).changed(), "fixture inserts a second exact key");

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    require(session.setCurrentTime(time(5)), "session moves to a time between the two keys");

    require(!session.beginTransformInteraction({}, mapping).has_value(),
            "an animated position with no exact key at the current time now begins (D1's "
            "relaxation), rather than refusing");
    const auto freshOverride = session.transformInteractionOverrides();
    require(!freshOverride.empty(), "an active interaction always reports an override");
    if (!freshOverride.empty()) {
        const auto* value = std::get_if<document::Vec2d>(&freshOverride.front().value);
        require(value != nullptr && *value == document::Vec2d{8.0, 14.0},
                "begin freezes the exact Linear-interpolated base at the current time (halfway "
                "between (3,4) and (13,24))");
    }

    session.updateTransformInteraction({10.0, 0.0});
    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitTransformInteraction(), "commit succeeds for the sampled-base gesture");
    require(session.snapshot().revision().value() == revisionBeforeCommit.value() + 1,
            "commit is exactly one transaction");

    const auto* curve = session.composition()->animationCurves().findVec2(*curveId);
    require(curve != nullptr && curve->keyframes.size() == 3,
            "commit INSERTS a new key at the current time rather than mutating an existing one");
    if (curve == nullptr || curve->keyframes.size() != 3) {
        return;
    }
    const auto inserted =
        std::ranges::find(curve->keyframes, time(5), &document::Vec2Keyframe::time);
    require(inserted != curve->keyframes.end() &&
                inserted->value == document::Vec2d{8.0 + 10.0 / 100.0 * format.width(), 14.0},
            "the inserted key holds the sampled-base-plus-displacement value at the exact current "
            "time");
    const auto seed = std::ranges::find(curve->keyframes, time(0), &document::Vec2Keyframe::time);
    const auto second =
        std::ranges::find(curve->keyframes, time(10), &document::Vec2Keyframe::time);
    require(seed != curve->keyframes.end() && seed->value == document::Vec2d{3.0, 4.0} &&
                second != curve->keyframes.end() && second->value == document::Vec2d{13.0, 24.0},
            "the curve's other two keys are untouched by the insert");

    require(session.undo(), "the sampled-base commit undoes cleanly");
    curve = session.composition()->animationCurves().findVec2(*curveId);
    require(curve != nullptr && curve->keyframes.size() == 2,
            "undo removes EXACTLY the inserted key, restoring the original two");
    if (curve != nullptr) {
        const auto seedAfterUndo =
            std::ranges::find(curve->keyframes, time(0), &document::Vec2Keyframe::time);
        const auto secondAfterUndo =
            std::ranges::find(curve->keyframes, time(10), &document::Vec2Keyframe::time);
        require(seedAfterUndo != curve->keyframes.end() &&
                    seedAfterUndo->value == document::Vec2d{3.0, 4.0} &&
                    secondAfterUndo != curve->keyframes.end() &&
                    secondAfterUndo->value == document::Vec2d{13.0, 24.0},
                "undo leaves the curve's other keys exactly as they were");
    }
}

// Regression on D1's original had-a-key behavior (issue #86, task E1): when the playhead DOES sit
// exactly on a key, commit must still update that SAME key rather than insert a second one.
void testAnimatedParameterExactKeyStillUpdatesThatKey() {
    auto newProject = document::makeNewProject("Animated Exact Key", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{3.0, 4.0});
    const auto format = document.snapshot().project().findComposition(compositionId)->format();
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    commands::Transaction animate("Animate position", document.snapshot().revision());
    animate.emplace<commands::CreateAnimationForParameter>(compositionId, ids.position, time(0));
    const auto animateResult = stack.execute(std::move(animate));
    const auto curveId =
        animateResult.outputId<document::AnimationCurveId>(commands::kAnimationCurveOutput);
    require(animateResult.changed() && curveId.has_value(), "position becomes animated");
    if (!curveId.has_value()) {
        return;
    }

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    require(session.currentTime() == time(0),
            "the session's default current time already sits exactly on the seeded key's time");
    require(!session.beginTransformInteraction({}, mapping).has_value(),
            "an animated position with an exact key at the current time begins");
    session.updateTransformInteraction({10.0, 0.0});
    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitTransformInteraction(), "commit succeeds for the animated key update");
    require(session.snapshot().revision().value() == revisionBeforeCommit.value() + 1,
            "commit is exactly one transaction");

    const auto* curve = session.composition()->animationCurves().findVec2(*curveId);
    require(curve != nullptr && curve->keyframes.size() == 1,
            "the commit updates the existing seeded key rather than inserting a second one");
    require(curve->keyframes.front().value ==
                document::Vec2d{3.0 + 10.0 / 100.0 * format.width(), 4.0},
            "the updated key holds the base-plus-displacement value");

    require(session.undo(), "the animated commit undoes cleanly");
    curve = session.composition()->animationCurves().findVec2(*curveId);
    require(curve != nullptr && curve->keyframes.size() == 1 &&
                curve->keyframes.front().value == document::Vec2d{3.0, 4.0},
            "undo restores the exact prior key value");
}

void testCommitIsExactlyOneTransactionAndUndoRestoresExactPriorValue() {
    auto newProject = document::makeNewProject("Commit Undo", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{50.0, 60.0});
    const auto format = document.snapshot().project().findComposition(compositionId)->format();
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    require(!session.beginTransformInteraction({}, mapping).has_value(), "begin succeeds");
    session.updateTransformInteraction({30.0, -20.0});

    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitTransformInteraction(), "commit succeeds");
    require(!session.transformInteractionActive(), "commit always clears interaction state");
    require(session.snapshot().revision().value() == revisionBeforeCommit.value() + 1,
            "one drag creates exactly one document transaction (one undo step)");
    require(session.constantVec2Value(ids.position) ==
                document::Vec2d{50.0 + 30.0 / 100.0 * format.width(),
                                60.0 + (-20.0) / 100.0 * format.height()},
            "the committed value matches base plus the frozen mapping's total displacement");

    require(session.undo(), "the commit undoes cleanly");
    require(session.constantVec2Value(ids.position) == document::Vec2d{50.0, 60.0},
            "undo restores the exact prior value");
}

void testZeroMoveCommitsNothing() {
    auto newProject = document::makeNewProject("Zero Move", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{7.0, 8.0});
    const auto format = document.snapshot().project().findComposition(compositionId)->format();
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    require(!session.beginTransformInteraction({}, mapping).has_value(), "begin succeeds");
    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitTransformInteraction(), "a zero-displacement commit reports success");
    require(session.snapshot().revision() == revisionBeforeCommit,
            "a zero move creates no command / no undo step");
    require(session.constantVec2Value(ids.position) == document::Vec2d{7.0, 8.0},
            "the value is untouched by a zero-move commit");
}

void testCancelClearsState() {
    auto newProject = document::makeNewProject("Cancel Clears", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, compositionId, document::Vec2d{0.0, 0.0});
    const auto format = document.snapshot().project().findComposition(compositionId)->format();
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    require(!session.beginTransformInteraction({}, mapping).has_value(), "begin succeeds");
    session.updateTransformInteraction({99.0, 99.0});
    const auto revisionBeforeCancel = session.snapshot().revision();

    session.cancelTransformInteraction();
    require(!session.transformInteractionActive(), "cancel clears interaction state");
    require(session.transformInteractionOverrides().empty(), "cancel leaves no override");
    require(session.snapshot().revision() == revisionBeforeCancel, "cancel creates no command");
    require(session.constantVec2Value(ids.position) == document::Vec2d{0.0, 0.0},
            "cancel never mutates project truth");

    // invalidateTransformInteraction() is the same clearing effect, for the Viewer's own detected
    // environment changes.
    require(!session.beginTransformInteraction({}, mapping).has_value(), "begin succeeds again");
    session.invalidateTransformInteraction();
    require(!session.transformInteractionActive(), "invalidate clears interaction state");
}

void testInvalidationOnCompositionSwitchAndStaleRevision() {
    auto newProject = document::makeNewProject("Invalidation", "Main", time(10));
    const auto firstCompositionId = newProject.initialCompositionId;
    const auto secondCompositionId = document::CompositionId::fromRaw(2);
    {
        using namespace document;
        const auto stackId = NodeId::fromRaw(100);
        const auto outputId = NodeId::fromRaw(101);
        const auto edgeId = EdgeId::fromRaw(100);
        CanonicalGraph graph(stackId);
        const bool built =
            graph.addNode(
                {stackId, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}) &&
            graph.addNode({outputId,
                           std::string(kCompositionOutputNodeType),
                           {},
                           kCompositionOutputNodeSchemaVersion}) &&
            graph.addEdge({edgeId,
                           {stackId, std::string(kLayerStackOutputPort)},
                           NodeInputRef{outputId, std::string(kCompositionOutputInputPort)}});
        graph.setCompositionOutput({outputId, std::string(kCompositionOutputOutputPort)});
        require(built, "second composition graph fixture builds");
        const auto format = document::CompositionFormat::create(64, 36);
        require(format.has_value(), "second composition format fixture is valid");
        if (!format.has_value()) {
            return;
        }
        require(newProject.project.addComposition(Composition(secondCompositionId, "Second",
                                                              time(10), std::move(graph), *format)),
                "second composition fixture is added");
    }
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack, firstCompositionId, document::Vec2d{1.0, 1.0});
    const auto format = document.snapshot().project().findComposition(firstCompositionId)->format();
    const auto mapping = makeMapping(QRectF(0.0, 0.0, 100.0, 100.0), format);

    ui::CompositionSession session(document, stack, firstCompositionId);
    session.selectLayer(ids.layer);

    // Composition switch cancels.
    require(!session.beginTransformInteraction({}, mapping).has_value(), "begin succeeds");
    require(session.setComposition(secondCompositionId), "session switches composition");
    require(!session.transformInteractionActive(),
            "a composition switch cancels an active interaction (its target belongs to the OLD "
            "composition)");

    // An unrelated document edit (stale base revision) cancels.
    require(session.setComposition(firstCompositionId), "session switches back");
    session.selectLayer(ids.layer);
    require(!session.beginTransformInteraction({}, mapping).has_value(), "begin succeeds again");
    require(session.addSolidLayer(QStringLiteral("Unrelated"), {1, 1, 1, 1}),
            "an unrelated edit advances the document revision");
    require(!session.transformInteractionActive(),
            "a snapshot change that breaks the frozen base revision cancels the interaction");
}

runtime::EvaluatedOperationBounds evaluatedBounds(ui::CompositionSession& session,
                                                  const document::LayerId layer) {
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "registry initializes");
    registry.freeze();
    runtime::SnapshotCompiler compiler(registry);
    const auto compiled = compiler.compile(
        {session.snapshot(), session.compositionId(), session.transformInteractionOverrides()}, {});
    require(compiled.plan != nullptr, "gesture snapshot compiles");
    runtime::CpuCompositionEvaluator evaluator;
    const auto result = evaluator.evaluate(compiled.plan,
                                           {.time = session.currentTime(),
                                            .output = compiled.plan->output(),
                                            .resolution = runtime::CompositionFormatResolution{},
                                            .pixelStorageByteLimit = 64 * 1024 * 1024},
                                           {});
    require(result.frame() != nullptr, "gesture snapshot evaluates");
    const auto bounds = result.frame()->evaluatedBounds();
    const auto found =
        std::ranges::find(bounds, layer, &runtime::EvaluatedOperationBounds::layerId);
    require(found != bounds.end(), "layer publishes bounds");
    return *found;
}

void requireNear(const document::Vec2d actual, const document::Vec2d expected,
                 const std::string_view message) {
    require(std::hypot(actual.x - expected.x, actual.y - expected.y) < 1e-8, message);
}

void testParentedTransformsCommitAtomically() {
    const auto format = *document::CompositionFormat::create(200, 200);
    auto project = document::makeNewProject("Transforms", "Main", time(10), format);
    const auto composition = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, composition);
    require(session.addSolidLayer(QStringLiteral("Parent"), {1, 0, 0, 1}), "add parent");
    const auto parent = *std::get_if<document::LayerId>(&session.selection().primary);
    require(session.setSelectedScale(1.5, 0.75) && session.setSelectedRotation(30),
            "transform parent");
    require(session.addSolidLayer(QStringLiteral("Child"), {0, 1, 0, 1}), "add child");
    const auto child = *std::get_if<document::LayerId>(&session.selection().primary);
    require(session.setLayerParent(child, parent), "parent child");
    require(session.setSelectedScale(0.4, 0.6) && session.setSelectedRotation(-20),
            "transform child");
    const auto mapping = makeMapping(QRectF(30, 40, 400, 250), format);
    const auto original = evaluatedBounds(session, child);
    auto origin = mapping.toScreen(original.anchor);
    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Move, 0, origin, original}, mapping),
            "begin parented move");
    session.updateTransformInteraction(origin + QPointF(40, -25));
    const auto preview = evaluatedBounds(session, child);
    requireNear(preview.anchor, {original.anchor.x + 20, original.anchor.y - 20},
                "parented child follows the cursor in composition space");
    require(session.commitTransformInteraction() &&
                session.undoLabel() == QStringLiteral("Move Layer"),
            "move commits one named transaction");
    require(session.undo(), "undo move");
    requireNear(evaluatedBounds(session, child).anchor, original.anchor, "one undo restores move");

    origin = mapping.toScreen(original.polygon[0]);
    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Scale, 0, origin, original}, mapping),
            "begin corner scale");
    session.updateTransformInteraction(origin + QPointF(-35, -22));
    const auto scaled = evaluatedBounds(session, child);
    requireNear(scaled.polygon[2], original.polygon[2],
                "scale keeps the opposite corner in world space");
    require(session.transformInteractionOverrides().size() == 2,
            "scale previews scale and position together");
    require(session.commitTransformInteraction() &&
                session.undoLabel() == QStringLiteral("Scale Layer"),
            "scale commits one transaction");
    require(session.undo(), "undo scale");
    for (std::size_t i = 0; i < 4; ++i)
        requireNear(evaluatedBounds(session, child).polygon[i], original.polygon[i],
                    "one undo restores both scale and position");

    origin = mapping.toScreen(original.anchor);
    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Anchor, 0, origin, original}, mapping),
            "begin anchor move");
    session.updateTransformInteraction(origin + QPointF(18, -12));
    const auto anchored = evaluatedBounds(session, child);
    for (std::size_t i = 0; i < 4; ++i)
        requireNear(anchored.polygon[i], original.polygon[i],
                    "anchor drag preserves visual placement");
    requireNear(anchored.anchor, {original.anchor.x + 9, original.anchor.y - 9.6},
                "anchor follows cursor");
    require(session.commitTransformInteraction() &&
                session.undoLabel() == QStringLiteral("Move Anchor"),
            "anchor commits one transaction");
    require(session.undo(), "undo anchor");
    requireNear(evaluatedBounds(session, child).anchor, original.anchor,
                "one undo restores anchor and position");

    origin = mapping.toScreen(original.polygon[0]);
    require(
        !session.beginTransformInteraction({ui::TransformGesture::Kind::Scale, 0, origin, original},
                                           mapping, {.shift = true, .alt = true}),
        "begin uniform pivot scale");
    session.updateTransformInteraction(origin + QPointF(-30, -30), {.shift = true, .alt = true});
    requireNear(evaluatedBounds(session, child).anchor, original.anchor,
                "Alt scale fixes the anchor");
    const auto overrides = session.transformInteractionOverrides();
    const auto* scale = std::get_if<document::Vec2d>(&overrides.back().value);
    require(scale && std::abs(scale->x / 0.4 - scale->y / 0.6) < 1e-8,
            "Shift scales both axes uniformly");
    session.cancelTransformInteraction();

    require(!session.beginTransformInteraction(
                {ui::TransformGesture::Kind::Rotate, 0, origin, original}, mapping),
            "begin rotation");
    session.updateTransformInteraction(origin + QPointF(45, -10), {.shift = true});
    const auto rotationOverrides = session.transformInteractionOverrides();
    const auto* angle = std::get_if<double>(&rotationOverrides.front().value);
    require(angle && std::abs(*angle / 15.0 - std::round(*angle / 15.0)) < 1e-8,
            "Shift snaps rotation to 15 degrees");
    require(session.commitTransformInteraction(), "rotation commits");
    require(session.undo(), "undo rotation");
    for (std::size_t i = 0; i < 4; ++i)
        requireNear(evaluatedBounds(session, child).polygon[i], original.polygon[i],
                    "undo restores rotation");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testParentedTransformsCommitAtomically();
    testDisplacementMathNonSquareNegativeAndBaseTotal();
    testBeginRejectionsAndFreezing();
    testDrivenAndAnimatedParameterRejections();
    testAnimatedParameterSampledBaseInsertsKeyAtCurrentTimeAndUndoRemovesOnlyIt();
    testAnimatedParameterExactKeyStillUpdatesThatKey();
    testCommitIsExactlyOneTransactionAndUndoRestoresExactPriorValue();
    testZeroMoveCommitsNothing();
    testCancelClearsState();
    testInvalidationOnCompositionSwitchAndStaleRevision();
    return 0;
}
