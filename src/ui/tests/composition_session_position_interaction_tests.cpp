// Tests for CompositionSession's direct-manipulation position interaction (docs/architecture/
// animation-and-time.md, "Direct Manipulation And Preview Overrides"; issue #82, task D1).
// Deliberately pure-session (QCoreApplication, no widgets, no preview pipeline) mirroring
// composition_session_animation_tests.cpp's idiom: PositionInteractionMapping is supplied directly
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
#include <bloom/runtime/evaluation.hpp>
#include <bloom/ui/composition_session.hpp>

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

[[nodiscard]] ui::PositionInteractionMapping
makeMapping(const QRectF& displayRect, const document::CompositionFormat compositionFormat) {
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
    return ui::PositionInteractionMapping{
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
    require(!session.beginPositionInteraction(mapping).has_value(),
            "begin succeeds for a selected layer with a resolvable constant position");

    session.updatePositionInteraction(20.0, 10.0);
    const auto override1 = session.positionInteractionOverride();
    require(override1.has_value(), "an active interaction always reports an override");
    if (override1.has_value()) {
        const auto* value1 = std::get_if<document::Vec2d>(&override1->value);
        require(value1 != nullptr && value1->x == 100.0 + 20.0 / 200.0 * 1000.0 &&
                    value1->y == 10.0 + 10.0 / 100.0 * 100.0,
                "compositionDx/Dy = screenDx/Dy / displayWidth/Height * compositionWidth/Height, "
                "per axis independently");
    }

    // Negative displacement.
    session.updatePositionInteraction(-40.0, -5.0);
    const auto override2 = session.positionInteractionOverride();
    require(override2.has_value(), "an active interaction always reports an override");
    if (override2.has_value()) {
        const auto* value2 = std::get_if<document::Vec2d>(&override2->value);
        require(value2 != nullptr && value2->x == 100.0 + (-40.0) / 200.0 * 1000.0 &&
                    value2->y == 10.0 + (-5.0) / 100.0 * 100.0,
                "negative screen displacement maps to negative composition displacement");
    }

    // Base + TOTAL displacement, never a chain of already-rounded intermediates: two sequential
    // updates must land exactly where one single combined update lands, not at
    // base + f(first) + f(second).
    session.cancelPositionInteraction();
    require(!session.beginPositionInteraction(mapping).has_value(), "restart the interaction");
    session.updatePositionInteraction(5.0, 5.0);
    session.updatePositionInteraction(37.0, -13.0);
    const auto chained = session.positionInteractionOverride();

    session.cancelPositionInteraction();
    require(!session.beginPositionInteraction(mapping).has_value(),
            "restart the interaction again");
    session.updatePositionInteraction(37.0, -13.0);
    const auto direct = session.positionInteractionOverride();

    require(chained.has_value() && direct.has_value(),
            "both restarted interactions report overrides");
    if (chained.has_value() && direct.has_value()) {
        const auto* chainedValue = std::get_if<document::Vec2d>(&chained->value);
        const auto* directValue = std::get_if<document::Vec2d>(&direct->value);
        require(chainedValue != nullptr && directValue != nullptr && *chainedValue == *directValue,
                "two updates land exactly where one combined update lands (base + TOTAL "
                "displacement)");
    }
    session.cancelPositionInteraction();
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
    require(session.beginPositionInteraction(validMapping) ==
                ui::PositionInteractionRejection::NoLayerSelected,
            "no selection is a typed NoLayerSelected rejection");
    require(!session.positionInteractionActive(), "a rejected begin leaves no active interaction");

    // Empty mapping.
    session.selectLayer(ids.layer);
    require(session.beginPositionInteraction(makeMapping(QRectF(), format)) ==
                ui::PositionInteractionRejection::EmptyMapping,
            "an empty display rectangle is a typed EmptyMapping rejection");
    require(!session.positionInteractionActive(),
            "an empty-mapping rejection leaves no active interaction");

    // Successful begin freezes the base value/revision.
    require(!session.beginPositionInteraction(validMapping).has_value(),
            "a selected layer with a resolvable position and a non-empty mapping begins");
    require(session.positionInteractionActive(), "begin leaves the interaction active");
    const auto freshOverride = session.positionInteractionOverride();
    require(freshOverride.has_value() && freshOverride->parameterId == ids.position &&
                freshOverride->sourceRevision == document.snapshot().revision() &&
                std::get_if<document::Vec2d>(&freshOverride->value) != nullptr &&
                *std::get_if<document::Vec2d>(&freshOverride->value) == document::Vec2d{1.0, 2.0},
            "a fresh begin's override starts at exactly the base value/revision/target");
    session.cancelPositionInteraction();
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
        commands::Transaction drive("Drive position", document.snapshot().revision());
        drive.emplace<commands::SetParameterSource>(
            compositionId, ids.position,
            document::DriverBindingSource{document::DriverBindingId::fromRaw(1)});
        require(stack.execute(std::move(drive)).changed(), "test position becomes driver-backed");
    }
    ui::CompositionSession drivenSession(document, stack, compositionId);
    drivenSession.selectLayer(ids.layer);
    require(drivenSession.beginPositionInteraction(mapping) ==
                ui::PositionInteractionRejection::DrivenParameter,
            "a driven position parameter never begins a gesture (never silently disconnects it)");
    require(!drivenSession.positionInteractionActive(),
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

    require(!session.beginPositionInteraction(mapping).has_value(),
            "an animated position with no exact key at the current time now begins (D1's "
            "relaxation), rather than refusing");
    const auto freshOverride = session.positionInteractionOverride();
    require(freshOverride.has_value(), "an active interaction always reports an override");
    if (freshOverride.has_value()) {
        const auto* value = std::get_if<document::Vec2d>(&freshOverride->value);
        require(value != nullptr && *value == document::Vec2d{8.0, 14.0},
                "begin freezes the exact Linear-interpolated base at the current time (halfway "
                "between (3,4) and (13,24))");
    }

    session.updatePositionInteraction(10.0, 0.0);
    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitPositionInteraction(), "commit succeeds for the sampled-base gesture");
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
    require(!session.beginPositionInteraction(mapping).has_value(),
            "an animated position with an exact key at the current time begins");
    session.updatePositionInteraction(10.0, 0.0);
    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitPositionInteraction(), "commit succeeds for the animated key update");
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
    require(!session.beginPositionInteraction(mapping).has_value(), "begin succeeds");
    session.updatePositionInteraction(30.0, -20.0);

    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitPositionInteraction(), "commit succeeds");
    require(!session.positionInteractionActive(), "commit always clears interaction state");
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
    require(!session.beginPositionInteraction(mapping).has_value(), "begin succeeds");
    const auto revisionBeforeCommit = session.snapshot().revision();
    require(session.commitPositionInteraction(), "a zero-displacement commit reports success");
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
    require(!session.beginPositionInteraction(mapping).has_value(), "begin succeeds");
    session.updatePositionInteraction(99.0, 99.0);
    const auto revisionBeforeCancel = session.snapshot().revision();

    session.cancelPositionInteraction();
    require(!session.positionInteractionActive(), "cancel clears interaction state");
    require(!session.positionInteractionOverride().has_value(), "cancel leaves no override");
    require(session.snapshot().revision() == revisionBeforeCancel, "cancel creates no command");
    require(session.constantVec2Value(ids.position) == document::Vec2d{0.0, 0.0},
            "cancel never mutates project truth");

    // invalidatePositionInteraction() is the same clearing effect, for the Viewer's own detected
    // environment changes.
    require(!session.beginPositionInteraction(mapping).has_value(), "begin succeeds again");
    session.invalidatePositionInteraction();
    require(!session.positionInteractionActive(), "invalidate clears interaction state");
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
    require(!session.beginPositionInteraction(mapping).has_value(), "begin succeeds");
    require(session.setComposition(secondCompositionId), "session switches composition");
    require(!session.positionInteractionActive(),
            "a composition switch cancels an active interaction (its target belongs to the OLD "
            "composition)");

    // An unrelated document edit (stale base revision) cancels.
    require(session.setComposition(firstCompositionId), "session switches back");
    session.selectLayer(ids.layer);
    require(!session.beginPositionInteraction(mapping).has_value(), "begin succeeds again");
    require(session.addTextLayer(QStringLiteral("Unrelated"), QStringLiteral("Unrelated")),
            "an unrelated edit advances the document revision");
    require(!session.positionInteractionActive(),
            "a snapshot change that breaks the frozen base revision cancels the interaction");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
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
