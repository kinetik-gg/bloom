// ADR 0031: align and distribute as one undoable command. The geometry here is hand-built, exactly
// what a caller measures; the evaluated end-to-end cases (parented layers re-measured after the
// command, headless/delivered-frame parity) live in the scripting and UI tests.

#include "command_test_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_alignment_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace bloom::commands::test {
namespace {

using document::CompositionFormat;

struct Fixture final {
    Document document{
        document::makeNewProject("Project", "Main", core::RationalTime::fromInteger(10),
                                 CompositionFormat::create(1000, 500).value_or(CompositionFormat{}))
            .project};
    CommandStack stack{document};
    document::CompositionId composition = document.snapshot().project().compositions().front().id();

    struct Layer final {
        document::LayerId id;
        document::ParameterId position;
    };

    template <typename Operation, typename... Arguments> CommandResult run(Arguments&&... args) {
        Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(args)...);
        return stack.execute(std::move(transaction));
    }
    Layer solid(const char* name, const Vec2d position) {
        const auto result =
            run<AddSolidLayer>(composition, name, core::Color4d{1, 1, 1, 1}, position);
        return {result.outputId<document::LayerId>(kAddSolidLayerLayerOutput)
                    .value_or(document::LayerId{}),
                result.outputId<document::ParameterId>(kAddSolidLayerPositionParameterOutput)
                    .value_or(document::ParameterId{})};
    }
    [[nodiscard]] const document::Composition& current() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] std::optional<Vec2d> constant(const document::ParameterId id) const {
        const auto* record = current().parameters().find(id);
        const auto* value =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* vector = value ? std::get_if<Vec2d>(&value->value) : nullptr;
        return vector ? std::optional(*vector) : std::nullopt;
    }
    CommandResult align(LayerAlignmentRequest request, const char* label = "Align") {
        request.composition = composition;
        Transaction transaction(label, document.snapshot().revision());
        transaction.emplace<AlignLayers>(std::move(request));
        return stack.execute(std::move(transaction));
    }
};

[[nodiscard]] bool near(const double a, const double b) { return std::abs(a - b) < 1e-9; }
[[nodiscard]] bool near(const Vec2d a, const Vec2d b) { return near(a.x, b.x) && near(a.y, b.y); }

[[nodiscard]] LayerAlignmentGeometry box(const Fixture::Layer& layer, const double left,
                                         const double top, const double right, const double bottom,
                                         const Vec2d position) {
    return {.layer = layer.id, .bounds = {left, top, right, bottom}, .position = position};
}

[[nodiscard]] LayerAlignmentGeometry box(const Fixture::Layer& layer, const double left,
                                         const double top, const double right,
                                         const double bottom) {
    return {.layer = layer.id, .bounds = {left, top, right, bottom}, .position = std::nullopt};
}

