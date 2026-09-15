#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/timeline_frame_math.hpp>

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

using namespace bloom;

[[noreturn]] void fail(const std::string_view message) {
    std::cerr << "composition session animation test failed: " << message << '\n';
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

struct LayerIds final {
    document::LayerId layer;
    document::ParameterId position;
    document::ParameterId opacity;
};

[[nodiscard]] LayerIds addSolidLayer(document::Document& document, commands::CommandStack& stack) {
    commands::Transaction transaction("Add test layer", document.snapshot().revision());
    transaction.emplace<commands::AddSolidLayer>(
        document.snapshot().project().compositions().front().id(), "Solid",
        core::Color4d{0.2, 0.3, 0.4, 1.0}, document::Vec2d{10.0, 20.0});
    const auto result = stack.execute(std::move(transaction));
    const auto layer = result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    const auto position =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput);
    const auto opacity =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerOpacityParameterOutput);
    if (!(result.changed() && layer.has_value() && position.has_value() && opacity.has_value())) {
        fail("solid layer command must expose its stable IDs");
    }
    return {*layer, *position, *opacity};
}

[[nodiscard]] document::AnimationCurveId
animateParameter(document::Document& document, commands::CommandStack& stack,
                 const document::CompositionId compositionId,
                 const document::ParameterId parameterId) {
    commands::Transaction transaction("Animate test parameter", document.snapshot().revision());
    transaction.emplace<commands::CreateAnimationForParameter>(compositionId, parameterId, time(0));
    const auto result = stack.execute(std::move(transaction));
    const auto curve = result.outputId<document::AnimationCurveId>(commands::kAnimationCurveOutput);
    if (!(result.changed() && curve.has_value())) {
        fail("animation command must expose its curve ID");
    }
    return *curve;
}

void testAnimatedEditsUseExactSessionTime() {
    auto newProject = document::makeNewProject("Animated Session", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);
    const auto positionCurve = animateParameter(document, stack, compositionId, ids.position);
    const auto opacityCurve = animateParameter(document, stack, compositionId, ids.opacity);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    require(session.setCurrentTime(time(3, 2)), "session accepts an exact subframe time");
    const auto beforeEdits = session.snapshot().revision();
    require(session.setSelectedPosition(30.0, 40.0),
            "position edit inserts an exact-time Vec2 key");
    require(session.setSelectedOpacity(0.25), "opacity edit inserts an exact-time scalar key");
    require(session.snapshot().revision().value() == beforeEdits.value() + 2,
            "each animated property edit is one document transaction");

    const auto* composition = session.composition();
    require(composition != nullptr, "active composition remains available");
    const auto* position = composition->animationCurves().findVec2(positionCurve);
    const auto* opacity = composition->animationCurves().findScalar(opacityCurve);
    require(position != nullptr && position->keyframes.size() == 2 &&
                position->keyframes.back().time == time(3, 2) &&
                position->keyframes.back().value == document::Vec2d{30.0, 40.0},
            "position key preserves exact time and typed value");
    require(opacity != nullptr && opacity->keyframes.size() == 2 &&
                opacity->keyframes.back().time == time(3, 2) &&
                opacity->keyframes.back().value == 0.25,
            "opacity key preserves exact time and typed value");
    const auto opacityKeyId = opacity->keyframes.back().id;

    const auto beforeNoChange = session.snapshot().revision();
    require(session.setSelectedOpacity(0.25) && session.snapshot().revision() == beforeNoChange,
            "setting the exact existing key value is a successful no-change");
    require(session.undo(), "animated opacity edit is undoable");
    opacity = session.composition()->animationCurves().findScalar(opacityCurve);
    require(opacity != nullptr && opacity->keyframes.size() == 1,
            "undo removes only the inserted opacity key");
    require(session.redo(), "animated opacity edit is redoable");
    opacity = session.composition()->animationCurves().findScalar(opacityCurve);
    require(opacity != nullptr && opacity->keyframes.size() == 2 &&
                opacity->keyframes.back().id == opacityKeyId,
            "redo restores the exact stable key identity");
}

