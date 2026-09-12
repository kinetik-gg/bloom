#include "command_test_support.hpp"

#include <bloom/commands/animation_operations.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

namespace bloom::commands::test {
namespace {

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("animation test time must be valid");
    }
    return *value;
}

template <typename Value>
[[nodiscard]] Value requireValue(std::optional<Value> value, const std::string_view message) {
    if (!value.has_value()) {
        throw std::logic_error(std::string(message));
    }
    return *value;
}

[[nodiscard]] const document::ParameterRecord& parameter(const document::Snapshot& snapshot,
                                                         const document::ParameterId id) {
    const auto* result = composition(snapshot).parameters().find(id);
    requireFixture(result != nullptr, "animation test parameter must remain addressable");
    return *result;
}

[[nodiscard]] document::AnimationCurveId animationSource(const document::Snapshot& snapshot,
                                                         const document::ParameterId id) {
    const auto* source =
        std::get_if<document::AnimationCurveSource>(&parameter(snapshot, id).source);
    requireFixture(source != nullptr, "animation test parameter must have an animation source");
    return source->curveId;
}

[[nodiscard]] double constantScalar(const document::Snapshot& snapshot,
                                    const document::ParameterId id) {
    const auto* source =
        std::get_if<document::ConstantValueSource>(&parameter(snapshot, id).source);
    requireFixture(source != nullptr, "animation test parameter must have a constant source");
    const auto* value = std::get_if<double>(&source->value);
    requireFixture(value != nullptr, "animation test constant must be scalar");
    return *value;
}

[[nodiscard]] const document::ScalarAnimationCurve&
scalarCurve(const document::Snapshot& snapshot, const document::AnimationCurveId id) {
    const auto* result = composition(snapshot).animationCurves().findScalar(id);
    requireFixture(result != nullptr, "scalar animation curve must remain addressable");
    return *result;
}

[[nodiscard]] const document::Vec2AnimationCurve& vec2Curve(const document::Snapshot& snapshot,
                                                            const document::AnimationCurveId id) {
    const auto* result = composition(snapshot).animationCurves().findVec2(id);
    requireFixture(result != nullptr, "Vec2 animation curve must remain addressable");
    return *result;
}

// --- Task S5 source-parameter fixture -----------------------------------------------------------
//
// The shared makeProject() graph carries a Layer Stack and two Layer Outputs; the schemas task S5
// made animatable -- a solid's colour, a text layer's size and colour -- live on SOURCE nodes,
// which that fixture has none of. Rather than widen the shared fixture (and with it every other
// command test's node set), this adds the two source nodes and their parameters locally, which is
// all these cases need: an animation command is keyed by parameter, not by connectivity.
inline constexpr document::NodeId kSolidSourceNodeId = document::NodeId::fromRaw(23);
inline constexpr document::NodeId kTextSourceNodeId = document::NodeId::fromRaw(24);
inline constexpr ParameterId kSolidColorId = ParameterId::fromRaw(70);
inline constexpr ParameterId kTextContentId = ParameterId::fromRaw(71);
inline constexpr ParameterId kTextSizeId = ParameterId::fromRaw(72);
inline constexpr ParameterId kTextColorId = ParameterId::fromRaw(73);
inline constexpr core::Color4d kFixtureSolidColor{0.25, 0.5, 0.75, 1.0};
inline constexpr core::Color4d kFixtureTextColor{1.0, 1.0, 1.0, 1.0};