void testEveryAlignmentAgainstEveryTarget(TestContext& test) {
    constexpr std::array arrangements{
        LayerArrangement::AlignLeft,           LayerArrangement::AlignHorizontalCentre,
        LayerArrangement::AlignRight,          LayerArrangement::AlignTop,
        LayerArrangement::AlignVerticalMiddle, LayerArrangement::AlignBottom};
    constexpr std::array targets{AlignmentTarget::Composition, AlignmentTarget::Selection,
                                 AlignmentTarget::KeyObject};
    for (const auto arrangement : arrangements) {
        for (const auto target : targets) {
            Fixture fixture;
            const auto a = fixture.solid("A", {100, 100});
            const auto b = fixture.solid("B", {400, 300});
            const auto key = fixture.solid("Key", {700, 200});
            const std::vector geometry{box(a, 50, 60, 150, 140), box(b, 300, 250, 500, 350),
                                       box(key, 650, 150, 750, 250)};
            // The expected target rectangle.
            AlignmentRect rect{0, 0, 1000, 500};
            if (target == AlignmentTarget::Selection)
                rect = {50, 60, 750, 350};
            if (target == AlignmentTarget::KeyObject)
                rect = {650, 150, 750, 250};
            const auto expected = [&](const AlignmentRect& bounds, const Vec2d position) {
                switch (arrangement) {
                case LayerArrangement::AlignLeft:
                    return Vec2d{position.x + rect.left - bounds.left, position.y};
                case LayerArrangement::AlignHorizontalCentre:
                    return Vec2d{position.x + rect.centreX() - bounds.centreX(), position.y};
                case LayerArrangement::AlignRight:
                    return Vec2d{position.x + rect.right - bounds.right, position.y};
                case LayerArrangement::AlignTop:
                    return Vec2d{position.x, position.y + rect.top - bounds.top};
                case LayerArrangement::AlignVerticalMiddle:
                    return Vec2d{position.x, position.y + rect.centreY() - bounds.centreY()};
                default:
                    return Vec2d{position.x, position.y + rect.bottom - bounds.bottom};
                }
            };
            const auto history = fixture.stack.size();
            const auto result = fixture.align({.composition = {},
                                               .time = {},
                                               .layers = {a.id, b.id, key.id},
                                               .arrangement = arrangement,
                                               .target = target,
                                               .keyLayer = target == AlignmentTarget::KeyObject
                                                               ? std::optional(key.id)
                                                               : std::nullopt,
                                               .geometry = geometry});
            const std::string label = std::string(layerArrangementId(arrangement)) + " to " +
                                      std::string(alignmentTargetId(target));
            test.expect(result.changed() && fixture.stack.size() == history + 1,
                        label + " is one undoable change");
            test.expect(
                near(*fixture.constant(a.position), expected(geometry[0].bounds, {100, 100})),
                label + " moves A onto the target");
            test.expect(
                near(*fixture.constant(b.position), expected(geometry[1].bounds, {400, 300})),
                label + " moves B onto the target");
            const auto keyExpected = target == AlignmentTarget::KeyObject
                                         ? Vec2d{700, 200}
                                         : expected(geometry[2].bounds, {700, 200});
            test.expect(near(*fixture.constant(key.position), keyExpected),
                        label + (target == AlignmentTarget::KeyObject
                                     ? " never moves the key layer"
                                     : " moves the third layer onto the target"));
        }
    }
}

