// Issue #120 (task U5): PropertiesEditor's kit field grid, hover recipe, keyframe indicator, and
// no-selection document view. Every expectation here is presentation-only -- it asserts VALUES,
// UNITS, and STATE RECIPES the redesigned panel now shows, not any editing capability beyond what
// composition_projection_test.cpp already exercises (setSelectedPosition/setSelectedOpacity
// directly through CompositionSession, unchanged by this task).

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPointF>
#include <QWidget>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <variant>

namespace {

using namespace bloom;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool near(const QColor& left, const QColor& right, const int tolerance) {
    return std::abs(left.red() - right.red()) <= tolerance &&
           std::abs(left.green() - right.green()) <= tolerance &&
           std::abs(left.blue() - right.blue()) <= tolerance;
}

[[nodiscard]] core::RationalTime time(const std::int64_t numerator,
                                      const std::int64_t denominator = 1) {
    const auto result = core::RationalTime::create(numerator, denominator);
    if (!result.has_value()) {
        std::cerr << "properties editor test: test time must be valid\n";
        std::exit(1);
    }
    return *result;
}

struct LayerIds final {
    document::LayerId layer;
    document::ParameterId position;
    document::ParameterId opacity;
    document::ParameterId color;
};

// Mirrors composition_session_animation_tests.cpp's addSolidLayer() fixture exactly (same
// AddSolidLayer transaction, same stable-ID output extraction), plus the color parameter output
// this file also needs.
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
    const auto color =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerColorParameterOutput);
    if (!(result.changed() && layer.has_value() && position.has_value() && opacity.has_value() &&
          color.has_value())) {
        std::cerr << "properties editor test: solid layer command must expose its stable IDs\n";
        std::exit(1);
    }
    return {*layer, *position, *opacity, *color};
}

// Mirrors composition_session_animation_tests.cpp's animateParameter() fixture exactly.
[[nodiscard]] document::AnimationCurveId
animateParameter(document::Document& document, commands::CommandStack& stack,
                 const document::CompositionId compositionId,
                 const document::ParameterId parameterId) {
    commands::Transaction transaction("Animate test parameter", document.snapshot().revision());
    transaction.emplace<commands::CreateAnimationForParameter>(compositionId, parameterId, time(0));
    const auto result = stack.execute(std::move(transaction));
    const auto curve = result.outputId<document::AnimationCurveId>(commands::kAnimationCurveOutput);
    if (!(result.changed() && curve.has_value())) {
        std::cerr << "properties editor test: animation command must expose its curve ID\n";
        std::exit(1);
    }
    return *curve;
}

// A row's keyframe indicator paints IconId::Keyframe tinted either Color::Keyframe (animated) or
// a dimmed Color::Muted (static) -- resolve which one is actually on-screen by grabbing the
// indicator and comparing its dominant non-transparent pixel against both candidate tints.
[[nodiscard]] bool indicatorLooksAnimated(QLabel& indicator) {
    const QImage image = indicator.grab().toImage();
    const QColor gold = ui::kit::color(ui::kit::Color::Keyframe);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 0 && near(pixel, gold, 24)) {
                return true;
            }
        }
    }
    return false;
}

