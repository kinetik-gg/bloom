// Issue #355: add/remove/connect/disconnect/parameter/keyframe operations publish the footprint the
// shared render-reachability helper proves, and fail closed to a whole render otherwise.
#include "command_test_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/document/value_nodes.hpp>

#include <iostream>
#include <set>

namespace bloom::commands::test {
namespace {
using namespace document;
using core::RationalTime;

[[nodiscard]] RationalTime t(const std::int64_t seconds) {
    return RationalTime::fromInteger(seconds);
}

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};
};

template <typename Op, typename... Args> CommandResult apply(Fixture& fixture, Args&&... args) {
    Transaction transaction("Edit", fixture.document.snapshot().revision());
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

[[nodiscard]] NodeId addNode(Fixture& fixture, const std::string_view type) {
    const auto result = apply<AddNode>(fixture, std::string(type), Vec2d{});
    const auto id = result.outputId<NodeId>(kAddNodeOutput);
    requireFixture(id.has_value(), "fixture node adds");
    return *id;
}

[[nodiscard]] ParameterId parameterOf(const Fixture& fixture, const NodeId node,
                                      const std::string_view role) {
    const auto* record = composition(fixture.document.snapshot()).graph().findNode(node);
    requireFixture(record != nullptr, "fixture node exists");
    for (const auto& binding : record->parameters)
        if (binding.role == role)
            return binding.parameterId;
    throw std::logic_error("fixture parameter role");
}

void describe(const CommandResult& result) {
    std::cerr << "  status=" << static_cast<int>(result.status)
              << " renderAffecting=" << result.renderAffecting << " footprint=";
    if (!result.affectedTimes)
        std::cerr << "whole";
    else
        for (const auto& interval : result.affectedTimes->intervals)
            std::cerr << '[' << interval.start.toSeconds() << ',' << interval.end.toSeconds()
                      << ')';
    for (const auto& failure : result.operationFailures)
        std::cerr << " issue=" << failure.issue.message;
    std::cerr << '\n';
}

[[nodiscard]] bool empty(const CommandResult& result) {
    const bool ok = result.changed() && result.renderAffecting &&
                    result.affectedTimes.has_value() &&
                    result.affectedTimes->compositionId == kCompositionId &&
                    result.affectedTimes->intervals.empty();
    if (!ok)
        describe(result);
    return ok;
}

[[nodiscard]] bool whole(const CommandResult& result) {
    const bool ok = result.changed() && result.renderAffecting && !result.affectedTimes.has_value();
    if (!ok)
        describe(result);
    return ok;
}

[[nodiscard]] bool spans(const CommandResult& result,
                         const std::vector<std::pair<std::int64_t, std::int64_t>>& expected) {
    bool ok = result.changed() && result.affectedTimes.has_value() &&
              result.affectedTimes->compositionId == kCompositionId &&
              result.affectedTimes->intervals.size() == expected.size();
    for (std::size_t index = 0; ok && index < expected.size(); ++index)
        ok = result.affectedTimes->intervals[index].start == t(expected[index].first) &&
             result.affectedTimes->intervals[index].end == t(expected[index].second);
    if (!ok)
        describe(result);
    return ok;
}

void testUnreachableStructuralEdits(TestContext& test) {
    Fixture fixture;
    const auto add = apply<AddNode>(fixture, std::string(kSolidSourceNodeType), Vec2d{});
    test.expect(empty(add), "adding an unconnected node publishes an empty footprint");
    test.expect(empty(fixture.stack.undo()) && empty(fixture.stack.redo()),
                "undo and redo replay the empty footprint");
    const auto solid = *add.outputId<NodeId>(kAddNodeOutput);
    const auto blur = addNode(fixture, "bloom.gaussian-blur");
    const auto value = addNode(fixture, kScalarValueNodeType);

    test.expect(empty(apply<ConnectPorts>(fixture, OutputPortRef{solid, "image"},
                                          InputPortRef{NodeInputRef{blur, "input"}})),
                "connecting two unreachable nodes is render-neutral");
    test.expect(
        empty(apply<ConnectPorts>(fixture, OutputPortRef{value, std::string(kValuePortName)},
                                  InputPortRef{NodeInputRef{blur, "radius"}})),
        "driving an unreachable node's parameter is render-neutral");
    test.expect(empty(apply<DisconnectInput>(fixture, InputPortRef{NodeInputRef{blur, "radius"}})),
                "undriving an unreachable parameter is render-neutral");
    test.expect(empty(apply<DisconnectInput>(fixture, InputPortRef{NodeInputRef{blur, "input"}})),
                "disconnecting unreachable nodes is render-neutral");
    test.expect(empty(apply<RemoveNodes>(fixture, std::set<NodeId>{solid, blur, value})),
                "deleting disconnected source, effect and value nodes keeps every frame");

    // Reaching the output is topology the helper does not bound: whole render.
    const auto reachable = addNode(fixture, kSolidSourceNodeType);
    test.expect(whole(apply<ConnectPorts>(fixture, OutputPortRef{reachable, "image"},
                                          InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})),
                "connecting into a reachable layer fails closed");
    test.expect(whole(apply<DisconnectInput>(
                    fixture, InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})),
                "disconnecting a reachable input fails closed");
    const auto driver = addNode(fixture, kScalarValueNodeType);
    test.expect(
        whole(apply<ConnectPorts>(
            fixture, OutputPortRef{driver, std::string(kValuePortName)},
            InputPortRef{NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)}})),
        "driving a reachable parameter fails closed");
    test.expect(whole(apply<DisconnectInput>(
                    fixture, InputPortRef{NodeInputRef{kFirstLayerNodeId,
                                                       std::string(kOpacityParameterRole)}})),
                "undriving a reachable parameter fails closed");
}