void testDistribution(TestContext& test) {
    // Three layers of different widths: the outermost centres stay, the middle one is even.
    {
        Fixture fixture;
        const auto a = fixture.solid("A", {50, 100});
        const auto b = fixture.solid("B", {100, 100});
        const auto c = fixture.solid("C", {700, 100});
        const auto result = fixture.align(
            {.composition = {},
             .time = {},
             .layers = {c.id, a.id, b.id},
             .arrangement = LayerArrangement::DistributeHorizontalCentres,
             .target = AlignmentTarget::Selection,
             .keyLayer = {},
             .geometry = {box(a, 0, 0, 100, 50), box(b, 90, 0, 110, 60), box(c, 600, 0, 800, 70)}},
            "Distribute");
        test.expect(result.changed(), "distributing three layers changes the middle one");
        test.expect(near(*fixture.constant(a.position), {50, 100}) &&
                        near(*fixture.constant(c.position), {700, 100}),
                    "the outermost centres stay");
        test.expect(near(*fixture.constant(b.position), {375, 100}),
                    "the inner centre is evenly spaced, whatever its width");
    }
    // Four unevenly sized layers in an arbitrary request order.
    {
        Fixture fixture;
        std::vector<Fixture::Layer> layers;
        std::vector<LayerAlignmentGeometry> geometry;
        const std::array centres{100.0, 120.0, 130.0, 700.0};
        const std::array halves{50.0, 10.0, 100.0, 25.0};
        for (std::size_t index = 0; index < 4; ++index) {
            layers.push_back(fixture.solid("Layer", {0, centres[index]}));
            geometry.push_back(box(layers.back(), 0, centres[index] - halves[index], 10,
                                   centres[index] + halves[index], {0, centres[index]}));
        }
        const auto result =
            fixture.align({.composition = {},
                           .time = {},
                           .layers = {layers[3].id, layers[1].id, layers[0].id, layers[2].id},
                           .arrangement = LayerArrangement::DistributeVerticalCentres,
                           .target = AlignmentTarget::Selection,
                           .keyLayer = {},
                           .geometry = geometry},
                          "Distribute");
        test.expect(result.changed(), "distributing four uneven layers vertically applies");
        test.expect(near(fixture.constant(layers[1].position)->y, 300) &&
                        near(fixture.constant(layers[2].position)->y, 500) &&
                        near(fixture.constant(layers[0].position)->y, 100) &&
                        near(fixture.constant(layers[3].position)->y, 700),
                    "four uneven layers get centres 100, 300, 500, 700");
    }
    {
        // Composition: the outermost layers' outer edges land on the frame; Key Object keeps the
        // key fixed too.
        Fixture fixture;
        const auto a = fixture.solid("A", {200, 0});
        const auto b = fixture.solid("B", {300, 0});
        const auto c = fixture.solid("C", {400, 0});
        const std::vector geometry{box(a, 150, 0, 250, 10, {200, 0}),
                                   box(b, 280, 0, 320, 10, {300, 0}),
                                   box(c, 350, 0, 450, 10, {400, 0})};
        test.expect(fixture
                        .align({.composition = {},
                                .time = {},
                                .layers = {a.id, b.id, c.id},
                                .arrangement = LayerArrangement::DistributeHorizontalCentres,
                                .target = AlignmentTarget::Composition,
                                .keyLayer = {},
                                .geometry = geometry})
                        .changed(),
                    "distributing across the composition applies");
        test.expect(near(*fixture.constant(a.position), {50, 0}) &&
                        near(*fixture.constant(c.position), {950, 0}) &&
                        near(*fixture.constant(b.position), {500, 0}),
                    "the outer edges reach the frame and the centre is between");
        test.expect(fixture.stack.undo().changed(), "the composition distribution undoes");
        const auto d = fixture.solid("D", {900, 0});
        auto withKey = geometry;
        withKey.push_back(box(d, 880, 0, 920, 10, {900, 0}));
        test.expect(fixture
                        .align({.composition = {},
                                .time = {},
                                .layers = {a.id, c.id, d.id},
                                .arrangement = LayerArrangement::DistributeHorizontalCentres,
                                .target = AlignmentTarget::KeyObject,
                                .keyLayer = b.id,
                                .geometry = withKey})
                        .changed(),
                    "distributing around a key applies");
        // Fixed: A (200), key B (300), D (900); C sits between B and D.
        test.expect(near(*fixture.constant(b.position), {300, 0}) &&
                        near(*fixture.constant(a.position), {200, 0}) &&
                        near(*fixture.constant(c.position), {600, 0}) &&
                        near(*fixture.constant(d.position), {900, 0}),
                    "the key stays fixed and the layers between fixed layers are even");
    }
    {
        Fixture fixture;
        const auto a = fixture.solid("A", {0, 0});
        const auto b = fixture.solid("B", {0, 0});
        const auto revision = fixture.document.snapshot().revision();
        const auto history = fixture.stack.size();
        const auto refused =
            fixture.align({.composition = {},
                           .time = {},
                           .layers = {a.id, b.id},
                           .arrangement = LayerArrangement::DistributeHorizontalCentres,
                           .target = AlignmentTarget::Selection,
                           .keyLayer = {},
                           .geometry = {box(a, 0, 0, 1, 1), box(b, 5, 0, 6, 1)}},
                          "Distribute");
        test.expect(refused.status == CommandStatus::Rejected &&
                        fixture.document.snapshot().revision() == revision &&
                        fixture.stack.size() == history,
                    "distributing fewer than three layers is refused with no transaction");
        test.expect(!refused.operationFailures.empty() &&
                        refused.operationFailures.front().issue.message.find("at least 3") !=
                            std::string::npos,
                    "the refusal says why");
    }
}