[[nodiscard]] Project makeSourceProject() {
    auto project = makeProject();
    auto* composition = project.findComposition(kCompositionId);
    requireFixture(composition != nullptr, "source fixture composition must exist");
    requireFixture(composition->graph().addNode(
                       {kSolidSourceNodeId,
                        std::string(document::kSolidSourceNodeType),
                        {{std::string(document::kSolidColorParameterRole), kSolidColorId}},
                        document::kSolidSourceNodeSchemaVersion}),
                   "source fixture solid node must be accepted");
    requireFixture(composition->graph().addNode(
                       {kTextSourceNodeId,
                        std::string(document::kTextSourceNodeType),
                        {{std::string(document::kTextParameterRole), kTextContentId},
                         {std::string(document::kTextSizeParameterRole), kTextSizeId},
                         {std::string(document::kTextColorParameterRole), kTextColorId}},
                        document::kTextSourceNodeSchemaVersion}),
                   "source fixture text node must be accepted");
    requireFixture(composition->parameters().insert(
                       {kSolidColorId, std::string(document::kSolidColorParameterSchemaKey),
                        ConstantValueSource{kFixtureSolidColor}}),
                   "source fixture solid colour parameter must be accepted");
    requireFixture(composition->parameters().insert({kTextContentId,
                                                     std::string(document::kTextParameterSchemaKey),
                                                     ConstantValueSource{std::string("Title")}}),
                   "source fixture text content parameter must be accepted");
    requireFixture(composition->parameters().insert(
                       {kTextSizeId, std::string(document::kTextSizeParameterSchemaKey),
                        ConstantValueSource{document::kDefaultTextSizePixels}}),
                   "source fixture text size parameter must be accepted");
    requireFixture(composition->parameters().insert(
                       {kTextColorId, std::string(document::kTextColorParameterSchemaKey),
                        ConstantValueSource{kFixtureTextColor}}),
                   "source fixture text colour parameter must be accepted");
    requireFixture(project.validate().ok(), "source fixture project must validate");
    return project;
}

[[nodiscard]] const document::Color4AnimationCurve&
color4Curve(const document::Snapshot& snapshot, const document::AnimationCurveId id) {
    const auto* result = composition(snapshot).animationCurves().findColor4(id);
    requireFixture(result != nullptr, "colour animation curve must remain addressable");
    return *result;
}

// Task S5, item 1: a colour parameter takes a colour curve seeded from its own constant, and every
// colour key operation mirrors the scalar and Vec2 ones exactly.
void testColorKeyOperations(TestContext& test) {
    Document document(makeSourceProject());
    CommandStack stack(document);

    Transaction create("Animate solid colour", document.snapshot().revision());
    create.emplace<CreateAnimationForParameter>(kCompositionId, kSolidColorId, time(0, 1));
    const auto created = stack.execute(std::move(create));
    const auto curveId = created.outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    test.expect(created.changed() && curveId.has_value(),
                "a solid colour parameter accepts animation and reports its curve");
    if (!curveId.has_value()) {
        return;
    }
    {
        const auto& curve = color4Curve(document.snapshot(), *curveId);
        test.expect(curve.keyframes.size() == 1 &&
                        curve.keyframes.front().value == kFixtureSolidColor,
                    "the seeded key holds the parameter's own constant exactly, so converting to "
                    "animation never changes the picture at the initial time");
    }

    Transaction insert("Insert colour key", document.snapshot().revision());
    insert.emplace<InsertColor4Keyframe>(kCompositionId, *curveId, time(1, 1),
                                         core::Color4d{1.0, 0.0, 0.0, 0.5},
                                         document::KeyframeInterpolation::EaseInOut);
    const auto inserted = stack.execute(std::move(insert));
    test.expect(inserted.changed(), "a colour key inserts at a free exact time");
    {
        const auto& curve = color4Curve(document.snapshot(), *curveId);
        test.expect(curve.keyframes.size() == 2 && curve.keyframes.back().outgoingInterpolation ==
                                                       document::KeyframeInterpolation::Linear,
                    "and the final key's interpolation stays canonical Linear");
    }

    // An alpha outside the unit interval is not a representable authoring colour, so the command
    // refuses it on exactly the terms a constant colour is refused.
    Transaction bad("Insert invalid colour key", document.snapshot().revision());
    bad.emplace<InsertColor4Keyframe>(kCompositionId, *curveId, time(3, 2),
                                      core::Color4d{0.0, 0.0, 0.0, 4.0});
    const auto rejected = stack.execute(std::move(bad));
    test.expect(!rejected.changed() && rejected.status == CommandStatus::Rejected,
                "a colour key outside the authoring-colour domain is rejected");

    // SetKeyframeAtTime's colour overload: updates the exact-time key, preserving its KeyframeId.
    const auto originalKeyId = color4Curve(document.snapshot(), *curveId).keyframes.front().id;
    Transaction set("Set colour at time", document.snapshot().revision());
    set.emplace<SetKeyframeAtTime>(kCompositionId, *curveId, time(0, 1),
                                   core::Color4d{0.1, 0.2, 0.3, 1.0});
    const auto setResult = stack.execute(std::move(set));
    test.expect(setResult.changed() &&
                    setResult.outputId<document::KeyframeId>(kKeyframeOutput) == originalKeyId,
                "the colour SetKeyframeAtTime updates the exact-time key and preserves its ID");

    // And back to a constant, carrying the chosen colour.
    Transaction revert("Freeze solid colour", document.snapshot().revision());
    revert.emplace<ConvertAnimationToConstant>(kCompositionId, kSolidColorId,
                                               core::Color4d{0.9, 0.8, 0.7, 1.0});
    const auto reverted = stack.execute(std::move(revert));
    const auto* constant = std::get_if<document::ConstantValueSource>(
        &parameter(document.snapshot(), kSolidColorId).source);
    const auto* colorValue =
        constant == nullptr ? nullptr : std::get_if<core::Color4d>(&constant->value);
    test.expect(reverted.changed() && colorValue != nullptr &&
                    *colorValue == core::Color4d{0.9, 0.8, 0.7, 1.0} &&
                    composition(document.snapshot()).animationCurves().find(*curveId) == nullptr,
                "converting a colour animation back to a constant erases the curve atomically");
}