void testSoloAndMutedReachability(TestContext& test) {
    {
        // A solo boundary outside every stack is unreachable itself but excludes every other
        // layer; adding/removing it changes the compiled topology, so it is never neutral.
        Fixture fixture;
        const auto layer = apply<AddNode>(fixture, std::string(kLayerOutputNodeType), Vec2d{});
        test.expect(empty(layer), "an unstacked layer node is unreachable");
        const auto node = *layer.outputId<NodeId>(kAddNodeOutput);
        const auto layerId = *layer.outputId<LayerId>("layer");
        (void)apply<SetLayerSolo>(fixture, layerId, true);
        test.expect(whole(apply<RemoveNodes>(fixture, std::set<NodeId>{node})),
                    "removing an unreachable solo layer still changes the compiled stack");
    }
    {
        Fixture fixture;
        (void)apply<SetLayerSolo>(fixture, kSecondLayerId, true);
        test.expect(empty(apply<SetParameterSource>(fixture, kOpacityId, ConstantValueSource{0.5})),
                    "a solo-excluded layer's parameter edit is render-neutral");
    }
    {
        Fixture fixture;
        (void)apply<SetLayerEnabled>(fixture, kFirstLayerId, false);
        test.expect(empty(apply<SetParameterSource>(fixture, kOpacityId, ConstantValueSource{0.5})),
                    "a disabled layer's parameter edit is render-neutral");
    }
    {
        Fixture fixture;
        (void)apply<SetNodeMuted>(fixture, kLayerStackNodeId, true);
        test.expect(spans(apply<SetParameterSource>(fixture, kOpacityId, ConstantValueSource{0.5}),
                          {{0, 10}}),
                    "a muted stack passes its first layer through ungated");
        test.expect(
            empty(apply<SetParameterSource>(fixture, kSecondOpacityId, ConstantValueSource{0.5})),
            "a muted stack's later slot is not compiled");
    }
}