void testParentSpaceAndCarriedChildren(TestContext& test) {
    Fixture fixture;
    const auto parent = fixture.solid("Parent", {500, 250});
    const auto child = fixture.solid("Child", {10, 20});
    test.expect(fixture.run<SetLayerParent>(fixture.composition, child.id, parent.id).succeeded(),
                "the child is parented");
    const auto childPosition = *fixture.constant(child.position);
    // The parent is rotated 90 degrees and scaled 2x: parent (x, y) -> composition (-2y, 2x).
    auto childGeometry = box(child, 100, 100, 200, 150);
    childGeometry.parentLinear = {0.0, 2.0, -2.0, 0.0};
    test.expect(fixture
                    .align({.composition = {},
                            .time = {},
                            .layers = {child.id},
                            .arrangement = LayerArrangement::AlignLeft,
                            .target = AlignmentTarget::Composition,
                            .keyLayer = {},
                            .geometry = {childGeometry}})
                    .changed(),
                "a parented layer aligns");
    // A composition delta of (-100, 0) is (0, 50) in the parent's space.
    test.expect(
        near(*fixture.constant(child.position), {childPosition.x + 0.0, childPosition.y + 50.0}),
        "the delta is converted into the parent's space");
    test.expect(fixture.stack.undo().changed(), "the parented alignment undoes");

    // Moving the parent and the child together: the child takes only what the parent leaves.
    const auto parentPosition = *fixture.constant(parent.position);
    test.expect(fixture
                    .align({.composition = {},
                            .time = {},
                            .layers = {parent.id, child.id},
                            .arrangement = LayerArrangement::AlignTop,
                            .target = AlignmentTarget::Composition,
                            .keyLayer = {},
                            .geometry = {box(parent, 0, 40, 10, 50), childGeometry}})
                    .changed(),
                "aligning a parent and its child applies");
    test.expect(near(*fixture.constant(parent.position), {parentPosition.x, parentPosition.y - 40}),
                "the parent moves by its own delta");
    // The child needs -100 vertically; the parent carries -40 of it, the rest is (-60) in
    // composition space = (-30, 0) in the parent's space.
    test.expect(near(*fixture.constant(child.position), {childPosition.x - 30, childPosition.y}),
                "the child moves by the remainder in its parent's space");

    auto singular = childGeometry;
    singular.parentLinear = {0, 0, 0, 0};
    const auto rejected = fixture.align({.composition = {},
                                         .time = {},
                                         .layers = {child.id},
                                         .arrangement = LayerArrangement::AlignLeft,
                                         .target = AlignmentTarget::Composition,
                                         .keyLayer = {},
                                         .geometry = {singular}});
    test.expect(rejected.status == CommandStatus::Rejected,
                "a singular parent leaves nothing to move and is refused");
}

void testKeyedAndStaticPosition(TestContext& test) {
    Fixture fixture;
    const auto animated = fixture.solid("Animated", {100, 100});
    const auto still = fixture.solid("Still", {300, 100});
    // The animated layer starts at 1 s, so composition time 3 is its key time 2.
    test.expect(fixture
                    .run<SetLayerTiming>(
                        fixture.composition, animated.id, core::RationalTime::fromInteger(1),
                        core::RationalTime::fromInteger(0), core::RationalTime::fromInteger(10))
                    .succeeded(),
                "the animated layer is offset");
    test.expect(fixture
                    .run<CreateAnimationForParameter>(fixture.composition, animated.position,
                                                      core::RationalTime::fromInteger(0))
                    .changed(),
                "Position is animated");
    test.expect(fixture
                    .run<SetKeyframeAtTimeForParameter>(fixture.composition, animated.position,
                                                        core::RationalTime::fromInteger(4),
                                                        Vec2d{500, 100})
                    .changed(),
                "a second key is added");
    const auto curveOf = [&]() -> const document::Vec2AnimationCurve* {
        const auto* record = fixture.current().parameters().find(animated.position);
        const auto* source = std::get_if<document::AnimationCurveSource>(&record->source);
        return source ? fixture.current().animationCurves().findVec2(source->curveId) : nullptr;
    };
    const auto before = *curveOf();
    const auto history = fixture.stack.size();
    // At key time 2 the animated layer sits at (300, 100) halfway between its keys.
    const auto result = fixture.align({.composition = {},
                                       .time = core::RationalTime::fromInteger(3),
                                       .layers = {animated.id, still.id},
                                       .arrangement = LayerArrangement::AlignTop,
                                       .target = AlignmentTarget::Composition,
                                       .keyLayer = {},
                                       .geometry = {box(animated, 250, 80, 350, 120, {300, 100}),
                                                    box(still, 250, 60, 350, 140, {300, 100})}});
    test.expect(result.changed() && fixture.stack.size() == history + 1,
                "keyed and static layers align in one undoable change");
    const auto* after = curveOf();
    const auto keysAt = [](const document::Vec2AnimationCurve& curve, const std::size_t axis) {
        return curve.components[axis].keyframes;
    };
    bool keyed = after != nullptr;
    for (std::size_t axis = 0; keyed && axis < 2; ++axis) {
        const auto keys = keysAt(*after, axis);
        keyed = keys.size() == 3 && keys[1].time == core::RationalTime::fromInteger(2) &&
                keys[0] == keysAt(before, axis)[0] && keys[2] == keysAt(before, axis)[1];
    }
    test.expect(keyed, "a key is inserted at the layer's own key time and the others stay");
    test.expect(after != nullptr && near(after->components[0].keyframes[1].value, 300) &&
                    near(after->components[1].keyframes[1].value, 20),
                "the new key is the sampled value plus the delta");
    test.expect(near(*fixture.constant(still.position), {300, 40}), "a static Position is set");

    // Undo and redo round-trip exactly.
    const auto aligned = fixture.document.snapshot();
    test.expect(fixture.stack.undo().changed() && *curveOf() == before &&
                    near(*fixture.constant(still.position), {300, 100}),
                "one undo restores every layer");
    test.expect(fixture.stack.redo().changed() && *curveOf() == *after &&
                    fixture.current().parameters().find(still.position)->source ==
                        aligned.project()
                            .findComposition(fixture.composition)
                            ->parameters()
                            .find(still.position)
                            ->source,
                "redo restores the aligned values");
}