void testDrivenEditIsExplicitlyRejected() {
    auto newProject = document::makeNewProject("Driven Session", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    // ADAPTED (task S7): a driver source names the value-graph output it reads, and the kinds have
    // to connect -- so the fixture adds the Vector2 value node a position can actually be driven
    // from.
    commands::Transaction addValue("Add value node", document.snapshot().revision());
    addValue.emplace<commands::AddNode>(compositionId, std::string(document::kVector2ValueNodeType),
                                        document::Vec2d{200.0, 200.0});
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

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    QString rejection;
    QObject::connect(&session, &ui::CompositionSession::commandRejected, &session,
                     [&rejection](const QString& message) { rejection = message; });
    const auto revision = session.snapshot().revision();
    require(!session.setSelectedPosition(1.0, 2.0), "driven position edit is rejected");
    require(session.snapshot().revision() == revision,
            "driven position rejection cannot mutate project truth");
    require(rejection.contains(QStringLiteral("Disconnect")) &&
                rejection.contains(QStringLiteral("driven")),
            "driven rejection describes the required explicit transition");
}

void testComponentDiamondsAndSelections() {
    auto newProject = document::makeNewProject("Component Session", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);
    ui::CompositionSession session(document, stack, compositionId);

    const auto exactTime = time(2);
    require(session.keyframeDiamondState(ids.position, document::AnimationComponent::X,
                                         exactTime) == ui::KeyframeDiamondState::Constant,
            "a constant vector component reports the constant diamond state");
    require(session.toggleKeyframe(ids.position, document::AnimationComponent::X, exactTime),
            "toggling one vector component creates a component-scoped animation");
    const auto* parameter = session.composition()->parameters().find(ids.position);
    require(parameter != nullptr, "the component parameter remains addressable");
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    require(source != nullptr, "component toggling creates an animation source");
    const auto* curve = session.composition()->animationCurves().findVec2(source->curveId);
    require(curve != nullptr && curve->components[0].keyframes.size() == 1 &&
                curve->components[1].keyframes.empty(),
            "the first component toggle leaves the sibling component unkeyed");
    require(session.keyframeParameterState(ids.position, exactTime) ==
                ui::KeyframeParameterState::Some,
            "the parameter state distinguishes a partially keyed vector");
    require(session.keyframeDiamondState(ids.position, document::AnimationComponent::Y,
                                         exactTime) == ui::KeyframeDiamondState::AnimatedWithoutKey,
            "the sibling component reports animated-without-key");
    const auto xKeyId = curve->components[0].keyframes.front().id;
    session.selectKeyframe(source->curveId, document::AnimationComponent::X, xKeyId);
    const auto* selected = std::get_if<ui::KeyframeSelection>(&session.selection().primary);
    require(selected != nullptr && selected->component.has_value() &&
                *selected->component == document::AnimationComponent::X,
            "component selection carries its component identity through the session seam");

    require(session.toggleKeyframe(ids.position, document::AnimationComponent::Y, exactTime),
            "the sibling component can be keyed independently");
    require(session.keyframeParameterState(ids.position, exactTime) ==
                ui::KeyframeParameterState::All,
            "the aggregate parameter state becomes All when every component is keyed");
    const auto effective = session.effectiveVec2Value(ids.position);
    require(effective.has_value() && *effective == document::Vec2d{10.0, 20.0},
            "component animation keeps the unchanged effective vector value");
}

// --- Task S5, item 0: THE KEYFRAME GESTURE ------------------------------------------------------
//
// Before this task nothing in production constructed CreateAnimationForParameter at all: the
// diamonds only reported a source, so no artist gesture could make a parameter animate. These cases
// drive the gesture end to end through CompositionSession::toggleKeyframe(), which is exactly what
// a diamond click calls, and pin the four transitions and their undo granularity.
void testKeyframeGestureCreatesAndRemovesAnimation() {
    auto newProject = document::makeNewProject("Gesture Session", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);

    // A constant parameter shows an EMPTY diamond, and its constant is what the first click
    // captures.
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::Constant,
            "a constant parameter's diamond reads Constant");
    const auto* opacityParameter = session.parameterForSelection(document::kOpacityParameterRole);
    require(opacityParameter != nullptr, "the fixture exposes an opacity parameter");
    const auto constantOpacity = session.constantValue(opacityParameter->id);
    require(constantOpacity.has_value(), "the fixture opacity starts constant");
    // value_or, not a dereference: require() exits on failure, but that is a runtime fact the
    // static analyzer cannot see, and an unchecked optional access is a build error here.
    const double originalOpacity = constantOpacity.value_or(0.0);

    require(session.setCurrentTime(time(2, 1)), "the session moves to frame 2's exact time");
    const auto beforeFirstClick = session.snapshot().revision();
    require(session.toggleKeyframe(document::kOpacityParameterRole),
            "the first diamond click converts a constant parameter to an animation");
    require(session.snapshot().revision().value() == beforeFirstClick.value() + 1,
            "and the whole convert-and-key gesture is exactly ONE transaction, so it is one undo "
            "step -- CreateAnimationForParameter and the key land together");
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::AnimatedWithKey,
            "the diamond now reads Animated-with-key at the time that was clicked");

    const auto* parameter = session.parameterForSelection(document::kOpacityParameterRole);
    require(parameter != nullptr, "the opacity parameter stays addressable");
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    require(source != nullptr, "and it now carries an animation source");
    const auto curveId = source->curveId;
    {
        const auto* curve = session.composition()->animationCurves().findScalar(curveId);
        require(curve != nullptr && curve->keyframes.size() == 1 &&
                    curve->keyframes.front().time == time(2, 1) &&
                    curve->keyframes.front().value == originalOpacity,
                "the seeded key sits at the clicked time holding the parameter's own constant, so "
                "animating a parameter never changes the picture");
    }

    // Away from that key the diamond is OUTLINED, and a click there inserts one at the sampled
    // value.
    require(session.setCurrentTime(time(5, 1)), "the session moves to another frame");
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::AnimatedWithoutKey,
            "an animated parameter with no key at the current time reads Animated-without-key");
    require(session.toggleKeyframe(document::kOpacityParameterRole),
            "a click at a time with no key inserts one");
    {
        const auto* curve = session.composition()->animationCurves().findScalar(curveId);
        require(curve != nullptr && curve->keyframes.size() == 2 &&
                    curve->keyframes.back().time == time(5, 1) &&
                    curve->keyframes.back().value == originalOpacity,
                "the inserted key holds the curve's own exactly sampled value there, so inserting "
                "a key never moves the value either");
    }
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::AnimatedWithKey,
            "and the diamond fills once a key is there");

    // Clicking an existing key deletes it -- and the curve survives, because another key remains.
    const auto beforeDelete = session.snapshot().revision();
    require(session.toggleKeyframe(document::kOpacityParameterRole),
            "a click on an existing key deletes it");
    require(session.snapshot().revision().value() == beforeDelete.value() + 1,
            "in exactly one transaction");
    {
        const auto* curve = session.composition()->animationCurves().findScalar(curveId);
        require(curve != nullptr && curve->keyframes.size() == 1,
                "the curve keeps its remaining key");
    }

    // Deleting the LAST key converts back to a constant, holding that key's value.
    require(session.setCurrentTime(time(2, 1)), "the session returns to the remaining key's time");
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::AnimatedWithKey,
            "the remaining key is where it was put");
    const auto beforeFinalDelete = session.snapshot().revision();
    require(session.toggleKeyframe(document::kOpacityParameterRole),
            "clicking the curve's last key removes the animation");
    require(session.snapshot().revision().value() == beforeFinalDelete.value() + 1,
            "in exactly one transaction");
    require(session.composition()->animationCurves().find(curveId) == nullptr,
            "the orphaned curve is erased atomically with the source transition");
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::Constant,
            "and the diamond is empty again");
    require(session.constantValue(parameter->id) == constantOpacity,
            "the constant it returns to is the value that key carried");

    // Undo is pinned: four clicks, four undo steps, back to a constant parameter and no curve.
    require(session.undo() && session.undo() && session.undo() && session.undo(),
            "each of the four gesture transitions undoes on its own");
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::Constant,
            "undoing the whole gesture leaves the parameter exactly as constant as it started");
    require(session.composition()->animationCurves().records().empty(),
            "with no curve left behind");
}