// Task S5, item 1: text size joined the scalar-animatable set, and its SCHEMA domain -- not
// "scalar" -- is what bounds its keys.
void testTextSizeAnimationRespectsItsSchemaDomain(TestContext& test) {
    Document document(makeSourceProject());
    CommandStack stack(document);

    Transaction create("Animate text size", document.snapshot().revision());
    create.emplace<CreateAnimationForParameter>(kCompositionId, kTextSizeId, time(0, 1));
    const auto created = stack.execute(std::move(create));
    const auto curveId = created.outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    test.expect(created.changed() && curveId.has_value(),
                "text size accepts animation and gets a SCALAR curve");
    if (!curveId.has_value()) {
        return;
    }
    test.expect(composition(document.snapshot()).animationCurves().findScalar(*curveId) != nullptr,
                "and that curve really is the scalar kind");

    Transaction tooLarge("Oversized text size key", document.snapshot().revision());
    tooLarge.emplace<InsertScalarKeyframe>(kCompositionId, *curveId, time(1, 1),
                                           document::kMaximumTextSizePixels + 1.0);
    test.expect(!stack.execute(std::move(tooLarge)).changed(),
                "a text size key past the schema maximum is rejected");

    Transaction zero("Zero text size key", document.snapshot().revision());
    zero.emplace<InsertScalarKeyframe>(kCompositionId, *curveId, time(1, 1), 0.0);
    test.expect(!stack.execute(std::move(zero)).changed(),
                "and a zero em size is rejected, because the schema's lower bound is exclusive");

    Transaction valid("Text size key", document.snapshot().revision());
    valid.emplace<InsertScalarKeyframe>(kCompositionId, *curveId, time(1, 1),
                                        document::kMaximumTextSizePixels);
    test.expect(stack.execute(std::move(valid)).changed(),
                "while the inclusive maximum itself is accepted");

    // A rotation key is still free to wind: the domain belongs to the schema, not to the kind.
    Transaction rotation("Animate rotation", document.snapshot().revision());
    rotation.emplace<CreateAnimationForParameter>(kCompositionId, kFirstRotationId, time(0, 1));
    const auto rotationCurve = stack.execute(std::move(rotation))
                                   .outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    test.expect(rotationCurve.has_value(), "rotation still animates");
    if (rotationCurve.has_value()) {
        Transaction wind("Wind rotation", document.snapshot().revision());
        wind.emplace<InsertScalarKeyframe>(kCompositionId, *rotationCurve, time(1, 1), 1080.0);
        test.expect(stack.execute(std::move(wind)).changed(),
                    "and a rotation key may pass three full turns");
    }

    // Text CONTENT remains constant-only.
    Transaction content("Animate text content", document.snapshot().revision());
    content.emplace<CreateAnimationForParameter>(kCompositionId, kTextContentId, time(0, 1));
    const auto refusedContent = stack.execute(std::move(content));
    test.expect(!refusedContent.changed() && refusedContent.status == CommandStatus::Rejected,
                "text content refuses animation: a string has no interpolation");
}