void testSkipsAndRejections(TestContext& test) {
    Fixture fixture;
    const auto free = fixture.solid("Free", {100, 100});
    const auto locked = fixture.solid("Locked", {200, 100});
    const auto driven = fixture.solid("Driven", {300, 100});
    test.expect(fixture.run<SetLayerLocked>(fixture.composition, locked.id, true).changed(),
                "a layer is locked");
    const auto node = fixture.run<AddNode>(
        fixture.composition, std::string(document::kVector2ValueNodeType), Vec2d{0, 0});
    const auto valueNode = node.outputId<document::NodeId>(kAddNodeOutput);
    test.expect(
        valueNode.has_value() &&
            fixture
                .run<SetParameterSource>(fixture.composition, driven.position,
                                         document::DriverBindingSource{
                                             *valueNode, std::string(document::kValuePortName)})
                .changed(),
        "a layer's Position is driven");
    const std::vector geometry{box(free, 50, 50, 150, 150), box(locked, 150, 50, 250, 150),
                               box(driven, 250, 50, 350, 150)};
    const auto result = fixture.align({.composition = {},
                                       .time = {},
                                       .layers = {free.id, locked.id, driven.id},
                                       .arrangement = LayerArrangement::AlignRight,
                                       .target = AlignmentTarget::Composition,
                                       .keyLayer = {},
                                       .geometry = geometry});
    test.expect(result.changed() && near(*fixture.constant(free.position), {950, 100}),
                "the free layer still moves");
    test.expect(near(*fixture.constant(locked.position), {200, 100}),
                "the locked layer does not move");
    test.expect(result.warnings.size() == 2 &&
                    result.warnings[0].issue.message.find("locked") != std::string::npos &&
                    result.warnings[1].issue.message.find("driven") != std::string::npos,
                "each skipped layer is reported with its reason");
    test.expect(result.outputId<document::LayerId>(kAlignLayersSkippedLayerOutput) == locked.id &&
                    result.outputId<document::LayerId>(kAlignLayersMovedLayerOutput) == free.id,
                "the moved and skipped layers are outputs");
    const auto plan =
        planLayerAlignment(fixture.current(), {.composition = fixture.composition,
                                               .time = {},
                                               .layers = {locked.id, driven.id},
                                               .arrangement = LayerArrangement::AlignLeft,
                                               .target = AlignmentTarget::Composition,
                                               .keyLayer = {},
                                               .geometry = geometry});
    test.expect(plan.skips.size() == 2 &&
                    plan.skips[0].reason == LayerAlignmentSkipReason::Locked &&
                    plan.skips[1].reason == LayerAlignmentSkipReason::DrivenPosition,
                "the plan names typed skip reasons");

    const auto revision = fixture.document.snapshot().revision();
    const auto history = fixture.stack.size();
    const auto rejected = fixture.align({.composition = {},
                                         .time = {},
                                         .layers = {locked.id, driven.id},
                                         .arrangement = LayerArrangement::AlignLeft,
                                         .target = AlignmentTarget::Composition,
                                         .keyLayer = {},
                                         .geometry = geometry});
    test.expect(rejected.status == CommandStatus::Rejected &&
                    fixture.document.snapshot().revision() == revision &&
                    fixture.stack.size() == history,
                "when every layer is skipped the command is refused with no transaction");
    test.expect(rejected.operationFailures.size() == 3,
                "the refusal carries one reason per skipped layer");

    const auto noBounds = fixture.align({.composition = {},
                                         .time = {},
                                         .layers = {free.id},
                                         .arrangement = LayerArrangement::AlignLeft,
                                         .target = AlignmentTarget::Composition,
                                         .keyLayer = {},
                                         .geometry = {}});
    test.expect(noBounds.status == CommandStatus::Rejected &&
                    noBounds.operationFailures.back().issue.message.find("visible bounds") !=
                        std::string::npos,
                "a layer without evaluated bounds is skipped, and alone it is refused");
    const auto noKey = fixture.align({.composition = {},
                                      .time = {},
                                      .layers = {free.id},
                                      .arrangement = LayerArrangement::AlignLeft,
                                      .target = AlignmentTarget::KeyObject,
                                      .keyLayer = {},
                                      .geometry = geometry});
    test.expect(noKey.status == CommandStatus::Rejected, "Key Object needs a key layer");
    const auto keyWithoutBounds = fixture.align({.composition = {},
                                                 .time = {},
                                                 .layers = {free.id},
                                                 .arrangement = LayerArrangement::AlignLeft,
                                                 .target = AlignmentTarget::KeyObject,
                                                 .keyLayer = locked.id,
                                                 .geometry = {geometry[0]}});
    test.expect(keyWithoutBounds.status == CommandStatus::Rejected,
                "a key layer must have visible bounds");
    const auto onlyKey = fixture.align({.composition = {},
                                        .time = {},
                                        .layers = {free.id},
                                        .arrangement = LayerArrangement::AlignLeft,
                                        .target = AlignmentTarget::KeyObject,
                                        .keyLayer = free.id,
                                        .geometry = geometry});
    test.expect(onlyKey.status == CommandStatus::Rejected,
                "a request whose only layer is the key has nothing to move");
    const auto foreign = fixture.align({.composition = {},
                                        .time = {},
                                        .layers = {document::LayerId::fromRaw(999999)},
                                        .arrangement = LayerArrangement::AlignLeft,
                                        .target = AlignmentTarget::Composition,
                                        .keyLayer = {},
                                        .geometry = {}});
    test.expect(foreign.status == CommandStatus::Rejected, "an unknown layer is refused");
    const auto alreadyInPlace = fixture.align({.composition = {},
                                               .time = {},
                                               .layers = {free.id},
                                               .arrangement = LayerArrangement::AlignRight,
                                               .target = AlignmentTarget::Composition,
                                               .keyLayer = {},
                                               .geometry = {box(free, 900, 50, 1000, 150)}});
    test.expect(alreadyInPlace.status == CommandStatus::NoChange,
                "a layer already in place is no change");
}

