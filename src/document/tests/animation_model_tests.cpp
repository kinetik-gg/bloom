#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/validation.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::AnimationCurveId;
using bloom::document::AnimationCurveSource;
using bloom::document::AnimationCurveStore;
using bloom::document::CanonicalGraph;
using bloom::document::Color4AnimationCurve;
using bloom::document::Color4Keyframe;
using bloom::document::CommitStatus;
using bloom::document::Composition;
using bloom::document::CompositionId;
using bloom::document::Document;
using bloom::document::EdgeId;
using bloom::document::KeyframeId;
using bloom::document::KeyframeInterpolation;
using bloom::document::NodeId;
using bloom::document::NodeInputRef;
using bloom::document::ParameterId;
using bloom::document::Project;
using bloom::document::ProjectId;
using bloom::document::ScalarAnimationCurve;
using bloom::document::ScalarKeyframe;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::document::Vec2AnimationCurve;
using bloom::document::Vec2d;
using bloom::document::Vec2Keyframe;
using bloom::document::Vec3AnimationCurve;
using bloom::document::Vec3d;

class ExpectationContext final {
  public:
    bool expect(const bool condition, std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return true;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        return false;
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

template <typename Id> [[nodiscard]] constexpr Id id(const std::uint64_t value) noexcept {
    return Id::fromRaw(value);
}

[[nodiscard]] RationalTime time(const std::int64_t numerator, const std::int64_t denominator) {
    const auto value = RationalTime::create(numerator, denominator);
    if (!value.has_value()) {
        throw std::logic_error("Invalid rational time fixture");
    }
    return *value;
}

[[nodiscard]] bool hasIssue(const ValidationResult& result, const ValidationCode code,
                            const std::string_view path = {}) {
    return std::ranges::any_of(result.issues(), [&](const auto& issue) {
        return issue.code == code && (path.empty() || issue.path == path);
    });
}

[[nodiscard]] ScalarKeyframe
scalarKey(const std::uint64_t keyframeId, const RationalTime at, const double value,
          const KeyframeInterpolation interpolation = KeyframeInterpolation::Linear) {
    return {id<KeyframeId>(keyframeId), at, value, interpolation};
}

[[nodiscard]] Vec2Keyframe
vec2Key(const std::uint64_t keyframeId, const RationalTime at, const Vec2d value,
        const KeyframeInterpolation interpolation = KeyframeInterpolation::Linear) {
    return {id<KeyframeId>(keyframeId), at, value, interpolation};
}

[[nodiscard]] Composition emptyComposition(const std::uint64_t compositionValue,
                                           const std::uint64_t graphBase) {
    const auto stackId = id<NodeId>(graphBase);
    const auto outputId = id<NodeId>(graphBase + 1);
    CanonicalGraph graph(stackId);
    const bool built =
        graph.addNode({stackId,
                       std::string(bloom::document::kLayerStackNodeType),
                       {},
                       bloom::document::kLayerStackNodeSchemaVersion}) &&
        graph.addNode({outputId,
                       std::string(bloom::document::kCompositionOutputNodeType),
                       {},
                       bloom::document::kCompositionOutputNodeSchemaVersion}) &&
        graph.addEdge(
            {id<EdgeId>(graphBase),
             {stackId, std::string(bloom::document::kLayerStackOutputPort)},
             NodeInputRef{outputId, std::string(bloom::document::kCompositionOutputInputPort)}});
    graph.setCompositionOutput(
        {outputId, std::string(bloom::document::kCompositionOutputOutputPort)});
    if (!built) {
        throw std::logic_error("Could not create empty composition fixture");
    }
    return {id<CompositionId>(compositionValue), "Composition", RationalTime::fromInteger(10),
            std::move(graph)};
}

void addScalarAnimation(Composition& composition, const ParameterId parameterId,
                        std::string schemaKey, const AnimationCurveId curveId,
                        const KeyframeId keyframeId, const double value) {
    if (!composition.animationCurves().insert(ScalarAnimationCurve{
            curveId, {ScalarKeyframe{keyframeId, RationalTime::fromInteger(0), value}}}) ||
        !composition.parameters().insert(
            {parameterId, std::move(schemaKey), AnimationCurveSource{curveId}})) {
        throw std::logic_error("Could not create scalar animation fixture");
    }
}

void addVec2Animation(Composition& composition, const ParameterId parameterId,
                      const AnimationCurveId curveId, const KeyframeId keyframeId,
                      const Vec2d value) {
    if (!composition.animationCurves().insert(Vec2AnimationCurve{
            curveId, {Vec2Keyframe{keyframeId, RationalTime::fromInteger(0), value}}}) ||
        !composition.parameters().insert({parameterId,
                                          std::string(bloom::document::kPositionParameterSchemaKey),
                                          AnimationCurveSource{curveId}})) {
        throw std::logic_error("Could not create Vec2 animation fixture");
    }
}

void testStoreCanonicalizationAndMutation(ExpectationContext& expectations) {
    AnimationCurveStore store;
    expectations.expect(
        store.insert(ScalarAnimationCurve{
            id<AnimationCurveId>(20),
            {scalarKey(20, RationalTime::fromInteger(0), 0.25, KeyframeInterpolation::Hold),
             scalarKey(21, RationalTime::fromInteger(2), 0.75, KeyframeInterpolation::Hold)}}) &&
            store.insert(Vec2AnimationCurve{id<AnimationCurveId>(10),
                                            {vec2Key(10, RationalTime::fromInteger(0), {1.0, 2.0},
                                                     KeyframeInterpolation::Hold)}}),
        "typed animation curves insert with finite values and exact ordered times");
    expectations.expect(
        bloom::document::animationCurveId(store.records()[0]) == id<AnimationCurveId>(10) &&
            bloom::document::animationCurveId(store.records()[1]) == id<AnimationCurveId>(20),
        "curve records are stored canonically by AnimationCurveId");
    expectations.expect(
        store.findVec2(id<AnimationCurveId>(10))->keyframes.back().outgoingInterpolation ==
                KeyframeInterpolation::Linear &&
            store.findScalar(id<AnimationCurveId>(20))->keyframes.back().outgoingInterpolation ==
                KeyframeInterpolation::Linear,
        "the final outgoing interpolation is normalized to Linear");

    expectations.expect(
        store.insertKeyframe(id<AnimationCurveId>(20), scalarKey(24, RationalTime::fromInteger(3),
                                                                 1.0, KeyframeInterpolation::Hold)),
        "a key may be inserted after the prior final key");
    const auto* scalar = store.findScalar(id<AnimationCurveId>(20));
    expectations.expect(
        scalar != nullptr && scalar->keyframes[1].id == id<KeyframeId>(21) &&
            scalar->keyframes[1].outgoingInterpolation == KeyframeInterpolation::Linear &&
            scalar->keyframes[2].id == id<KeyframeId>(24) &&
            scalar->keyframes[2].outgoingInterpolation == KeyframeInterpolation::Linear &&
            store.eraseKeyframe(id<AnimationCurveId>(20), id<KeyframeId>(24)),
        "the former final key becomes a Linear segment and the new final key is canonical Linear");

    expectations.expect(
        store.insertKeyframe(id<AnimationCurveId>(20), scalarKey(22, RationalTime::fromInteger(1),
                                                                 0.5, KeyframeInterpolation::Hold)),
        "scalar key insertion preserves exact time ordering");
    scalar = store.findScalar(id<AnimationCurveId>(20));
    expectations.expect(scalar != nullptr && scalar->keyframes.size() == 3 &&
                            scalar->keyframes[1].id == id<KeyframeId>(22) &&
                            scalar->keyframes[1].outgoingInterpolation ==
                                KeyframeInterpolation::Hold,
                        "an interior key retains its outgoing interpolation");
    expectations.expect(
        !store.insertKeyframe(id<AnimationCurveId>(20), scalarKey(23, time(2, 2), 0.6)) &&
            !store.insertKeyframe(id<AnimationCurveId>(20),
                                  scalarKey(10, RationalTime::fromInteger(3), 0.6)) &&
            !store.insertKeyframe(id<AnimationCurveId>(20),
                                  vec2Key(23, RationalTime::fromInteger(3), {0.0, 0.0})),
        "exact-time collisions, project-local key ID reuse, and kind mismatch are rejected");

    expectations.expect(
        store.updateKeyframe(id<AnimationCurveId>(20),
                             scalarKey(22, time(3, 2), 0.625, KeyframeInterpolation::Hold)),
        "updating a key preserves its ID while changing exact time and value");
    scalar = store.findScalar(id<AnimationCurveId>(20));
    expectations.expect(scalar != nullptr && scalar->keyframes[1].id == id<KeyframeId>(22) &&
                            scalar->keyframes[1].time == time(3, 2) &&
                            scalar->keyframes[1].value == 0.625,
                        "updated keys remain in canonical exact-time order");
    expectations.expect(
        !store.updateKeyframe(id<AnimationCurveId>(20),
                              scalarKey(22, RationalTime::fromInteger(2), 0.5)) &&
            store.eraseKeyframe(id<AnimationCurveId>(20), id<KeyframeId>(21)) &&
            store.findScalar(id<AnimationCurveId>(20))->keyframes.back().outgoingInterpolation ==
                KeyframeInterpolation::Linear,
        "updates reject occupied times and deletion renormalizes the new final key");
    expectations.expect(store.eraseKeyframe(id<AnimationCurveId>(20), id<KeyframeId>(22)) &&
                            !store.eraseKeyframe(id<AnimationCurveId>(20), id<KeyframeId>(20)),
                        "the final remaining key cannot be deleted");
    expectations.expect(store.validate().ok(), "ordinary store mutations preserve all invariants");
}

void testStoreRejectsMalformedCurves(ExpectationContext& expectations) {
    AnimationCurveStore store;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto invalidInterpolation = std::bit_cast<KeyframeInterpolation>(std::uint8_t{255});
    expectations.expect(
        !store.insert(ScalarAnimationCurve{id<AnimationCurveId>(1), {}}) &&
            !store.insert(ScalarAnimationCurve{
                AnimationCurveId{}, {scalarKey(1, RationalTime::fromInteger(0), 0.0)}}) &&
            !store.insert(
                ScalarAnimationCurve{id<AnimationCurveId>(1),
                                     {scalarKey(1, RationalTime::fromInteger(1), 0.0),
                                      scalarKey(2, RationalTime::fromInteger(0), 1.0)}}) &&
            !store.insert(ScalarAnimationCurve{
                id<AnimationCurveId>(1),
                {scalarKey(1, time(1, 2), 0.0), scalarKey(2, time(2, 4), 1.0)}}) &&
            !store.insert(ScalarAnimationCurve{
                id<AnimationCurveId>(1), {scalarKey(1, RationalTime::fromInteger(0), nan)}}) &&
            !store.insert(
                Vec2AnimationCurve{id<AnimationCurveId>(1),
                                   {vec2Key(1, RationalTime::fromInteger(0),
                                            {0.0, std::numeric_limits<double>::infinity()})}}) &&
            !store.insert(ScalarAnimationCurve{
                id<AnimationCurveId>(1),
                {scalarKey(1, RationalTime::fromInteger(0), 0.0, invalidInterpolation)}}),
        "empty, invalid-ID, unordered, duplicate-time, non-finite, and invalid-mode curves reject");
}

void testCompositionAnimationValidation(ExpectationContext& expectations) {
    Project valid(id<ProjectId>(1), "Project");
    auto composition = emptyComposition(1, 10);
    addVec2Animation(composition, id<ParameterId>(1), id<AnimationCurveId>(1), id<KeyframeId>(1),
                     {10.0, 20.0});
    addScalarAnimation(composition, id<ParameterId>(2),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(2), id<KeyframeId>(2), 0.5);
    expectations.expect(valid.addComposition(std::move(composition)) && valid.validate().ok(),
                        "position and opacity accept local curves of their exact value kinds");

    auto missing = emptyComposition(1, 10);
    expectations.expect(
        missing.parameters().insert({id<ParameterId>(1),
                                     std::string(bloom::document::kOpacityParameterSchemaKey),
                                     AnimationCurveSource{id<AnimationCurveId>(99)}}) &&
            hasIssue(missing.validate(), ValidationCode::MissingReference,
                     "parameters[1].source.curveId"),
        "animation sources must resolve inside their owning composition");

    auto orphan = emptyComposition(1, 10);
    expectations.expect(
        orphan.animationCurves().insert(ScalarAnimationCurve{
            id<AnimationCurveId>(1), {scalarKey(1, RationalTime::fromInteger(0), 0.5)}}) &&
            hasIssue(orphan.validate(), ValidationCode::OrphanObject, "animationCurves[1]"),
        "unreferenced curve declarations are invalid");

    auto shared = emptyComposition(1, 10);
    addScalarAnimation(shared, id<ParameterId>(1),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(1), id<KeyframeId>(1), 0.5);
    expectations.expect(
        shared.parameters().insert({id<ParameterId>(2),
                                    std::string(bloom::document::kOpacityParameterSchemaKey),
                                    AnimationCurveSource{id<AnimationCurveId>(1)}}) &&
            hasIssue(shared.validate(), ValidationCode::SharedReference,
                     "parameters[2].source.curveId"),
        "version 1 rejects two parameters sharing one curve");

    auto wrongKind = emptyComposition(1, 10);
    addScalarAnimation(wrongKind, id<ParameterId>(1),
                       std::string(bloom::document::kPositionParameterSchemaKey),
                       id<AnimationCurveId>(1), id<KeyframeId>(1), 0.5);
    expectations.expect(hasIssue(wrongKind.validate(), ValidationCode::TypeMismatch,
                                 "parameters[1].source.curveId"),
                        "known schemas reject an animation curve of the wrong value kind");

    auto unsupported = emptyComposition(1, 10);
    addScalarAnimation(unsupported, id<ParameterId>(1),
                       std::string(bloom::document::kTextParameterSchemaKey),
                       id<AnimationCurveId>(1), id<KeyframeId>(1), 0.5);
    expectations.expect(
        hasIssue(unsupported.validate(), ValidationCode::InvalidValue, "parameters[1].source"),
        "constant-only built-in schemas reject animation sources");

    auto invalidOpacity = emptyComposition(1, 10);
    addScalarAnimation(invalidOpacity, id<ParameterId>(1),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(1), id<KeyframeId>(1), 1.25);
    expectations.expect(hasIssue(invalidOpacity.validate(), ValidationCode::InvalidValue,
                                 "animationCurves[1].keyframes[1].value"),
                        "opacity curve values retain their schema domain at every key");

    auto unknown = emptyComposition(1, 10);
    addScalarAnimation(unknown, id<ParameterId>(1), "com.example.animated-scalar",
                       id<AnimationCurveId>(1), id<KeyframeId>(1), -100.0);
    expectations.expect(
        hasIssue(unknown.validate(), ValidationCode::InvalidValue, "parameters[1].source"),
        "unknown schemas cannot silently opt into animation");
}

void testProjectGlobalAnimationIds(ExpectationContext& expectations) {
    Project curveCollision(id<ProjectId>(1), "Project");
    auto first = emptyComposition(1, 10);
    auto second = emptyComposition(2, 20);
    addScalarAnimation(first, id<ParameterId>(1),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(7), id<KeyframeId>(8), 0.0);
    addScalarAnimation(second, id<ParameterId>(2),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(7), id<KeyframeId>(9), 1.0);
    if (!curveCollision.addComposition(std::move(first)) ||
        !curveCollision.addComposition(std::move(second))) {
        throw std::logic_error("Could not build curve collision fixture");
    }
    expectations.expect(hasIssue(curveCollision.validate(), ValidationCode::DuplicateId,
                                 "compositions[2].animationCurves[7].id"),
                        "AnimationCurveId declarations are unique across compositions");

    Project keyCollision(id<ProjectId>(1), "Project");
    first = emptyComposition(1, 10);
    second = emptyComposition(2, 20);
    addScalarAnimation(first, id<ParameterId>(1),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(7), id<KeyframeId>(8), 0.0);
    addScalarAnimation(second, id<ParameterId>(2),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(9), id<KeyframeId>(8), 1.0);
    if (!keyCollision.addComposition(std::move(first)) ||
        !keyCollision.addComposition(std::move(second))) {
        throw std::logic_error("Could not build key collision fixture");
    }
    expectations.expect(hasIssue(keyCollision.validate(), ValidationCode::DuplicateId,
                                 "compositions[2].animationCurves[9].keyframes[8].id"),
                        "KeyframeId declarations are unique across compositions");

    Project crossCompositionReference(id<ProjectId>(1), "Project");
    first = emptyComposition(1, 10);
    second = emptyComposition(2, 20);
    if (!first.parameters().insert({id<ParameterId>(1),
                                    std::string(bloom::document::kOpacityParameterSchemaKey),
                                    AnimationCurveSource{id<AnimationCurveId>(9)}})) {
        throw std::logic_error("Could not build cross-composition reference fixture");
    }
    addScalarAnimation(second, id<ParameterId>(2),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(9), id<KeyframeId>(8), 1.0);
    if (!crossCompositionReference.addComposition(std::move(first)) ||
        !crossCompositionReference.addComposition(std::move(second))) {
        throw std::logic_error("Could not publish cross-composition reference fixture");
    }
    expectations.expect(
        hasIssue(crossCompositionReference.validate(), ValidationCode::MissingReference,
                 "compositions[1].parameters[1].source.curveId"),
        "a curve declared in another composition cannot satisfy a local animation source");
}

void testAnimationAllocatorAndPublication(ExpectationContext& expectations) {
    bloom::document::IdAllocator allocator;
    allocator.reserveExisting(id<KeyframeId>(99));
    expectations.expect(allocator.allocateKeyframe() == id<KeyframeId>(100) &&
                            allocator.allocateAnimationCurve() == id<AnimationCurveId>(1),
                        "Keyframe and AnimationCurve allocators advance independently");
    bloom::document::IdAllocator exhausted;
    exhausted.reserveExisting(id<KeyframeId>(std::numeric_limits<std::uint64_t>::max()));
    allocator.mergeHighWater(exhausted);
    expectations.expect(!allocator.allocateKeyframe().has_value(),
                        "Keyframe allocator exhaustion survives high-water merging");

    Project project(id<ProjectId>(1), "Project");
    auto composition = emptyComposition(1, 10);
    addScalarAnimation(composition, id<ParameterId>(1),
                       std::string(bloom::document::kOpacityParameterSchemaKey),
                       id<AnimationCurveId>(100), id<KeyframeId>(200), 0.0);
    if (!project.addComposition(std::move(composition))) {
        throw std::logic_error("Could not build publication fixture");
    }
    Document document(std::move(project));
    auto publishedDraft = document.draft(document.snapshot());
    expectations.expect(
        publishedDraft.ids().allocateAnimationCurve() == id<AnimationCurveId>(101) &&
            publishedDraft.ids().allocateKeyframe() == id<KeyframeId>(201),
        "document construction inventories curve and key declarations into allocator high-water");

    const auto historical = document.snapshot();
    auto advancingDraft = document.draft(historical);
    expectations.expect(
        advancingDraft.ids().allocateAnimationCurve() == id<AnimationCurveId>(101) &&
            advancingDraft.ids().allocateKeyframe() == id<KeyframeId>(201),
        "published allocator advances start above inventoried animation declarations");
    const auto advanced = document.commit(historical.revision(), std::move(advancingDraft));
    if (!advanced.committed() || !advanced.snapshot.has_value()) {
        throw std::logic_error("Could not publish allocator high-water fixture");
    }
    const auto restored = document.restore(advanced.snapshot->revision(), historical);
    if (!restored.committed() || !restored.snapshot.has_value()) {
        throw std::logic_error("Could not restore allocator high-water fixture");
    }
    auto restoredDraft = document.draft(*restored.snapshot);
    expectations.expect(restoredDraft.ids().allocateAnimationCurve() == id<AnimationCurveId>(102) &&
                            restoredDraft.ids().allocateKeyframe() == id<KeyframeId>(202),
                        "restore preserves published curve and keyframe allocator high-water");

    const auto before = document.snapshot();
    auto rejectedDraft = document.draft(before);
    const auto rejectedCurveId = rejectedDraft.ids().allocateAnimationCurve();
    const auto rejectedKeyframeId = rejectedDraft.ids().allocateKeyframe();
    auto* rejectedComposition = rejectedDraft.project().findComposition(id<CompositionId>(1));
    if (!rejectedCurveId.has_value() || !rejectedKeyframeId.has_value() ||
        rejectedComposition == nullptr ||
        !rejectedComposition->animationCurves().insert(ScalarAnimationCurve{
            *rejectedCurveId,
            {ScalarKeyframe{*rejectedKeyframeId, RationalTime::fromInteger(0), 1.0}}})) {
        throw std::logic_error("Could not build rejected allocation fixture");
    }
    const auto rejected = document.commit(before.revision(), std::move(rejectedDraft));
    auto retry = document.draft(document.snapshot());
    expectations.expect(rejected.status == CommitStatus::InvalidDraft &&
                            hasIssue(rejected.validation, ValidationCode::OrphanObject) &&
                            retry.ids().allocateAnimationCurve() == rejectedCurveId &&
                            retry.ids().allocateKeyframe() == rejectedKeyframeId,
                        "a rejected orphan-curve draft does not consume curve or keyframe IDs");
}

// --- Task S5: the Color4 curve kind and the EaseInOut mode ---------------------------------------
void testColor4CurveAndEasedInterpolation(ExpectationContext& expectations) {
    AnimationCurveStore store;
    expectations.expect(
        store.insert(Color4AnimationCurve{
            AnimationCurveId::fromRaw(1),
            {Color4Keyframe{KeyframeId::fromRaw(1), RationalTime::fromInteger(0),
                            Color4d{0.0, 0.5, 1.0, 1.0}, KeyframeInterpolation::EaseInOut},
             Color4Keyframe{KeyframeId::fromRaw(2), RationalTime::fromInteger(2),
                            Color4d{-0.25, 2.0, 0.0, 0.0}}}}),
        "a colour curve enters the store, HDR and negative channels included");
    expectations.expect(store.findColor4(AnimationCurveId::fromRaw(1)) != nullptr &&
                            store.findScalar(AnimationCurveId::fromRaw(1)) == nullptr &&
                            store.findVec2(AnimationCurveId::fromRaw(1)) == nullptr,
                        "and the three typed finders are mutually exclusive on it");
    expectations.expect(store.validate().ok(), "the colour curve validates as stored");

    // The authoring-colour domain IS the curve's key domain: an alpha outside the unit interval is
    // not a representable authoring colour, so the key never enters the store.
    expectations.expect(!store.insert(Color4AnimationCurve{
                            AnimationCurveId::fromRaw(2),
                            {Color4Keyframe{KeyframeId::fromRaw(3), RationalTime::fromInteger(0),
                                            Color4d{0.0, 0.0, 0.0, 1.5}}}}),
                        "a colour key whose alpha leaves the unit interval is refused");
    expectations.expect(
        !store.insert(Color4AnimationCurve{
            AnimationCurveId::fromRaw(3),
            {Color4Keyframe{KeyframeId::fromRaw(4), RationalTime::fromInteger(0),
                            Color4d{std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0}}}}),
        "and a non-finite channel is refused too");

    // Keyframe IDs stay project-global across kinds: a colour key may not reuse a scalar key's ID.
    expectations.expect(
        store.insert(ScalarAnimationCurve{
            AnimationCurveId::fromRaw(4),
            {ScalarKeyframe{KeyframeId::fromRaw(9), RationalTime::fromInteger(0), 1.0}}}),
        "a scalar curve coexists with a colour one");
    expectations.expect(
        !store.insertKeyframe(
            AnimationCurveId::fromRaw(1),
            Color4Keyframe{KeyframeId::fromRaw(9), RationalTime::fromInteger(1), Color4d{}}),
        "a colour key may not reuse a keyframe ID a scalar curve already holds");

    // Insert/update/erase behave exactly as the scalar and Vec2 overloads do, and the final key's
    // interpolation is re-normalized to Linear on every mutation.
    expectations.expect(
        store.insertKeyframe(AnimationCurveId::fromRaw(1),
                             Color4Keyframe{KeyframeId::fromRaw(5), RationalTime::fromInteger(4),
                                            Color4d{1.0, 1.0, 1.0, 1.0},
                                            KeyframeInterpolation::EaseInOut}),
        "a colour key inserts at a free exact time");
    const auto* curve = store.findColor4(AnimationCurveId::fromRaw(1));
    expectations.expect(curve != nullptr && curve->keyframes.size() == 3 &&
                            curve->keyframes.back().outgoingInterpolation ==
                                KeyframeInterpolation::Linear,
                        "and the new final key's interpolation is normalized to canonical Linear");
    expectations.expect(curve != nullptr && curve->keyframes[0].outgoingInterpolation ==
                                                KeyframeInterpolation::EaseInOut,
                        "while an interior eased key keeps its own mode");
    expectations.expect(
        !store.insertKeyframe(
            AnimationCurveId::fromRaw(1),
            Color4Keyframe{KeyframeId::fromRaw(6), RationalTime::fromInteger(4), Color4d{}}),
        "an occupied exact time refuses a colour insert, as it does every kind");
    expectations.expect(store.eraseKeyframe(AnimationCurveId::fromRaw(1), KeyframeId::fromRaw(5)) &&
                            store.findColor4(AnimationCurveId::fromRaw(1))->keyframes.size() == 2,
                        "erasing a colour key leaves the curve non-empty and ordered");
    expectations.expect(store.validate().ok(), "every colour mutation leaves the store canonical");
}

// Task S5: the animatable schema set, and which curve kind each member demands.
void testColorAndScalarSchemaOwnership(ExpectationContext& expectations) {
    using bloom::document::isAnimatableSchemaKey;
    using bloom::document::isColor4AnimatableSchemaKey;
    using bloom::document::isScalarAnimatableSchemaKey;
    using bloom::document::isScalarWithinSchemaDomain;
    using bloom::document::isVec2AnimatableSchemaKey;

    expectations.expect(
        isColor4AnimatableSchemaKey(bloom::document::kSolidColorParameterSchemaKey) &&
            isColor4AnimatableSchemaKey(bloom::document::kTextColorParameterSchemaKey),
        "both colour schemas are Color4-animatable");
    expectations.expect(isScalarAnimatableSchemaKey(bloom::document::kTextSizeParameterSchemaKey),
                        "text size joined the scalar-animatable set");
    expectations.expect(!isAnimatableSchemaKey(bloom::document::kTextParameterSchemaKey),
                        "text CONTENT stays constant-only: a string has no interpolation");
    expectations.expect(!isAnimatableSchemaKey("bloom.not.a.real.key"),
                        "and an unregistered key can never opt into animation");
    // No schema may claim two kinds at once, which is what keeps curve-kind validation total.
    for (const auto key :
         {bloom::document::kPositionParameterSchemaKey, bloom::document::kAnchorParameterSchemaKey,
          bloom::document::kScaleParameterSchemaKey, bloom::document::kRotationParameterSchemaKey,
          bloom::document::kOpacityParameterSchemaKey, bloom::document::kTextSizeParameterSchemaKey,
          bloom::document::kSolidColorParameterSchemaKey,
          bloom::document::kTextColorParameterSchemaKey}) {
        const int kinds = static_cast<int>(isVec2AnimatableSchemaKey(key)) +
                          static_cast<int>(isScalarAnimatableSchemaKey(key)) +
                          static_cast<int>(isColor4AnimatableSchemaKey(key));
        expectations.expect(kinds == 1, "every animatable schema demands exactly one curve kind");
    }

    // The scalar DOMAIN belongs to the schema, and one predicate answers for keys and constants
    // alike.
    expectations.expect(
        isScalarWithinSchemaDomain(bloom::document::kOpacityParameterSchemaKey, 1.0) &&
            !isScalarWithinSchemaDomain(bloom::document::kOpacityParameterSchemaKey, 1.5),
        "opacity keeps its unit domain");
    expectations.expect(
        isScalarWithinSchemaDomain(bloom::document::kRotationParameterSchemaKey, 450.0),
        "rotation may wind past a full turn");
    expectations.expect(
        !isScalarWithinSchemaDomain(bloom::document::kTextSizeParameterSchemaKey, 0.0) &&
            isScalarWithinSchemaDomain(bloom::document::kTextSizeParameterSchemaKey,
                                       bloom::document::kMaximumTextSizePixels) &&
            !isScalarWithinSchemaDomain(bloom::document::kTextSizeParameterSchemaKey,
                                        bloom::document::kMaximumTextSizePixels + 1.0),
        "and text size keeps its own exclusive-low, inclusive-high domain");
}

void testIndependentComponentCurves(ExpectationContext& expectations) {
    AnimationCurveStore store;
    const auto curveId = id<AnimationCurveId>(90);
    const auto inserted = store.insert(Vec3AnimationCurve{
        curveId,
        {},
        std::array{
            bloom::document::ComponentAnimationCurve{{
                ScalarKeyframe{id<KeyframeId>(91), RationalTime::fromInteger(0), 1.0},
            }},
            bloom::document::ComponentAnimationCurve{{
                ScalarKeyframe{id<KeyframeId>(92), RationalTime::fromInteger(0), 2.0},
            }},
            bloom::document::ComponentAnimationCurve{{
                ScalarKeyframe{id<KeyframeId>(93), RationalTime::fromInteger(0), 3.0},
            }},
        }});
    expectations.expect(inserted && store.validate().ok(),
                        "a Vec3 curve accepts one independent scalar key per component");
    const auto* x = store.findComponent(curveId, bloom::document::AnimationComponent::X);
    const auto* y = store.findComponent(curveId, bloom::document::AnimationComponent::Y);
    const auto* z = store.findComponent(curveId, bloom::document::AnimationComponent::Z);
    expectations.expect(x != nullptr && y != nullptr && z != nullptr &&
                            x->keyframes.front().value == 1.0 &&
                            y->keyframes.front().value == 2.0 && z->keyframes.front().value == 3.0,
                        "component lookup keeps X, Y and Z values separate");
    expectations.expect(
        store.insertKeyframe(curveId, bloom::document::AnimationComponent::Y,
                             ScalarKeyframe{id<KeyframeId>(94), RationalTime::fromInteger(1), 8.0,
                                            KeyframeInterpolation::EaseInOut}),
        "a component can add a key without requiring keys on its sibling components");
    expectations.expect(
        store.updateKeyframe(curveId, bloom::document::AnimationComponent::Y,
                             ScalarKeyframe{id<KeyframeId>(92), RationalTime::fromInteger(0), 2.0,
                                            KeyframeInterpolation::EaseInOut}),
        "an interior component key can choose its own interpolation");
    y = store.findComponent(curveId, bloom::document::AnimationComponent::Y);
    x = store.findComponent(curveId, bloom::document::AnimationComponent::X);
    expectations.expect(y != nullptr && y->keyframes.size() == 2 &&
                            y->keyframes.front().outgoingInterpolation ==
                                KeyframeInterpolation::EaseInOut &&
                            x != nullptr && x->keyframes.size() == 1,
                        "component interpolation and cardinality are independent");
    expectations.expect(
        store.eraseKeyframe(curveId, bloom::document::AnimationComponent::Y, id<KeyframeId>(92)) &&
            store.validate().ok(),
        "removing a component key leaves the other component curves intact");
    expectations.expect(store.findVec3(curveId)->components[1].keyframes.size() == 1 &&
                            store.findVec3(curveId)->components[0].keyframes.size() == 1,
                        "component deletion does not erase the Vec3 curve or sibling keys");
}

} // namespace

int main() {
    try {
        ExpectationContext expectations;
        testStoreCanonicalizationAndMutation(expectations);
        testStoreRejectsMalformedCurves(expectations);
        testCompositionAnimationValidation(expectations);
        testProjectGlobalAnimationIds(expectations);
        testAnimationAllocatorAndPublication(expectations);
        testColor4CurveAndEasedInterpolation(expectations);
        testColorAndScalarSchemaOwnership(expectations);
        testIndependentComponentCurves(expectations);
        return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