// Task S5, item 2: SetKeyframeInterpolation moves one key's outgoing mode and nothing else.
void testSetKeyframeInterpolation(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);

    Transaction create("Animate opacity", document.snapshot().revision());
    create.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId, time(0, 1));
    const auto curveId = stack.execute(std::move(create))
                             .outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    test.expect(curveId.has_value(), "the interpolation fixture animates opacity");
    if (!curveId.has_value()) {
        return;
    }
    Transaction second("Second opacity key", document.snapshot().revision());
    second.emplace<InsertScalarKeyframe>(kCompositionId, *curveId, time(2, 1), 0.0);
    static_cast<void>(stack.execute(std::move(second)));

    const auto& before = scalarCurve(document.snapshot(), *curveId);
    const auto firstKeyId = before.keyframes.front().id;
    const auto lastKeyId = before.keyframes.back().id;
    const auto firstTime = before.keyframes.front().time;
    const auto firstValue = before.keyframes.front().value;

    Transaction ease("Ease first key", document.snapshot().revision());
    ease.emplace<SetKeyframeInterpolation>(kCompositionId, *curveId, firstKeyId,
                                           document::KeyframeInterpolation::EaseInOut);
    const auto eased = stack.execute(std::move(ease));
    const auto& after = scalarCurve(document.snapshot(), *curveId);
    test.expect(eased.changed() && after.keyframes.front().outgoingInterpolation ==
                                       document::KeyframeInterpolation::EaseInOut,
                "an interior key accepts Ease In-Out");
    test.expect(after.keyframes.front().id == firstKeyId &&
                    after.keyframes.front().time == firstTime &&
                    after.keyframes.front().value == firstValue,
                "and its identity, exact time and value are untouched bit-for-bit");

    Transaction again("Ease first key again", document.snapshot().revision());
    again.emplace<SetKeyframeInterpolation>(kCompositionId, *curveId, firstKeyId,
                                            document::KeyframeInterpolation::EaseInOut);
    const auto repeated = stack.execute(std::move(again));
    test.expect(!repeated.changed() && repeated.status == CommandStatus::NoChange,
                "setting the mode a key already carries is a committing no-change");

    Transaction final("Hold the final key", document.snapshot().revision());
    final.emplace<SetKeyframeInterpolation>(kCompositionId, *curveId, lastKeyId,
                                            document::KeyframeInterpolation::Hold);
    const auto refused = stack.execute(std::move(final));
    test.expect(!refused.changed() && refused.status == CommandStatus::Rejected,
                "the FINAL key's interpolation is canonical Linear, so anything else is refused "
                "rather than silently normalized");

    test.expect(
        stack.undo().changed() &&
            scalarCurve(document.snapshot(), *curveId).keyframes.front().outgoingInterpolation ==
                document::KeyframeInterpolation::Linear,
        "and an interpolation change is one undo step of its own");
}

