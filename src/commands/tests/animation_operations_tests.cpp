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
                {document::ParameterId::fromRaw(64), std::string(document::kTextParameterSchemaKey),
                 document::ConstantValueSource{std::string("Text")}}),
        "unsupported animation fixture must add a text parameter");
    Document unsupportedDocument(std::move(unsupportedProject));
    CommandStack unsupportedStack(unsupportedDocument);
    Transaction unsupported("Reject text animation", unsupportedDocument.snapshot().revision());
    unsupported.emplace<CreateAnimationForParameter>(
        kCompositionId, document::ParameterId::fromRaw(64), initialTime);
    test.expect(unsupportedStack.execute(std::move(unsupported)).status == CommandStatus::Rejected,
                "creation accepts only constant position and opacity schemas");
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
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