// Task S5, items 0 and 1: the gesture reaches every animatable schema, including the colour and
// size ones that task S5 made animatable, and a driven parameter is refused rather than keyed.
void testKeyframeGestureReachesEveryAnimatableSchema() {
    auto newProject = document::makeNewProject("Breadth Session", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);

    ui::CompositionSession session(document, stack, compositionId);
    require(session.addTextLayer(QStringLiteral("Title"), QStringLiteral("Hello"), 48.0,
                                 core::Color4d{1.0, 0.5, 0.25, 1.0}),
            "a text layer is added for the breadth check");

    // Every role a text layer exposes that task S5 declares animatable, plus the transform ones.
    for (const auto role : {document::kPositionParameterRole, document::kAnchorParameterRole,
                            document::kScaleParameterRole, document::kRotationParameterRole,
                            document::kOpacityParameterRole, document::kTextSizeParameterRole,
                            document::kTextColorParameterRole}) {
        require(session.keyframeDiamondState(role) == ui::KeyframeDiamondState::Constant,
                "every animatable role starts with an empty diamond");
        require(session.toggleKeyframe(role), "and every one of them accepts the gesture");
        require(session.keyframeDiamondState(role) == ui::KeyframeDiamondState::AnimatedWithKey,
                "leaving a key at the current time");
    }
    // Text CONTENT has no diamond at all: a string cannot interpolate, so offering one would
    // promise a gesture that cannot exist.
    require(session.keyframeDiamondState(document::kTextParameterRole) ==
                ui::KeyframeDiamondState::Unsupported,
            "text content exposes no diamond");
    require(!session.toggleKeyframe(document::kTextParameterRole),
            "and refuses the gesture outright");

    // The colour curve really is a colour curve, and it holds the authored colour.
    const auto* color = session.parameterForSelection(document::kTextColorParameterRole);
    require(color != nullptr, "the text colour parameter resolves");
    const auto* colorSource = std::get_if<document::AnimationCurveSource>(&color->source);
    require(colorSource != nullptr, "and it is animated");
    const auto* colorCurve =
        session.composition()->animationCurves().findColor4(colorSource->curveId);
    require(colorCurve != nullptr && colorCurve->keyframes.size() == 1 &&
                colorCurve->keyframes.front().value == core::Color4d{1.0, 0.5, 0.25, 1.0},
            "a colour parameter gets a COLOUR curve seeded with its own authored colour");

    // An animated parameter's value edit at a time with no key inserts one (the AE rule), through
    // the same write path a constant edit uses.
    require(session.setCurrentTime(time(4, 1)), "the session moves off every key");
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::AnimatedWithoutKey,
            "there is no opacity key at the new time");
    require(session.effectiveScalarValue(document::kOpacityParameterRole).has_value(),
            "but the row still has a value to show -- the curve's sampled value -- so the field "
            "stays live and the edit below is reachable at all");
    require(session.setSelectedOpacity(0.5), "editing an animated value commits");
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::AnimatedWithKey,
            "and inserts a key at the current time, which is the AE behaviour");
}