void testSelectionShowsGroupedRowsWithValuesAndUnits(Expectations& expectations) {
    auto newProject = document::makeNewProject("Grid Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* positionX = properties.findChild<ui::kit::KValueField*>("positionXEditor");
    auto* positionY = properties.findChild<ui::kit::KValueField*>("positionYEditor");
    auto* opacity = properties.findChild<ui::kit::KValueField*>("opacityEditor");
    expectations.expect(positionX != nullptr && positionY != nullptr && opacity != nullptr,
                        "the Transform/Appearance rows expose kit::KValueField cells");
    if (positionX == nullptr || positionY == nullptr || opacity == nullptr) {
        return;
    }
    expectations.expect(positionX->value() == 10.0 && positionY->value() == 20.0,
                        "the Position row reads the solid's exact constant value");
    expectations.expect(positionX->unit() == QStringLiteral("px") &&
                            positionY->unit() == QStringLiteral("px"),
                        "Position cells carry the px unit suffix");
    expectations.expect(positionX->label() == QStringLiteral("X") &&
                            positionY->label() == QStringLiteral("Y"),
                        "Position cells carry their own X/Y sub-labels");
    expectations.expect(opacity->value() == 100.0 && opacity->unit() == QStringLiteral("%"),
                        "the Opacity row reads the default constant value with a % unit");

    auto* solidPanel = properties.findChild<QWidget*>("solidColorProperties");
    auto* solidColorValue = properties.findChild<QLabel*>("solidColorValue");
    expectations.expect(solidPanel != nullptr && !solidPanel->isHidden() &&
                            solidColorValue != nullptr &&
                            solidColorValue->text() == QStringLiteral("R 0.2  G 0.3  B 0.4  A 1"),
                        "the Solid Source group shows the exact RGBA text unchanged in content");

    auto* documentSection = properties.findChild<QWidget*>("propertiesDocumentSection");
    expectations.expect(documentSection != nullptr && documentSection->isHidden(),
                        "a real selection hides the no-selection document view");
}

void testAnimatedParameterShowsGoldStaticShowsDim(Expectations& expectations) {
    auto newProject = document::makeNewProject("Animation Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);
    (void)animateParameter(document, stack, compositionId, ids.opacity);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);
    properties.resize(properties.sizeHint());

    auto* opacityField = properties.findChild<ui::kit::KValueField*>("opacityEditor");
    auto* positionField = properties.findChild<ui::kit::KValueField*>("positionXEditor");
    expectations.expect(opacityField != nullptr && positionField != nullptr,
                        "both parameter rows resolve their value cells");
    if (opacityField == nullptr || positionField == nullptr) {
        return;
    }
    auto* opacityRow = opacityField->parentWidget();
    auto* positionRow = positionField->parentWidget()->parentWidget();
    expectations.expect(opacityRow != nullptr && positionRow != nullptr, "rows resolve");
    if (opacityRow == nullptr || positionRow == nullptr) {
        return;
    }
    auto* opacityIndicator = opacityRow->findChild<QLabel*>("propertiesKeyframeIndicator");
    auto* positionIndicator = positionRow->findChild<QLabel*>("propertiesKeyframeIndicator");
    expectations.expect(opacityIndicator != nullptr && positionIndicator != nullptr,
                        "both rows carry a keyframe indicator");
    if (opacityIndicator == nullptr || positionIndicator == nullptr) {
        return;
    }
    opacityIndicator->resize(opacityIndicator->sizeHint());
    positionIndicator->resize(positionIndicator->sizeHint());

    expectations.expect(indicatorLooksAnimated(*opacityIndicator),
                        "the animated Opacity parameter paints the gold Keyframe indicator");
    expectations.expect(!indicatorLooksAnimated(*positionIndicator),
                        "the still-constant Position parameter paints the dimmed indicator");
}

void testHoverPaintsTheStatesRecipeOnARow(Expectations& expectations) {
    auto newProject = document::makeNewProject("Hover Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* opacityField = properties.findChild<ui::kit::KValueField*>("opacityEditor");
    expectations.expect(opacityField != nullptr, "the Opacity row resolves for the hover check");
    if (opacityField == nullptr) {
        return;
    }
    auto* row = opacityField->parentWidget();
    expectations.expect(row != nullptr && row->objectName() == QStringLiteral("propertiesRow"),
                        "the Opacity value cell's immediate parent is its PropertiesRow container");
    if (row == nullptr) {
        return;
    }
    row->resize(row->sizeHint());

    const QColor restBackground = row->grab().toImage().pixelColor(1, 1);
    const QColor hoverExpected =
        ui::kit::color(ui::kit::surfaceForState(ui::kit::Color::Background, ui::kit::State::Hover));
    expectations.expect(!near(restBackground, hoverExpected, 4),
                        "a row at rest does not already paint the hover fill");

    QEnterEvent enter(QPointF(1.0, 1.0), QPointF(1.0, 1.0), QPointF(1.0, 1.0));
    QCoreApplication::sendEvent(row, &enter);

    const QImage hovered = row->grab().toImage();
    bool sawHoverFill = false;
    for (int y = 0; y < hovered.height() && !sawHoverFill; ++y) {
        for (int x = 0; x < hovered.width(); ++x) {
            if (near(hovered.pixelColor(x, y), hoverExpected, 4)) {
                sawHoverFill = true;
                break;
            }
        }
    }
    expectations.expect(sawHoverFill,
                        "hovering the row paints the States recipe: Background stepped to "
                        "Hover's surface, i.e. Surface");

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(row, &leave);
    const QColor afterLeave = row->grab().toImage().pixelColor(1, 1);
    expectations.expect(!near(afterLeave, hoverExpected, 4),
                        "leaving the row clears the hover fill");
}

void testNoSelectionShowsDocumentProperties(Expectations& expectations) {
    const auto frameRate = document::FrameRate::create(24000, 1001);
    if (!frameRate.has_value()) {
        expectations.expect(false, "the fractional test frame rate must be valid");
        return;
    }
    const auto format = document::CompositionFormat::create(
        1920, 1080, core::PixelAspectRatio::square(), *frameRate);
    if (!format.has_value()) {
        expectations.expect(false, "the fractional-rate test format must be valid");
        return;
    }
    // 240 exact frames at 24000/1001 fps -- long enough to prove the frame count and the
    // millisecond-truncated seconds text are both derived from the SAME exact rational
    // arithmetic composition_editors.cpp's formatExactSeconds()/frameContextFor() already use for
    // the timeline readout, not a second, possibly-diverging formatting rule.
    constexpr std::int64_t kFrameCount = 240;
    constexpr std::int64_t kFrameRateDenominator = 1001;
    auto newProject = document::makeNewProject(
        "Document Test", "Main", time(kFrameCount * kFrameRateDenominator, 24000), *format);
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);

    ui::CompositionSession session(document, stack, compositionId);
    ui::PropertiesEditor properties(session);

    auto* selectionSection = properties.findChild<QWidget*>("propertiesSelectionSection");
    auto* documentSection = properties.findChild<QWidget*>("propertiesDocumentSection");
    expectations.expect(selectionSection != nullptr && documentSection != nullptr &&
                            !documentSection->isHidden() && selectionSection->isHidden(),
                        "no selection shows the document view and hides the selection groups");

    auto* name = properties.findChild<QLabel*>("documentName");
    auto* formatLabel = properties.findChild<QLabel*>("documentFormat");
    auto* frameRateLabel = properties.findChild<QLabel*>("documentFrameRate");
    auto* durationLabel = properties.findChild<QLabel*>("documentDuration");
    auto* pixelAspectLabel = properties.findChild<QLabel*>("documentPixelAspect");
    expectations.expect(name != nullptr && formatLabel != nullptr && frameRateLabel != nullptr &&
                            durationLabel != nullptr && pixelAspectLabel != nullptr,
                        "the document view exposes every reachable composition fact");
    if (name == nullptr || formatLabel == nullptr || frameRateLabel == nullptr ||
        durationLabel == nullptr || pixelAspectLabel == nullptr) {
        return;
    }
    expectations.expect(name->text() == QStringLiteral("Main"),
                        "the document view names the exact composition");
    expectations.expect(formatLabel->text() == QStringLiteral("1920 × 1080 px"),
                        "the document view shows the exact composition format");
    expectations.expect(frameRateLabel->text() == QStringLiteral("24000/1001 fps"),
                        "a fractional frame rate is shown as the exact rational, never rounded");
    expectations.expect(durationLabel->text() == QStringLiteral("240 frames · 10.010s"),
                        "duration shows the exact frame count and the truncated exact seconds");
    expectations.expect(pixelAspectLabel->text() == QStringLiteral("1:1"),
                        "square pixel aspect is shown as an exact ratio");
}

void testSelectionSwapUpdatesRows(Expectations& expectations) {
    auto newProject = document::makeNewProject("Swap Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto first = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    ui::PropertiesEditor properties(session);

    auto* documentSection = properties.findChild<QWidget*>("propertiesDocumentSection");
    auto* selectionSection = properties.findChild<QWidget*>("propertiesSelectionSection");
    expectations.expect(documentSection != nullptr && !documentSection->isHidden() &&
                            selectionSection != nullptr && selectionSection->isHidden(),
                        "constructing with no selection starts on the document view");

    session.selectLayer(first.layer);
    expectations.expect(documentSection->isHidden() && !selectionSection->isHidden(),
                        "selecting a layer swaps to the selection groups");

    constexpr core::Color4d secondColor{0.9, 0.1, 0.5, 1.0};
    expectations.expect(session.addSolidLayer(QStringLiteral("Second"), secondColor),
                        "a second solid layer can be added for the swap");
    const auto* secondLayerId = std::get_if<document::LayerId>(&session.selection().primary);
    expectations.expect(secondLayerId != nullptr, "the new solid becomes the primary selection");
    if (secondLayerId == nullptr) {
        return;
    }

    auto* positionX = properties.findChild<ui::kit::KValueField*>("positionXEditor");
    auto* solidColorValue = properties.findChild<QLabel*>("solidColorValue");
    expectations.expect(positionX != nullptr && solidColorValue != nullptr, "rows still resolve");
    if (positionX == nullptr || solidColorValue == nullptr) {
        return;
    }
    // CompositionSession::addSolidLayer() defaults a new solid's position to the composition's
    // own center (960, 540 for the default 1920x1080 format -- composition_session.cpp's
    // compositionCenter()), distinct from the first layer's explicit (10, 20): proves the row
    // actually re-read the new selection rather than keeping stale values across the swap.
    expectations.expect(positionX->value() == 960.0,
                        "the swapped-to layer's own position value replaces the previous row");
    expectations.expect(solidColorValue->text() == QStringLiteral("R 0.9  G 0.1  B 0.5  A 1"),
                        "the swapped-to layer's own solid color replaces the previous row");

    session.clearSelection();
    expectations.expect(!documentSection->isHidden() && selectionSection->isHidden(),
                        "clearing the selection swaps back to the document view");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testSelectionShowsGroupedRowsWithValuesAndUnits(expectations);
    testAnimatedParameterShowsGoldStaticShowsDim(expectations);
    testHoverPaintsTheStatesRecipeOnARow(expectations);
    testNoSelectionShowsDocumentProperties(expectations);
    testSelectionSwapUpdatesRows(expectations);
    if (expectations.failures() > 0) {
        std::cerr << expectations.failures() << " properties editor expectation(s) failed\n";
        return 1;
    }
    return 0;
}