void testCreateAnimationOutputsUndoAndRedo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();
    const auto initialTime = time(1, 3);

    Transaction create("Animate opacity", original.revision());
    create.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId, initialTime);
    const auto created = stack.execute(std::move(create));
    const auto curveId = created.outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    const auto keyframeId = created.outputId<document::KeyframeId>(kKeyframeOutput);
    test.expect(created.changed() && curveId.has_value() && keyframeId.has_value(),
                "creating animation returns durable curve and keyframe IDs");
    if (!curveId.has_value() || !keyframeId.has_value()) {
        return;
    }

    const auto animated = document.snapshot();
    const auto& curve = scalarCurve(animated, *curveId);
    test.expect(animationSource(animated, kOpacityId) == *curveId && curve.keyframes.size() == 1 &&
                    curve.keyframes.front().id == *keyframeId &&
                    curve.keyframes.front().time == initialTime &&
                    curve.keyframes.front().value == 1.0 &&
                    curve.keyframes.front().outgoingInterpolation ==
                        document::KeyframeInterpolation::Linear,
                "opacity animation seeds one exact Linear scalar key from the constant");
    test.expect(stack.size() == 1, "creation is one undoable transaction");

    test.expect(stack.undo().changed(), "animation creation is undoable");
    const auto undone = document.snapshot();
    test.expect(constantScalar(undone, kOpacityId) == 1.0 &&
                    composition(undone).animationCurves().find(*curveId) == nullptr,
                "undo restores the constant and removes the curve declaration");
    auto highWater = document.draft(undone);
    test.expect(highWater.ids().allocateAnimationCurve() > curveId &&
                    highWater.ids().allocateKeyframe() > keyframeId,
                "undo preserves animation allocator high-water");

    test.expect(stack.redo().changed(), "animation creation is redoable");
    const auto redone = document.snapshot();
    test.expect(animationSource(redone, kOpacityId) == *curveId &&
                    scalarCurve(redone, *curveId) == curve,
                "redo restores the exact curve, key, time, and value");

    Document positionDocument(makeProject());
    CommandStack positionStack(positionDocument);
    Transaction createPosition("Animate position", positionDocument.snapshot().revision());
    createPosition.emplace<CreateAnimationForParameter>(kCompositionId, kFirstPositionId,
                                                        core::RationalTime::fromInteger(-2));
    const auto positionResult = positionStack.execute(std::move(createPosition));
    const auto positionCurveId =
        positionResult.outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    test.expect(positionResult.changed() && positionCurveId.has_value(),
                "position animation creates a typed Vec2 curve");
    if (positionCurveId.has_value()) {
        const auto positionSnapshot = positionDocument.snapshot();
        const auto& positionCurve = vec2Curve(positionSnapshot, *positionCurveId);
        test.expect(positionCurve.keyframes.size() == 1 &&
                        positionCurve.keyframes.front().time ==
                            core::RationalTime::fromInteger(-2) &&
                        positionCurve.keyframes.front().value == document::Vec2d{0.0, 0.0},
                    "position creation preserves exact out-of-range time and Vec2 value");
    }

    auto unsupportedProject = makeProject();
    auto* unsupportedComposition = unsupportedProject.findComposition(kCompositionId);
    requireFixture(
        unsupportedComposition != nullptr &&
            unsupportedComposition->parameters().insert(
                {document::ParameterId::fromRaw(80), std::string(document::kTextParameterSchemaKey),
                 document::ConstantValueSource{std::string("Text")}}),
        "unsupported animation fixture must add a text parameter");
    Document unsupportedDocument(std::move(unsupportedProject));
    CommandStack unsupportedStack(unsupportedDocument);
    Transaction unsupported("Reject text animation", unsupportedDocument.snapshot().revision());
    unsupported.emplace<CreateAnimationForParameter>(
        kCompositionId, document::ParameterId::fromRaw(80), initialTime);
    test.expect(unsupportedStack.execute(std::move(unsupported)).status == CommandStatus::Rejected,
                "creation refuses a schema the shared animatable predicates do not name");
}

void testRejectedTransactionDoesNotConsumeIds(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto original = document.snapshot();

    Transaction rejected("Reject after allocation", original.revision());
    rejected.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId,
                                                  core::RationalTime::fromInteger(0));
    rejected.emplace<SetCompositionName>(document::CompositionId::fromRaw(9999), "Missing");
    const auto rejectedResult = stack.execute(std::move(rejected));
    test.expect(rejectedResult.status == CommandStatus::Rejected && stack.size() == 0 &&
                    document.snapshot().revision() == original.revision(),
                "a later failure rejects the whole allocation transaction");

    Transaction retry("Retry animation", original.revision());
    retry.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId,
                                               core::RationalTime::fromInteger(0));
    const auto retried = stack.execute(std::move(retry));
    test.expect(retried.outputId<document::AnimationCurveId>(kAnimationCurveOutput) ==
                        document::AnimationCurveId::fromRaw(1) &&
                    retried.outputId<document::KeyframeId>(kKeyframeOutput) ==
                        document::KeyframeId::fromRaw(1),
                "rejected transactions consume neither curve nor keyframe IDs");
}