void testIdsAndKeyTime(TestContext& test) {
    for (const auto arrangement :
         {LayerArrangement::AlignLeft, LayerArrangement::AlignHorizontalCentre,
          LayerArrangement::AlignRight, LayerArrangement::AlignTop,
          LayerArrangement::AlignVerticalMiddle, LayerArrangement::AlignBottom,
          LayerArrangement::DistributeHorizontalCentres,
          LayerArrangement::DistributeVerticalCentres})
        test.expect(layerArrangementFromId(layerArrangementId(arrangement)) == arrangement,
                    "every arrangement id round-trips");
    for (const auto target :
         {AlignmentTarget::Composition, AlignmentTarget::Selection, AlignmentTarget::KeyObject})
        test.expect(alignmentTargetFromId(alignmentTargetId(target)) == target,
                    "every target id round-trips");
    document::LayerTiming timing;
    timing.startTime = core::RationalTime::fromInteger(2);
    timing.speed = document::LayerSpeed::create(2, 1).value_or(document::LayerSpeed{});
    test.expect(layerKeyTimeOfCompositionTime(timing, core::RationalTime::fromInteger(5)) ==
                    core::RationalTime::fromInteger(6),
                "key time is (t - start) * speed");
    test.expect(layerKeyTimeOfCompositionTime({}, core::RationalTime::create(1, 3).value()) ==
                    core::RationalTime::create(1, 3),
                "identity timing keeps the composition time");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testEveryAlignmentAgainstEveryTarget(test);
        bloom::commands::test::testDistribution(test);
        bloom::commands::test::testParentSpaceAndCarriedChildren(test);
        bloom::commands::test::testKeyedAndStaticPosition(test);
        bloom::commands::test::testSkipsAndRejections(test);
        bloom::commands::test::testIdsAndKeyTime(test);
    } catch (const std::exception& error) {
        test.fail(std::string("unexpected test exception: ") + error.what());
    }
    return test.failures() == 0 ? 0 : 1;
}
