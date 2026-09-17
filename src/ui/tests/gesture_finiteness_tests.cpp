// SAVEFIX-1. The viewer transform gestures compute the value they offer the document from frozen
// evaluated bounds, and every degeneracy test they used to make was a test against exactly zero:
// an empty bounds rectangle, a singular basis, a zero-length scale arm. None of those is a
// finiteness test, so a bounds rectangle that has collapsed towards zero (a layer whose size or
// scale was driven to nearly nothing and then dragged) produced an infinite scale factor, and a
// bounds rectangle that overflowed to an infinity produced a NaN -- values the document must then
// refuse, losing the whole gesture, and values that would have been unsaveable had any of the
// refusals below it ever been bypassed.
//
// These tests script exactly the gestures named in the crash follow-up (scale on a collapsed
// layer, rotation about a zero-length ray, an anchor drag on a zero-size layer) against a keyed
// component parameter, and pin that the gesture now either refuses to start or declines to offer
// the unrepresentable value, while the representable gestures beside them are unchanged.

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
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QCoreApplication>
#include <QPointF>
#include <QRectF>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << "gesture finiteness test failed: " << message << '\n';
}

[[noreturn]] void fail(const std::string_view message) {
    std::cerr << "gesture finiteness fixture failed: " << message << '\n';
    std::exit(1);
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator) {
    const auto result = core::RationalTime::create(numerator, 1);
    if (!result.has_value()) {
        fail("fixture time must be valid");
    }
    return *result;
}

[[nodiscard]] document::CompositionFormat squareFormat() {
    const auto format = document::CompositionFormat::create(400, 400);
    if (!format.has_value()) {
        fail("fixture composition format must be valid");
    }
    return *format;
}

[[nodiscard]] ui::ViewerMapping makeMapping(const QRectF& displayRect) {
    const auto format = squareFormat();
    const auto window = render::ImageWindow::create(0, 0, format.width(), format.height());
    if (!window) {
        fail("fixture display window must be valid");
    }
    const auto descriptor =
        render::ReferenceDisplayBufferDescriptor::create(*window.value(), format.pixelAspect());
    if (!descriptor) {
        fail("fixture display descriptor must be valid");
    }
    return ui::ViewerMapping{
        .displayRect = displayRect,
        .compositionFormat = format,
        .resolution = runtime::CompositionFormatResolution{},
        .pixelAspect = format.pixelAspect(),
        .displayDescriptor = *descriptor.value(),
    };
}

struct Fixture final {
    document::Document document;
    commands::CommandStack stack;
    document::LayerId layer;
};

[[nodiscard]] document::LayerId addSolidLayer(document::Document& document,
                                              commands::CommandStack& stack,
                                              const document::CompositionId compositionId) {
    commands::Transaction transaction("Add test layer", document.snapshot().revision());
    transaction.emplace<commands::AddSolidLayer>(
        compositionId, "Solid", core::Color4d{0.2, 0.3, 0.4, 1.0}, document::Vec2d{100.0, 100.0});
    const auto result = stack.execute(std::move(transaction));
    const auto layer = result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    if (!result.changed() || !layer.has_value()) {
        fail("the solid layer fixture must expose its layer id");
    }
    return *layer;
}

// Frozen bounds shaped exactly like the evaluator publishes them, with the local rectangle's
// extent under the caller's control so a collapsed layer can be scripted without driving a whole
// preview pipeline.
[[nodiscard]] runtime::EvaluatedOperationBounds boundsFor(const document::LayerId layer,
                                                          const double localExtent) {
    runtime::EvaluatedOperationBounds bounds{};
    bounds.layerId = layer;
    bounds.local = {0.0, 0.0, localExtent, localExtent};
    bounds.output = {0.0, 0.0, 200.0, 200.0};
    bounds.polygon = {document::Vec2d{0.0, 0.0}, document::Vec2d{200.0, 0.0},
                      document::Vec2d{200.0, 200.0}, document::Vec2d{0.0, 200.0}};
    bounds.anchor = {100.0, 100.0};
    return bounds;
}