void testScalarKeyOperations(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction create("Animate opacity", document.snapshot().revision());
    create.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId,
                                                core::RationalTime::fromInteger(0));
    const auto created = stack.execute(std::move(create));
    const auto curveId = created.outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    const auto initialKeyId = created.outputId<document::KeyframeId>(kKeyframeOutput);
    const auto curve = requireValue(curveId, "scalar operation fixture must create a curve ID");
    const auto initialKey =
        requireValue(initialKeyId, "scalar operation fixture must create a keyframe ID");

    Transaction createAgain("Reject already animated", document.snapshot().revision());
    createAgain.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId,
                                                     core::RationalTime::fromInteger(1));
    test.expect(stack.execute(std::move(createAgain)).status == CommandStatus::Rejected,
                "creation requires the parameter to still have a constant source");

    Transaction deleteLast("Reject final deletion", document.snapshot().revision());
    deleteLast.emplace<DeleteKeyframe>(kCompositionId, curve, initialKey);
    test.expect(stack.execute(std::move(deleteLast)).status == CommandStatus::Rejected,
                "generic deletion rejects the curve's final key");

    Transaction wrongKind("Reject Vec2 on scalar", document.snapshot().revision());
    wrongKind.emplace<InsertVec2Keyframe>(kCompositionId, curve, core::RationalTime::fromInteger(1),
                                          document::Vec2d{1.0, 2.0});
    test.expect(stack.execute(std::move(wrongKind)).status == CommandStatus::Rejected,
                "typed insertion rejects a mismatched curve kind");

    Transaction wrongDomain("Reject opacity domain", document.snapshot().revision());
    wrongDomain.emplace<InsertScalarKeyframe>(kCompositionId, curve,
                                              core::RationalTime::fromInteger(1), 1.25);
    test.expect(stack.execute(std::move(wrongDomain)).status == CommandStatus::Rejected,
                "scalar insertion rejects out-of-domain opacity before publication");

    Transaction insertEnd("Insert scalar end", document.snapshot().revision());
    insertEnd.emplace<InsertScalarKeyframe>(kCompositionId, curve,
                                            core::RationalTime::fromInteger(2), 0.75,
                                            document::KeyframeInterpolation::Hold);
    const auto insertedEnd = stack.execute(std::move(insertEnd));
    const auto endKeyId = insertedEnd.outputId<document::KeyframeId>(kKeyframeOutput);
    test.expect(endKeyId == document::KeyframeId::fromRaw(2),
                "rejected typed inserts do not consume keyframe IDs");

    Transaction insertInterior("Insert scalar interior", document.snapshot().revision());
    insertInterior.emplace<InsertScalarKeyframe>(kCompositionId, curve,
                                                 core::RationalTime::fromInteger(1), 0.5,
                                                 document::KeyframeInterpolation::Hold);
    const auto insertedInterior = stack.execute(std::move(insertInterior));
    const auto interiorKeyId = insertedInterior.outputId<document::KeyframeId>(kKeyframeOutput);
    const auto interiorKey = requireValue(interiorKeyId, "interior scalar key must be created");

    Transaction update("Update scalar key", document.snapshot().revision());
    update.emplace<UpdateScalarKeyframe>(kCompositionId, curve, interiorKey, time(3, 2), 0.25,
                                         document::KeyframeInterpolation::Hold);
    const auto updated = stack.execute(std::move(update));
    test.expect(updated.outputId<document::KeyframeId>(kKeyframeOutput) == interiorKeyId,
                "typed update preserves and returns the keyframe ID");

    Transaction occupied("Reject occupied move", document.snapshot().revision());
    occupied.emplace<UpdateScalarKeyframe>(kCompositionId, curve, interiorKey,
                                           core::RationalTime::fromInteger(2), 0.25);
    test.expect(stack.execute(std::move(occupied)).status == CommandStatus::Rejected,
                "typed update rejects moving onto another exact key time");

    Transaction setExisting("Set existing scalar", document.snapshot().revision());
    setExisting.emplace<SetKeyframeAtTime>(kCompositionId, curve, time(3, 2), 0.4);
    const auto setResult = stack.execute(std::move(setExisting));
    test.expect(setResult.outputId<document::KeyframeId>(kKeyframeOutput) == interiorKeyId,
                "SetKeyframeAtTime updates and returns an exact-time key");
    const auto setSnapshot = document.snapshot();
    const auto& setCurve = scalarCurve(setSnapshot, curve);
    const auto setKey =
        std::ranges::find(setCurve.keyframes, interiorKey, &document::ScalarKeyframe::id);
    test.expect(setKey != setCurve.keyframes.end() && setKey->value == 0.4 &&
                    setKey->outgoingInterpolation == document::KeyframeInterpolation::Hold,
                "exact-time upsert preserves an existing key's outgoing interpolation");

    const auto beforeNoChange = document.snapshot();
    const auto historyBeforeNoChange = stack.size();
    Transaction noChange("Set identical scalar", beforeNoChange.revision());
    noChange.emplace<SetKeyframeAtTime>(kCompositionId, curve, time(3, 2), 0.4);
    const auto noChangeResult = stack.execute(std::move(noChange));
    test.expect(noChangeResult.status == CommandStatus::NoChange &&
                    noChangeResult.outputId<document::KeyframeId>(kKeyframeOutput) ==
                        interiorKeyId &&
                    document.snapshot().revision() == beforeNoChange.revision() &&
                    stack.size() == historyBeforeNoChange,
                "idempotent upsert returns the existing ID without revision or history churn");

    Transaction setNew("Set new scalar", document.snapshot().revision());
    setNew.emplace<SetKeyframeAtTime>(kCompositionId, curve, core::RationalTime::fromInteger(3),
                                      0.1);
    const auto setNewResult = stack.execute(std::move(setNew));
    const auto newKeyId = setNewResult.outputId<document::KeyframeId>(kKeyframeOutput);
    const auto newKey = requireValue(newKeyId, "new exact-time scalar key must be created");

    Transaction erase("Delete scalar key", document.snapshot().revision());
    erase.emplace<DeleteKeyframe>(kCompositionId, curve, newKey);
    const auto erased = stack.execute(std::move(erase));
    const auto afterErase = document.snapshot();
    const auto& erasedCurve = scalarCurve(afterErase, curve);
    test.expect(erased.outputId<document::KeyframeId>(kKeyframeOutput) == newKeyId &&
                    std::ranges::find(erasedCurve.keyframes, newKey,
                                      &document::ScalarKeyframe::id) == erasedCurve.keyframes.end(),
                "generic deletion removes and reports the selected scalar key");
}