void testParameterSpans(TestContext& test) {
    Fixture fixture;
    (void)apply<SetLayerRange>(fixture, kFirstLayerId, t(1), t(9));
    const auto edit = apply<SetParameterSource>(fixture, kOpacityId, ConstantValueSource{0.5});
    test.expect(spans(edit, {{1, 9}}), "a layer parameter edit footprints the layer's span");
    test.expect(spans(fixture.stack.undo(), {{1, 9}}) && spans(fixture.stack.redo(), {{1, 9}}),
                "undo and redo replay the parameter footprint");

    (void)apply<SetLayerParent>(fixture, kSecondLayerId, kFirstLayerId);
    (void)apply<SetLayerRange>(fixture, kSecondLayerId, t(5), t(10));
    test.expect(spans(apply<SetParameterSource>(fixture, kFirstPositionId,
                                                ConstantValueSource{Vec2d{4.0, 0.0}}),
                      {{1, 10}}),
                "a parent edit also covers its child's span");

    // A transaction of several parameter edits is proven by ONE diff.
    const auto before = fixture.stack.work().parameterFootprintDiffs;
    Transaction transaction("Two edits", fixture.document.snapshot().revision());
    transaction.emplace<SetParameterSource>(kCompositionId, kOpacityId, ConstantValueSource{0.25});
    transaction.emplace<SetParameterSource>(kCompositionId, kSecondOpacityId,
                                            ConstantValueSource{0.25});
    const auto both = fixture.stack.execute(std::move(transaction));
    test.expect(spans(both, {{1, 10}}), "a multi-parameter transaction unions its spans");
    test.expect(fixture.stack.work().parameterFootprintDiffs == before + 1,
                "a multi-operation transaction diffs parameters exactly once");

    // A parameter edit mixed with a graph edit fails closed.
    Transaction mixed("Mixed", fixture.document.snapshot().revision());
    mixed.emplace<SetParameterSource>(kCompositionId, kOpacityId, ConstantValueSource{0.75});
    mixed.emplace<AddNode>(kCompositionId, std::string(kSolidSourceNodeType), Vec2d{});
    test.expect(whole(fixture.stack.execute(std::move(mixed))),
                "a parameter edit beside a topology change is a whole render");
}

void testKeyframeIntervals(TestContext& test) {
    Fixture fixture;
    (void)apply<SetLayerRange>(fixture, kFirstLayerId, t(1), t(9));
    const auto key = [&](const std::int64_t time, const double value) {
        return apply<SetKeyframeAtTimeForParameter>(fixture, kOpacityId, t(time), value);
    };
    test.expect(spans(apply<CreateAnimationForParameter>(fixture, kOpacityId, t(2)), {{1, 9}}),
                "turning a constant into animation covers the layer span");
    test.expect(spans(key(6, 1.0), {{2, 9}}),
                "a key appended after the last key covers from it on");
    test.expect(spans(key(4, 0.5), {{2, 6}}),
                "an inserted key covers its two neighbouring segments");
    test.expect(spans(key(4, 0.75), {{2, 6}}), "a re-valued key covers its two segments");
    test.expect(spans(fixture.stack.undo(), {{2, 6}}), "undo replays the key interval");
    (void)fixture.stack.redo();

    const auto* parameter = composition(fixture.document.snapshot()).parameters().find(kOpacityId);
    const auto curveId = std::get<AnimationCurveSource>(parameter->source).curveId;
    const auto* curve =
        composition(fixture.document.snapshot()).animationCurves().findScalar(curveId);
    requireFixture(curve != nullptr && curve->keyframes.size() == 3, "three keys");
    const auto first = curve->keyframes.front().id;
    test.expect(spans(apply<DeleteKeyframe>(fixture, curveId, first), {{1, 4}}),
                "deleting the first key covers the held lead-in to its neighbour");
    const auto middle =
        composition(fixture.document.snapshot()).animationCurves().findScalar(curveId)->keyframes;
    test.expect(spans(apply<MoveKeyframes>(
                          fixture, std::vector<KeyframeMove>{{{curveId, middle.front().id}, t(5)}}),
                      {{1, 6}}),
                "moving a key covers its old and new neighbourhoods");

    // An unreachable node's animation is render-neutral.
    const auto blur = addNode(fixture, "bloom.gaussian-blur");
    test.expect(empty(apply<CreateAnimationForParameter>(
                    fixture, parameterOf(fixture, blur, "radius"), t(3))),
                "animating an unconnected node keeps every frame");
}

void testCompositionSourceReference(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kCompositionSourceNodeType);
    const auto reference = parameterOf(fixture, source, "composition");
    test.expect(
        empty(apply<SetParameterSource>(fixture, reference, ConstantValueSource{std::int64_t{99}})),
        "an unconnected composition source reference is neutral");
    (void)apply<ConnectPorts>(fixture, OutputPortRef{source, "image"},
                              InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}});
    (void)apply<SetLayerRange>(fixture, kFirstLayerId, t(3), t(7));
    const auto timeOffset = parameterOf(fixture, source, "timeOffset");
    test.expect(
        spans(apply<SetParameterSource>(fixture, timeOffset, ConstantValueSource{1.0}), {{3, 7}}),
        "a reachable nested source parameter is confined to its layer");
}

