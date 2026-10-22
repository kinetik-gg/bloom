// Task FIX1 item G: a value node's own primitive is animatable, and a parameter it drives samples
// it per frame. The gesture is the card's own keyframe diamond -- the same widget a layer row
// carries -- and the proof is the composited frame at each of ten frames.
#include "node_production_harness.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>

#include <utility>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
[[nodiscard]] core::RationalTime frame(const std::int64_t index) {
    const auto time = core::RationalTime::create(index, 24);
    if (!time.has_value())
        throw std::runtime_error("frame time");
    return *time;
}

void testVector3DrivenReadout() {
    App fixture;
    expect(addDefaultSolidLayer(fixture.session), "a solid layer for the Vector 3 readout");
    QCoreApplication::processEvents();
    const auto layer = fixture.nodeOfType(document::kLayerOutputNodeType);
    expect(layer.has_value(), "the Vector 3 fixture has a layer output");
    if (!layer)
        return;
    const auto vector = fixture.addByType(document::kVector3ValueNodeType, {-600, -200});
    const auto separate = fixture.addByType(document::kSeparateXyzNodeType, {-350, -200});
    const auto* record = fixture.session.composition()->graph().findNode(vector);
    document::ParameterId valueId;
    if (record)
        for (const auto& binding : record->parameters)
            if (binding.role == document::kValueParameterRole)
                valueId = binding.parameterId;
    expect(valueId.isValid(), "the Vector 3 value parameter is addressable");
    if (!valueId.isValid())
        return;
    auto* vectorOutput = fixture.outputSocket(vector);
    auto* vectorInput = fixture.namedSocket(separate, QStringLiteral("vector"), true);
    expect(vectorOutput && vectorInput,
           "the Vector 3 driver sockets are available");
    if (!(vectorOutput && vectorInput))
        return;
    fixture.drag(vectorOutput->scenePos(), vectorInput->scenePos());
    QCoreApplication::processEvents();
    expect(driverFor(fixture, separate, document::kVectorPortName) != nullptr,
           "Vector 3 drives Separate XYZ");
    // The first connection rebuilds the cards; reacquire sockets from the current scene.
    auto* xOutput = fixture.namedSocket(separate, QStringLiteral("x"), false);
    auto* opacityInput =
        fixture.namedSocket(*layer, QString::fromUtf8(document::kOpacityParameterRole), true);
    expect(xOutput && opacityInput, "Separate XYZ and opacity sockets are available");
    if (!(xOutput && opacityInput))
        return;
    fixture.drag(xOutput->scenePos(), opacityInput->scenePos());
    QCoreApplication::processEvents();
    expect(driverFor(fixture, *layer, document::kOpacityParameterRole) != nullptr,
           "Separate XYZ drives layer opacity");
    (void)fixture.session.setCurrentTime(frame(0));
    expect(fixture.session.currentTime() == frame(0), "Vector 3 starts at frame zero");
    expect(fixture.session.setParameterValue(valueId, document::Vec3d{0.2, 0.3, 0.4},
                                             QStringLiteral("Set Vector 3")),
           "the first Vector 3 value is authored");
    expect(fixture.session.toggleKeyframeForParameter(valueId), "Vector 3 receives its first key");
    expect(fixture.session.setCurrentTime(frame(10)), "Vector 3 moves to frame ten");
    expect(fixture.session.setParameterValue(valueId, document::Vec3d{0.8, 0.6, 0.1},
                                             QStringLiteral("Set Vector 3")),
           "the second Vector 3 value is keyed");
    const auto* opacity = parameterFor(fixture, *layer, document::kOpacityParameterRole);
    expect(opacity != nullptr, "the driven opacity parameter is addressable");
    if (!opacity)
        return;
    for (const auto [index, expected] : std::array<std::pair<std::int64_t, double>, 3>{
             std::pair{std::int64_t{0}, 0.2}, std::pair{std::int64_t{5}, 0.5},
             std::pair{std::int64_t{10}, 0.8}}) {
        expect(fixture.session.setCurrentTime(frame(index)), "readout moves to a keyed frame");
        fixture.session.refreshDrivenValues();
        const auto expectedText = QString::number(expected, 'g', 12);
        QElapsedTimer wait;
        wait.start();
        while (fixture.session.drivenValueText(opacity->id) != expectedText &&
               wait.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        const auto composited = composite(fixture, frame(index));
        expect(fixture.session.drivenValueText(opacity->id) == expectedText && composited.ok &&
                   closeTo(composited.centerPixel[3], expected),
               "animated Vector 3 readout matches the rendered Separate XYZ driver");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        expect(addDefaultSolidLayer(a.session), "a solid layer to drive");
        QCoreApplication::processEvents();
        const auto layerNode = a.nodeOfType(document::kLayerOutputNodeType);
        if (!layerNode.has_value())
            return 1;
        const auto layer = *layerNode;

        const auto scalar = a.addByType(document::kScalarValueNodeType, {-600, -200});
        QCoreApplication::processEvents();

        // The Scalar card carries a keyframe diamond, exactly as a layer row does.
        auto* diamond = qobject_cast<KeyframeDiamond*>(
            a.editor.graphScene()->nodeFieldForTest(scalar, QStringLiteral("nodeKeyframeDiamond")));
        expect(diamond != nullptr, "the Scalar card carries a keyframe diamond");
        if (diamond == nullptr)
            return 1;
        expect(diamond->state() == KeyframeDiamondState::Constant,
               "which reads Constant before the first key");

        // Wire it into the layer's opacity by hand, the owner's own gesture.
        a.drag(a.outputSocket(scalar)->scenePos(),
               a.namedSocket(layer, QString::fromUtf8(document::kOpacityParameterRole), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        expect(driverFor(a, layer, document::kOpacityParameterRole) != nullptr,
               "the Scalar drives the layer's opacity");

        // Key it 0 at frame 0 and 1 at frame 10, through the diamond and the card's own field.
        auto* value = qobject_cast<kit::KValueField*>(
            a.editor.graphScene()->nodeFieldForTest(scalar, QStringLiteral("nodeOperandEditor")));
        expect(value != nullptr, "the Scalar card carries its inline field");
        if (value == nullptr)
            return 1;
        (void)a.session.setCurrentTime(frame(0));
        expect(a.session.currentTime() == frame(0), "the session is at frame 0");
        value->setValue(0.0);
        QCoreApplication::processEvents();
        QTest::mouseClick(diamond, Qt::LeftButton);
        QCoreApplication::processEvents();
        expect(diamond->state() == KeyframeDiamondState::AnimatedWithKey,
               "clicking the diamond animates the Scalar and keys it here");

        expect(a.session.setCurrentTime(frame(10)), "the session moves to frame 10");
        QCoreApplication::processEvents();
        value->setValue(1.0);
        QCoreApplication::processEvents();
        expect(diamond->state() == KeyframeDiamondState::AnimatedWithKey,
               "and editing the value at a new time writes a second key there");

        // The sampled value reaches the composited frame, per frame.
        bool everyFrameMatches = true;
        for (std::int64_t index = 0; index <= 10; ++index) {
            const auto composited = composite(a, frame(index));
            const double expected = static_cast<double>(index) / 10.0;
            if (!composited.ok || !closeTo(composited.centerPixel[3], expected)) {
                everyFrameMatches = false;
                std::cerr << "DIAG frame " << index << " alpha "
                          << (composited.ok ? composited.centerPixel[3] : -1.0F) << " expected "
                          << expected << '\n';
            }
        }
        expect(everyFrameMatches,
               "the driven opacity samples the animated Scalar at every one of the ten frames");

        // Undo puts the whole thing back.
        expect(a.session.undo(), "the last key undoes");
        testVector3DrivenReadout();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