void testVec2KeyOperations(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction create("Animate position", document.snapshot().revision());
    create.emplace<CreateAnimationForParameter>(kCompositionId, kFirstPositionId,
                                                core::RationalTime::fromInteger(0));
    const auto created = stack.execute(std::move(create));
    const auto curveId = created.outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    const auto curve = requireValue(curveId, "Vec2 operation fixture must create a curve");

    Transaction setEnd("Set Vec2 end", document.snapshot().revision());
    setEnd.emplace<SetKeyframeAtTime>(kCompositionId, curve, core::RationalTime::fromInteger(4),
                                      document::Vec2d{4.0, 4.0});
    test.expect(stack.execute(std::move(setEnd)).changed(), "Vec2 upsert inserts a new key");

    Transaction insert("Insert Vec2 interior", document.snapshot().revision());
    insert.emplace<InsertVec2Keyframe>(kCompositionId, curve, core::RationalTime::fromInteger(2),
                                       document::Vec2d{2.0, 2.0},
                                       document::KeyframeInterpolation::Hold);
    const auto inserted = stack.execute(std::move(insert));
    const auto keyframeId = inserted.outputId<document::KeyframeId>(kKeyframeOutput);
    const auto keyframe = requireValue(keyframeId, "typed Vec2 insert must return a keyframe ID");

    Transaction update("Update Vec2 key", document.snapshot().revision());
    update.emplace<UpdateVec2Keyframe>(kCompositionId, curve, keyframe, time(5, 2),
                                       document::Vec2d{5.0, 6.0},
                                       document::KeyframeInterpolation::Hold);
    const auto updated = stack.execute(std::move(update));
    test.expect(updated.outputId<document::KeyframeId>(kKeyframeOutput) == keyframeId,
                "typed Vec2 update preserves and returns its ID");

    Transaction set("Set existing Vec2", document.snapshot().revision());
    set.emplace<SetKeyframeAtTime>(kCompositionId, curve, time(5, 2), document::Vec2d{9.0, 8.0});
    const auto setResult = stack.execute(std::move(set));
    const auto setSnapshot = document.snapshot();
    const auto& setCurve = vec2Curve(setSnapshot, curve);
    const auto key = std::ranges::find(setCurve.keyframes, keyframe, &document::Vec2Keyframe::id);
    test.expect(setResult.outputId<document::KeyframeId>(kKeyframeOutput) == keyframeId &&
                    key != setCurve.keyframes.end() && key->value == document::Vec2d{9.0, 8.0} &&
                    key->outgoingInterpolation == document::KeyframeInterpolation::Hold,
                "Vec2 exact-time upsert preserves ID and interpolation");

    Transaction wrongKind("Reject scalar on Vec2", document.snapshot().revision());
    wrongKind.emplace<InsertScalarKeyframe>(kCompositionId, curve,
                                            core::RationalTime::fromInteger(3), 0.5);
    test.expect(stack.execute(std::move(wrongKind)).status == CommandStatus::Rejected,
                "typed scalar insertion rejects a Vec2 curve");
}