// Task S5, item 2: the selected key's interpolation, through the session path the timeline menu
// uses.
void testSelectedKeyframeInterpolation() {
    auto newProject = document::makeNewProject("Easing Session", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    // The session already starts at exact zero, and setCurrentTime() reports "no change" for that,
    // so this fixture simply keys at the time it is already on.
    require(session.currentTime() == time(0), "the easing fixture starts at zero");
    require(session.toggleKeyframe(document::kOpacityParameterRole), "opacity animates");
    require(session.setCurrentTime(time(4, 1)), "and gains a second key");
    require(session.toggleKeyframe(document::kOpacityParameterRole), "at a later frame");

    const auto* parameter = session.parameterForSelection(document::kOpacityParameterRole);
    const auto* source = parameter == nullptr
                             ? nullptr
                             : std::get_if<document::AnimationCurveSource>(&parameter->source);
    require(source != nullptr, "the easing fixture curve resolves");
    const auto* curve = session.composition()->animationCurves().findScalar(source->curveId);
    require(curve != nullptr && curve->keyframes.size() == 2, "with two keys");
    const auto firstKey = curve->keyframes.front().id;
    const auto lastKey = curve->keyframes.back().id;

    session.selectKeyframe(source->curveId, firstKey);
    require(session.selectedKeyframeInterpolation() == document::KeyframeInterpolation::Linear,
            "a newly created key starts Linear");
    require(!session.selectedKeyframeIsFinal(), "and the first of two keys is not the final one");
    require(session.setSelectedKeyframeInterpolation(document::KeyframeInterpolation::EaseInOut),
            "the selected interior key accepts Ease In-Out");
    require(session.selectedKeyframeInterpolation() == document::KeyframeInterpolation::EaseInOut,
            "and reports it back");

    session.selectKeyframe(source->curveId, lastKey);
    require(session.selectedKeyframeIsFinal(),
            "the final key is reported as final, so a menu can disable what it cannot accept");
    require(!session.setSelectedKeyframeInterpolation(document::KeyframeInterpolation::Hold),
            "and the command layer refuses a non-Linear final interpolation");
}

// Task S5, item 0: the PARAMETER-keyed half of the gesture, which is what a node card's diamond
// calls. A card paints and keys its OWN node's parameter whether or not that node is selected, and
// clicking it must not move the selection -- otherwise clicking a key on one card would silently
// retarget the properties panel.
void testParameterKeyedGestureNeedsNoSelection() {
    auto newProject = document::makeNewProject("Canvas Gesture", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    session.clearSelection();
    require(std::holds_alternative<std::monostate>(session.selection().primary),
            "the canvas fixture starts with nothing selected");

    // With no selection at all, the role-keyed reader has nothing to resolve -- and the
    // parameter-keyed one still answers, because it needs only the parameter.
    require(session.keyframeDiamondState(document::kOpacityParameterRole) ==
                ui::KeyframeDiamondState::Unsupported,
            "the role-keyed diamond state is Unsupported with no selection");
    require(session.keyframeDiamondStateForParameter(ids.opacity) ==
                ui::KeyframeDiamondState::Constant,
            "while the parameter-keyed one reports the parameter's own state");

    require(session.setCurrentTime(time(3, 1)), "the session moves to frame 3");
    require(session.toggleKeyframeForParameter(ids.opacity),
            "and the parameter-keyed gesture converts it to an animation with no selection at all");
    require(std::holds_alternative<std::monostate>(session.selection().primary),
            "keying a parameter never changes the selection");
    require(session.keyframeDiamondStateForParameter(ids.opacity) ==
                ui::KeyframeDiamondState::AnimatedWithKey,
            "the key landed at the session time");
    require(session.effectiveScalarValue(ids.opacity).has_value(),
            "and the parameter-keyed value reader answers for an animated parameter too");

    // An invalid parameter is refused rather than silently doing nothing to something else.
    require(!session.toggleKeyframeForParameter(document::ParameterId::fromRaw(999999)),
            "an unknown parameter refuses the gesture");
}

// --- Task S5, item 3c: per-frame evaluation over a 10-frame animated composition -----------------
//
// The point of the whole slice: a key at frame 0 and a key at frame 9 must sample to a DIFFERENT,
// exactly predictable value at every one of the ten frames in between -- never an interpolation of
// the transport's idea of time, and never an accumulated one. This steps the session frame by frame
// through the same exact mapping the timeline ruler uses and asserts the sampled value at each.
void testSteppingEveryFrameSamplesItsOwnExactValue() {
    // A 10-frame composition at 24 fps: frame i is at exact time i/24.
    const auto durationResult = core::RationalTime::create(10, 24);
    require(durationResult.has_value(), "the ten-frame fixture duration is valid");
    const auto duration = durationResult.value_or(core::RationalTime::fromInteger(1));
    auto newProject = document::makeNewProject("Per Frame", "Main", duration);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);

    const auto* const context = session.composition();
    require(context != nullptr, "the ten-frame composition resolves");
    const auto frameRate = context->format().frameRate();
    require(ui::maxFrameIndex(frameRate, duration) == std::optional<std::uint64_t>(9),
            "a 10-frame composition at its own rate has frames 0 through 9");

    // Opacity: 0 at frame 0, 1 at frame 9, linear between. Ten frames, ten exact rational values.
    require(session.setSelectedOpacity(0.0), "the fixture opacity starts at zero");
    require(session.toggleKeyframe(document::kOpacityParameterRole),
            "a key at frame 0 converts opacity to an animation");
    const auto lastFrameTime = ui::frameTimeForIndex(frameRate, duration, 9);
    require(lastFrameTime.has_value(), "frame 9 has an exact time");
    require(session.setCurrentTime(lastFrameTime.value_or(core::RationalTime::fromInteger(0))),
            "the session steps to the last frame");
    require(session.setSelectedOpacity(1.0),
            "and editing the animated value there inserts the second key");

    for (std::uint64_t index = 0; index <= 9; ++index) {
        const auto frameTime = ui::frameTimeForIndex(frameRate, duration, index);
        require(frameTime.has_value(), "every frame index in range has an exact time");
        const auto exactTime = frameTime.value_or(core::RationalTime::fromInteger(-1));
        // setCurrentTime() reports "no change" when the session is already there, which it is for
        // frame 9 after the edit above -- the assertion below is about the SAMPLED VALUE, so the
        // session is simply moved and then read.
        static_cast<void>(session.setCurrentTime(exactTime));
        require(session.currentTime() == exactTime,
                "the session sits at the frame's own exact rational time");
        const auto sampled = session.effectiveScalarValue(document::kOpacityParameterRole);
        // The exact expected value: frame i is i/9 of the way from 0 to 1, and the sampler derives
        // that factor as an exact rational before rounding once to binary64 -- so this comparison
        // is exact, not approximate.
        const double expected = static_cast<double>(index) / 9.0;
        require(sampled.has_value(), "every frame samples a value");
        require(sampled.value_or(-1.0) == expected,
                "and each frame's sampled value is exactly its own fraction of the segment, so the "
                "ten frames really do carry ten different values");
        // The diamond agrees with the document at every frame: filled only where a key is.
        const auto expectedState = (index == 0 || index == 9)
                                       ? ui::KeyframeDiamondState::AnimatedWithKey
                                       : ui::KeyframeDiamondState::AnimatedWithoutKey;
        require(session.keyframeDiamondState(document::kOpacityParameterRole) == expectedState,
                "and the diamond is filled at exactly the two frames that carry a key");
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testAnimatedEditsUseExactSessionTime();
    testDrivenEditIsExplicitlyRejected();
    testComponentDiamondsAndSelections();
    testKeyframeGestureCreatesAndRemovesAnimation();
    testKeyframeGestureReachesEveryAnimatableSchema();
    testSelectedKeyframeInterpolation();
    testParameterKeyedGestureNeedsNoSelection();
    testSteppingEveryFrameSamplesItsOwnExactValue();
    return 0;
}
