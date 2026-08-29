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

    commands::Transaction drive("Drive position", document.snapshot().revision());
    drive.emplace<commands::SetParameterSource>(
        compositionId, ids.position,
        document::DriverBindingSource{document::DriverBindingId::fromRaw(1)});
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

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testAnimatedEditsUseExactSessionTime();
    testDrivenEditIsExplicitlyRejected();
    return 0;
}