void testAnimatedToConstantTransitionUndoRedo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction create("Animate opacity", document.snapshot().revision());
    create.emplace<CreateAnimationForParameter>(kCompositionId, kOpacityId,
                                                core::RationalTime::fromInteger(0));
    const auto created = stack.execute(std::move(create));
    const auto curveId = created.outputId<document::AnimationCurveId>(kAnimationCurveOutput);
    const auto curve = requireValue(curveId, "transition fixture must create a curve");

    Transaction wrongType("Reject wrong constant type", document.snapshot().revision());
    wrongType.emplace<ConvertAnimationToConstant>(kCompositionId, kOpacityId,
                                                  document::Vec2d{1.0, 2.0});
    test.expect(stack.execute(std::move(wrongType)).status == CommandStatus::Rejected &&
                    animationSource(document.snapshot(), kOpacityId) == curve,
                "animated-to-constant transition rejects a mismatched value kind atomically");

    const auto historyBefore = stack.size();
    Transaction convert("Convert to constant", document.snapshot().revision());
    convert.emplace<ConvertAnimationToConstant>(kCompositionId, kOpacityId, 0.33);
    const auto converted = stack.execute(std::move(convert));
    const auto constant = document.snapshot();
    test.expect(converted.changed() && stack.size() == historyBefore + 1 &&
                    constantScalar(constant, kOpacityId) == 0.33 &&
                    composition(constant).animationCurves().find(curve) == nullptr,
                "explicit transition changes the source and erases its orphan curve atomically");

    test.expect(stack.undo().changed(), "constant transition is one meaningful undo step");
    const auto undone = document.snapshot();
    test.expect(animationSource(undone, kOpacityId) == curve &&
                    composition(undone).animationCurves().findScalar(curve) != nullptr,
                "undo restores the exact animated source and curve");
    test.expect(stack.redo().changed(), "constant transition is redoable");
    const auto redone = document.snapshot();
    test.expect(constantScalar(redone, kOpacityId) == 0.33 &&
                    composition(redone).animationCurves().find(curve) == nullptr,
                "redo reapplies the constant and curve erasure");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testCreateAnimationOutputsUndoAndRedo(test);
        bloom::commands::test::testRejectedTransactionDoesNotConsumeIds(test);
        bloom::commands::test::testScalarKeyOperations(test);
        bloom::commands::test::testVec2KeyOperations(test);
        bloom::commands::test::testAnimatedToConstantTransitionUndoRedo(test);
        bloom::commands::test::testColorKeyOperations(test);
        bloom::commands::test::testTextSizeAnimationRespectsItsSchemaDomain(test);
        bloom::commands::test::testSetKeyframeInterpolation(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