void testAffectedCompositionScope(TestContext& test) {
    Fixture fixture;
    const auto solid = addNode(fixture, kSolidSourceNodeType);
    const auto rewire = apply<ConnectPorts>(fixture, OutputPortRef{solid, "image"},
                                            InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}});
    test.expect(whole(rewire) && rewire.affectedComposition == kCompositionId,
                "a whole-render node edit still names its composition");
    test.expect(fixture.stack.undo().affectedComposition == kCompositionId &&
                    fixture.stack.redo().affectedComposition == kCompositionId,
                "undo and redo replay the composition scope");
    test.expect(apply<SetParameterSource>(fixture, kOpacityId, ConstantValueSource{0.5})
                        .affectedComposition == kCompositionId,
                "a finite footprint implies its composition scope");

    Transaction mixed("Unscoped", fixture.document.snapshot().revision());
    mixed.emplace<AddNode>(kCompositionId, std::string(kSolidSourceNodeType), Vec2d{});
    mixed.emplace<SetLayerEnabled>(kCompositionId, kFirstLayerId, false);
    const auto unscoped = fixture.stack.execute(std::move(mixed));
    test.expect(whole(unscoped) && !unscoped.affectedComposition.has_value(),
                "an operation without a declared scope leaves the scope unknown");
}

// ADR 0024: layer timing edits and key edits on a retimed layer.
void testLayerTiming(TestContext& test) {
    Fixture fixture;
    (void)apply<SetLayerRange>(fixture, kFirstLayerId, t(1), t(4));
    test.expect(spans(apply<SetLayerTiming>(fixture, kFirstLayerId, t(2), t(3), t(6)), {{1, 6}}),
                "moving a layer covers its old and new spans");
    test.expect(spans(fixture.stack.undo(), {{1, 6}}) && spans(fixture.stack.redo(), {{1, 6}}),
                "undo and redo replay the move footprint");
    test.expect(spans(apply<SetLayerTiming>(fixture, kFirstLayerId, t(2), t(3), t(8)), {{6, 8}}),
                "trimming with the same mapping covers only the changed span");
    test.expect(spans(apply<SetLayerTimeModes>(fixture, kFirstLayerId,
                                               document::LayerTimeMode::Hold,
                                               document::LayerTimeMode::Loop),
                      {{3, 8}}),
                "a mode change covers the layer span");
    test.expect(spans(apply<CreateAnimationForParameter>(fixture, kOpacityId, t(0)), {{3, 8}}) &&
                    spans(apply<SetKeyframeAtTimeForParameter>(fixture, kOpacityId, t(1), 0.5),
                          {{3, 8}}),
                "key edits on a retimed layer cover its whole span");
    // The layer starts at 2: a key at layer time 1 between keys at 0 and 4 changes layer times
    // [0, 4), which is composition [2, 6) within the layer's span [3, 8).
    (void)apply<SetKeyframeAtTimeForParameter>(fixture, kOpacityId, t(4), 1.0);
    test.expect(spans(apply<SetKeyframeAtTimeForParameter>(fixture, kOpacityId, t(1), 0.25),
                      {{3, 6}}),
                "a key edit is projected through the layer's start time");
    const auto refused = apply<SetLayerTiming>(fixture, kFirstLayerId, t(0), t(20), t(30));
    test.expect(!refused.changed(), "a range wholly after the composition is refused");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testUnreachableStructuralEdits(test);
        bloom::commands::test::testSoloAndMutedReachability(test);
        bloom::commands::test::testParameterSpans(test);
        bloom::commands::test::testKeyframeIntervals(test);
        bloom::commands::test::testCompositionSourceReference(test);
        bloom::commands::test::testAffectedCompositionScope(test);
        bloom::commands::test::testLayerTiming(test);
    } catch (const std::exception& error) {
        test.fail(error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