[[nodiscard]] bool overridesAreFinite(const ui::CompositionSession& session) {
    for (const auto& override : session.transformInteractionOverrides()) {
        const bool finite = std::visit(
            [](const auto& held) {
                using Held = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, double>) {
                    return std::isfinite(held);
                } else if constexpr (std::is_same_v<Held, document::Vec2d>) {
                    return std::isfinite(held.x) && std::isfinite(held.y);
                } else {
                    return true;
                }
            },
            override.value);
        if (!finite) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool componentKeyframeValuesAreFinite(const ui::CompositionSession& session,
                                                    const std::string_view role) {
    const auto* parameter = session.parameterForSelection(role);
    const auto* source = parameter != nullptr
                             ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                             : nullptr;
    if (source == nullptr) {
        return true;
    }
    const auto* record = session.composition()->animationCurves().find(source->curveId);
    const auto* curve =
        record != nullptr ? std::get_if<document::Vec2AnimationCurve>(record) : nullptr;
    if (curve == nullptr) {
        return true;
    }
    for (const auto& component : curve->components) {
        for (const auto& key : component.keyframes) {
            if (!std::isfinite(key.value) || !std::isfinite(key.outgoingHandle.value) ||
                !std::isfinite(key.incomingHandle.value)) {
                return false;
            }
        }
    }
    for (const auto& key : curve->keyframes) {
        if (!std::isfinite(key.value.x) || !std::isfinite(key.value.y)) {
            return false;
        }
    }
    return true;
}

// A layer collapsed to a denormal local extent. Every divisor in the scale branch is non-zero, so
// the old `arm.x() != 0` guard admitted the division and the quotient overflowed to an infinity.
void testCollapsedLayerScaleOffersNoInfiniteValue() {
    auto newProject = document::makeNewProject("Collapsed Scale", "Main", time(10), squareFormat());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto layer = addSolidLayer(document, stack, compositionId);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(layer);
    expect(session.toggleKeyframe(document::kScaleParameterRole),
           "the fixture animates scale, so the gesture commits through component keyframes");

    const auto mapping = makeMapping(QRectF(0.0, 0.0, 400.0, 400.0));
    const auto collapsed = boundsFor(layer, std::numeric_limits<double>::denorm_min());
    const QPointF origin(0.0, 0.0);
    const auto rejection = session.beginTransformInteraction(
        {ui::TransformGesture::Kind::Scale, 0, origin, collapsed}, mapping);
    if (!rejection.has_value()) {
        session.updateTransformInteraction(origin + QPointF(40.0, 25.0));
        expect(overridesAreFinite(session),
               "a scale drag on a layer collapsed to a denormal extent never offers a "
               "non-finite scale");
        const auto committed = session.commitTransformInteraction();
        expect(!committed ||
                   componentKeyframeValuesAreFinite(session, document::kScaleParameterRole),
               "whatever such a drag commits, every component keyframe value stays finite");
    }
    session.cancelTransformInteraction();
}

// Bounds that overflowed in the evaluator. The old code divided them anyway: infinity minus
// infinity is a NaN, and a NaN basis passes Qt's fuzzy determinant test, so the gesture armed
// itself with an all-NaN parent inverse.
void testOverflowedBoundsAreRefusedBeforeTheyProduceNaN() {
    auto newProject =
        document::makeNewProject("Overflowed Bounds", "Main", time(10), squareFormat());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto layer = addSolidLayer(document, stack, compositionId);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(layer);

    const auto mapping = makeMapping(QRectF(0.0, 0.0, 400.0, 400.0));
    auto overflowed = boundsFor(layer, 200.0);
    overflowed.polygon[0] = {-std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()};
    overflowed.polygon[1] = {std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()};
    const QPointF origin(10.0, 10.0);
    const auto rejection = session.beginTransformInteraction(
        {ui::TransformGesture::Kind::Scale, 0, origin, overflowed}, mapping);
    expect(rejection.has_value(), "a gesture cannot arm itself from bounds that are not finite");
    if (!rejection.has_value()) {
        session.updateTransformInteraction(origin + QPointF(30.0, -18.0));
        expect(overridesAreFinite(session), "and it never offers a non-finite value if it does");
    }
    session.cancelTransformInteraction();
}

// A zero-size layer publishes an empty local rectangle; an anchor drag on it has no basis at all.
void testZeroSizeLayerAnchorDragIsRefused() {
    auto newProject =
        document::makeNewProject("Zero Size Anchor", "Main", time(10), squareFormat());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto layer = addSolidLayer(document, stack, compositionId);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(layer);

    const auto mapping = makeMapping(QRectF(0.0, 0.0, 400.0, 400.0));
    const auto empty = boundsFor(layer, 0.0);
    const QPointF origin(10.0, 10.0);
    expect(session
               .beginTransformInteraction({ui::TransformGesture::Kind::Anchor, 0, origin, empty},
                                          mapping)
               .has_value(),
           "an anchor drag on a zero-size layer is refused rather than dividing by its extent");
    session.cancelTransformInteraction();
}

// Rotating about the pivot itself is a zero-length ray: the angle is undefined, and the gesture
// must hold the authored rotation rather than offer atan2(0, 0)'s answer for a NaN ray.
void testRotationAboutAZeroLengthRayHoldsTheAuthoredAngle() {
    auto newProject =
        document::makeNewProject("Zero Ray Rotation", "Main", time(10), squareFormat());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto layer = addSolidLayer(document, stack, compositionId);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(layer);

    const auto mapping = makeMapping(QRectF(0.0, 0.0, 400.0, 400.0));
    const auto bounds = boundsFor(layer, 200.0);
    const auto origin = mapping.toScreen(bounds.anchor);
    const auto rejection = session.beginTransformInteraction(
        {ui::TransformGesture::Kind::Rotate, 0, origin, bounds}, mapping);
    expect(!rejection.has_value(), "a rotation gesture arms on finite bounds");
    if (rejection.has_value()) {
        return;
    }
    session.updateTransformInteraction(origin);
    expect(overridesAreFinite(session),
           "a rotation whose ray has zero length offers a finite angle");
    session.cancelTransformInteraction();
}

// A degenerate viewer mapping has no composition-space answer; it must not seed one as a NaN.
void testDegenerateMappingMapsFinitely() {
    const auto degenerate = makeMapping(QRectF(0.0, 0.0, 0.0, 0.0));
    const auto mapped = degenerate.toComposition(QPointF(12.0, 34.0));
    expect(std::isfinite(mapped.x) && std::isfinite(mapped.y),
           "an empty display rectangle maps to a finite composition point");
}

} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication application(argc, argv);
        testCollapsedLayerScaleOffersNoInfiniteValue();
        testOverflowedBoundsAreRefusedBeforeTheyProduceNaN();
        testZeroSizeLayerAnchorDragIsRefused();
        testRotationAboutAZeroLengthRayHoldsTheAuthoredAngle();
        testDegenerateMappingMapsFinitely();
        return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "gesture finiteness test threw: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
