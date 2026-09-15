// Issue #120 (task U5): PropertiesEditor's kit field grid, keyframe indicator, and no-selection
// document view. Task P1/P2/P3/P4 (owner review 2026-09-12) added real behavior on top: the
// selection title row and its "Nothing selected" placeholder are gone, whole-row hover is gone,
// the RGBA cells are genuinely editable through a new CompositionSession::setSelectedSolidColor()
// command with undo parity, and a focused+hovered cell's on-screen border is pinned to Accent.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointF>
#include <QSettings>
#include <QThread>
#include <QVariant>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
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

void prepareColor(ui::CompositionSession& session) {
    QElapsedTimer timer;
    timer.start();
    while (!session.colorConverter() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
}

double displayChannel(const double reference) {
    return reference <= 0.0031308 ? reference * 12.92
                                  : 1.055 * std::pow(reference, 1.0 / 2.4) - 0.055;
}
bool displayNear(const double shown, const double reference) {
    return std::abs(shown - displayChannel(reference)) < 3e-5;
}
QColor displayColor(const double r, const double g, const double b, const double a = 1) {
    return QColor::fromRgbF(static_cast<float>(displayChannel(r)),
                            static_cast<float>(displayChannel(g)),
                            static_cast<float>(displayChannel(b)), static_cast<float>(a));
}

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
    document::ParameterId anchor;
    document::ParameterId scale;
    document::ParameterId rotation;
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
    const auto anchor =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerAnchorParameterOutput);
    const auto scale =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerScaleParameterOutput);
    const auto rotation =
        result.outputId<document::ParameterId>(commands::kAddSolidLayerRotationParameterOutput);
    if (!(result.changed() && layer.has_value() && position.has_value() && opacity.has_value() &&
          color.has_value() && anchor.has_value() && scale.has_value() && rotation.has_value())) {
        std::cerr << "properties editor test: solid layer command must expose its stable IDs\n";
        std::exit(1);
    }
    return {*layer, *position, *opacity, *color, *anchor, *scale, *rotation};
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
// Task S5, item 0: the row's indicator is a clickable ui::KeyframeDiamond now rather than a QLabel.
// objectName "propertiesKeyframeIndicator" is unchanged, so only the looked-up TYPE moved here.
[[nodiscard]] bool indicatorLooksAnimated(QWidget& indicator) {
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
    prepareColor(session);
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
    auto* red = properties.findChild<ui::kit::KValueField*>("solidColorRedEditor");
    auto* green = properties.findChild<ui::kit::KValueField*>("solidColorGreenEditor");
    auto* blue = properties.findChild<ui::kit::KValueField*>("solidColorBlueEditor");
    auto* alpha = properties.findChild<ui::kit::KValueField*>("solidColorAlphaEditor");
    expectations.expect(solidPanel != nullptr && !solidPanel->isHidden() && red != nullptr &&
                            green != nullptr && blue != nullptr && alpha != nullptr,
                        "the Solid Source group exposes four RGBA kit::KValueField cells");
    if (red == nullptr || green == nullptr || blue == nullptr || alpha == nullptr) {
        return;
    }
    expectations.expect(displayNear(red->value(), 0.2) && displayNear(green->value(), 0.3) &&
                            displayNear(blue->value(), 0.4) && alpha->value() == 1.0,
                        "the RGBA cells display the solid's converted reference value");
    expectations.expect(red->unit().isEmpty() && green->unit().isEmpty() &&
                            blue->unit().isEmpty() && alpha->unit().isEmpty(),
                        "RGBA cells carry no unit suffix");
    expectations.expect(red->decimals() == 3 && red->singleStep() == 0.01,
                        "RGBA cells show 3 decimals and scrub in 0.01 steps");
    // FORMAL AMENDMENT 1 (2026-09-12): the RGBA cells are unbounded -- negative and HDR channels
    // are never clipped, exactly like the read-only label they replaced. A solid's default palette
    // color is well within [0, 1], so this only pins that the range was not narrowed to it; the
    // no-clipping guarantee itself is pinned by testRgbaCellsNeverClipNegativeOrHdrChannels below.
    expectations.expect(red->minimum() < 0.0 && red->maximum() > 1.0,
                        "RGBA cells are not range-clamped to 0-1");
    expectations.expect(
        red->label() == QStringLiteral("R") && green->label() == QStringLiteral("G") &&
            blue->label() == QStringLiteral("B") && alpha->label() == QStringLiteral("A"),
        "RGBA cells carry their own R/G/B/A sub-labels");

    auto* documentSection = properties.findChild<QWidget*>("propertiesDocumentSection");
    expectations.expect(documentSection != nullptr && documentSection->isHidden(),
                        "a real selection hides the no-selection document view");

    expectations.expect(properties.findChild<QLabel*>("propertiesSelectionTitle") == nullptr,
                        "task P1: the selection title row no longer exists");
    for (const auto* label : properties.findChildren<QLabel*>()) {
        expectations.expect(label->text() != QStringLiteral("Nothing selected"),
                            "task P1: no label anywhere in the panel reads \"Nothing selected\"");
    }
}

void testAnimatedParameterShowsGoldStaticShowsDim(Expectations& expectations) {
    auto newProject = document::makeNewProject("Animation Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);
    (void)animateParameter(document, stack, compositionId, ids.opacity);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
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
    auto* opacityIndicator =
        opacityRow->findChild<ui::KeyframeDiamond*>("propertiesKeyframeIndicator");
    auto* positionIndicator =
        positionRow->findChild<ui::KeyframeDiamond*>("propertiesKeyframeIndicator");
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

// Task P2 (owner review 2026-09-12: "hover is for the component being interacted, not the whole
// row"): a row entering hover must paint NOTHING of its own -- the row is a plain container now,
// and every hover affordance comes from the kit control it lays out.
void testRowNeverPaintsWholeRowHover(Expectations& expectations) {
    auto newProject = document::makeNewProject("Hover Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* opacityField = properties.findChild<ui::kit::KValueField*>("opacityEditor");
    expectations.expect(opacityField != nullptr, "the Opacity row resolves for the hover check");
    if (opacityField == nullptr) {
        return;
    }
    auto* row = opacityField->parentWidget();
    expectations.expect(row != nullptr && row->objectName() == QStringLiteral("propertiesRow"),
                        "the Opacity value cell's immediate parent is its row container");
    if (row == nullptr) {
        return;
    }
    row->resize(row->sizeHint());

    row->clearFocus();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(row, &leave);
    const QImage before = row->grab().toImage();

    QEnterEvent enter(QPointF(1.0, 1.0), QPointF(1.0, 1.0), QPointF(1.0, 1.0));
    QCoreApplication::sendEvent(row, &enter);

    const QImage hovered = row->grab().toImage();
    expectations.expect(before == hovered,
                        "entering the row repaints nothing -- no whole-row hover fill");
    // Card surfaces legitimately share the old row-hover token. Image equality above pins
    // the behavior without forbidding that color in controls, antialiased text or card paint.
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
    prepareColor(session);
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
    prepareColor(session);
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
    auto* red = properties.findChild<ui::kit::KValueField*>("solidColorRedEditor");
    auto* green = properties.findChild<ui::kit::KValueField*>("solidColorGreenEditor");
    auto* blue = properties.findChild<ui::kit::KValueField*>("solidColorBlueEditor");
    expectations.expect(positionX != nullptr && red != nullptr && green != nullptr &&
                            blue != nullptr,
                        "rows still resolve");
    if (positionX == nullptr || red == nullptr || green == nullptr || blue == nullptr) {
        return;
    }
    // CompositionSession::addSolidLayer() defaults a new solid's position to the composition's
    // own center (960, 540 for the default 1920x1080 format -- composition_session.cpp's
    // compositionCenter()), distinct from the first layer's explicit (10, 20): proves the row
    // actually re-read the new selection rather than keeping stale values across the swap.
    expectations.expect(positionX->value() == 960.0,
                        "the swapped-to layer's own position value replaces the previous row");
    expectations.expect(displayNear(red->value(), 0.9) && displayNear(green->value(), 0.1) &&
                            displayNear(blue->value(), 0.5),
                        "the swapped-to layer's own solid color replaces the previous RGBA cells");

    session.clearSelection();
    expectations.expect(!documentSection->isHidden() && selectionSection->isHidden(),
                        "clearing the selection swaps back to the document view");
}

// Task P3: editing an RGBA cell writes through CompositionSession::setSelectedSolidColor() into a
// real commands::SetParameterSource transaction -- the document value changes, the other three
// channels are untouched, and undo restores the exact pre-edit color, the same undo/dirty/revision
// parity Position already has.
void testRgbaCellsEditThroughCommandWithUndo(Expectations& expectations) {
    auto newProject = document::makeNewProject("Color Edit Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* red = properties.findChild<ui::kit::KValueField*>("solidColorRedEditor");
    expectations.expect(red != nullptr, "the Red cell resolves for the edit check");
    if (red == nullptr) {
        return;
    }

    red->setValue(0.75);
    const auto edited = session.constantColorValue(ids.color);
    expectations.expect(edited.has_value() && std::abs(edited->red - 0.522522) < 3e-5,
                        "editing the Red cell commits through a command and updates the document");
    expectations.expect(edited.has_value() && edited->green == 0.3 && edited->blue == 0.4 &&
                            edited->alpha == 1.0,
                        "editing one channel leaves the other three exactly as they were");
    expectations.expect(session.canUndo(), "the color edit lands as one undoable command");

    expectations.expect(session.undo(), "the color edit can be undone");
    const auto restored = session.constantColorValue(ids.color);
    expectations.expect(restored.has_value() && restored->red == 0.2 && restored->green == 0.3 &&
                            restored->blue == 0.4 && restored->alpha == 1.0,
                        "undo restores the exact pre-edit color");
}

// FORMAL AMENDMENT 1 (2026-09-12, after the first report): the 0-1 clamp in the first pass of P3
// was the package author's error -- the document contract allows negative and HDR channels, and
// the former read-only label promised no clipping. The RGBA cells are unbounded and must restore
// that exact guarantee: display the exact stored value for a negative/HDR channel, with no
// clipping.
// Task S4: the Transform group's three new rows. What is pinned here is the full authoring loop --
// the row shows the stored value in its authored unit, an edit commits through a command, and undo
// restores the exact prior value -- for each of the three, plus the unit conversions (scale as a
// percentage of a unitless factor) and the fact that rotation is NOT clamped to one turn.
void testTransformRowsEditThroughCommandsWithUndo(Expectations& expectations) {
    auto newProject = document::makeNewProject("Transform Rows Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* anchorX = properties.findChild<ui::kit::KValueField*>("anchorXEditor");
    auto* anchorY = properties.findChild<ui::kit::KValueField*>("anchorYEditor");
    auto* scaleX = properties.findChild<ui::kit::KValueField*>("scaleXEditor");
    auto* scaleY = properties.findChild<ui::kit::KValueField*>("scaleYEditor");
    auto* rotation = properties.findChild<ui::kit::KValueField*>("rotationEditor");
    expectations.expect(anchorX != nullptr && anchorY != nullptr && scaleX != nullptr &&
                            scaleY != nullptr && rotation != nullptr,
                        "the Transform group exposes Anchor X/Y, Scale X/Y, and Rotation rows");
    if (anchorX == nullptr || anchorY == nullptr || scaleX == nullptr || scaleY == nullptr ||
        rotation == nullptr) {
        return;
    }

    // A new layer starts at the identity transform, which reads as a centre anchor, 100% on both
    // axes, and zero degrees.
    expectations.expect(anchorX->value() == 0.0 && anchorY->value() == 0.0 &&
                            scaleX->value() == 100.0 && scaleY->value() == 100.0 &&
                            rotation->value() == 0.0,
                        "a new layer's transform rows read as the identity transform");
    expectations.expect(anchorX->unit() == QStringLiteral("px") &&
                            scaleX->unit() == QStringLiteral("%") &&
                            rotation->unit() == QString::fromUtf8("\u00b0"),
                        "the transform rows carry pixel, percent, and degree units");
    expectations.expect(rotation->singleStep() == 1.0 && scaleX->singleStep() == 1.0,
                        "rotation scrubs in whole degrees and scale in whole percent");

    const auto revisionBefore = document.snapshot().revision();
    anchorX->setValue(-24.0);
    const auto anchorValue = session.constantVec2Value(ids.anchor);
    expectations.expect(anchorValue.has_value() && anchorValue->x == -24.0 && anchorValue->y == 0.0,
                        "editing Anchor X commits through a command and leaves Y alone");

    // Task PROPS-1, deliverable 1: the Scale row carries a proportional-link toggle, ON by default.
    // Broken here so this half of the case still authors exactly one axis; the linked behaviour is
    // asserted on its own below, after the undo assertions have counted these three edits.
    auto* scaleLink = properties.findChild<ui::kit::KButton*>("scaleLinkToggle");
    expectations.expect(scaleLink != nullptr && scaleLink->isChecked(),
                        "the Scale row exposes a proportional-link toggle, engaged by default");
    if (scaleLink != nullptr) {
        scaleLink->setChecked(false);
    }
    scaleY->setValue(50.0);
    const auto scaleValue = session.constantVec2Value(ids.scale);
    expectations.expect(scaleValue.has_value() && scaleValue->x == 1.0 && scaleValue->y == 0.5,
                        "a scale row authored as a percentage stores a unitless factor");

    // 450 degrees is a legitimate authored value: the row must not fold or clamp it, because a
    // rotation curve has to be able to wind past a full turn.
    rotation->setValue(450.0);
    const auto rotationValue = session.constantValue(ids.rotation);
    expectations.expect(rotationValue.has_value() && *rotationValue == 450.0,
                        "a rotation past a full turn is stored exactly as authored");

    expectations.expect(document.snapshot().revision() != revisionBefore && session.canUndo(),
                        "each transform edit is one undoable command");
    expectations.expect(session.undo() && session.undo() && session.undo(),
                        "all three transform edits undo");
    const auto restoredAnchor = session.constantVec2Value(ids.anchor);
    const auto restoredScale = session.constantVec2Value(ids.scale);
    const auto restoredRotation = session.constantValue(ids.rotation);
    expectations.expect(
        restoredAnchor.has_value() && *restoredAnchor == document::kDefaultAnchor &&
            restoredScale.has_value() && *restoredScale == document::kDefaultScale &&
            restoredRotation.has_value() && *restoredRotation == document::kDefaultRotationDegrees,
        "undo restores the exact identity transform");

    // With the link re-engaged, editing one axis carries the other along at the stored ratio: the
    // uniform 100/100 identity becomes a uniform 50/50 from a single Y edit.
    if (scaleLink != nullptr) {
        scaleLink->setChecked(true);
    }
    scaleY->setValue(50.0);
    const auto linkedScale = session.constantVec2Value(ids.scale);
    expectations.expect(linkedScale.has_value() && linkedScale->x == 0.5 && linkedScale->y == 0.5,
                        "a linked scale row carries the other axis along at the stored ratio");
}

// The Appearance group's Blending row: a real control over a real parameter, one undoable command
// per change, reading back what the document stores. It carries no keyframe indicator on purpose --
// the blend-mode schema is not animatable -- which is why this case lives beside the transform-row
// case rather than inside the keyframe-indicator one below.
void testBlendingRowEditsThroughOneCommandWithUndo(Expectations& expectations) {
    auto newProject = document::makeNewProject("Blending Row Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* blending = properties.findChild<ui::kit::KDropdown*>("blendModeEditor");
    expectations.expect(blending != nullptr, "the Appearance group exposes a Blending row");
    if (blending == nullptr) {
        return;
    }
    expectations.expect(
        blending->isEnabled() && blending->count() == static_cast<int>(core::kBlendModes.size()) &&
            blending->currentText() == ui::blendModeDisplayName(core::kDefaultBlendMode),
        "a new layer's Blending row is enabled, offers every mode, and reads Normal");

    const auto overlayRow = [blending] {
        const auto stored = core::blendModeStoredValue(core::BlendMode::Overlay);
        for (int index = 0; index < blending->count(); ++index) {
            if (blending->itemData(index).value<std::int64_t>() == stored) {
                return index;
            }
        }
        return -1;
    }();
    expectations.expect(overlayRow >= 0, "the vocabulary includes Overlay");
    if (overlayRow < 0) {
        return;
    }

    const auto revisionBefore = document.snapshot().revision();
    blending->setCurrentIndex(overlayRow);
    expectations.expect(session.blendModeForLayer(ids.layer) == core::BlendMode::Overlay,
                        "picking a mode commits it to the layer's own parameter");
    expectations.expect(document.snapshot().revision() != revisionBefore && session.canUndo() &&
                            session.undoLabel() == QStringLiteral("Set Blend Mode"),
                        "the edit is exactly one undoable command, named for what it did");
    expectations.expect(session.undo() &&
                            session.blendModeForLayer(ids.layer) == core::kDefaultBlendMode,
                        "undo restores the authored Normal");
    expectations.expect(blending->currentText() ==
                            ui::blendModeDisplayName(core::kDefaultBlendMode),
                        "and the row follows the undone document rather than its own last choice");

    // A committing no-op: re-picking the mode the layer already has must publish no revision and no
    // history entry, exactly as the text-content row's equal-value path does.
    const auto settled = document.snapshot().revision();
    const auto historyBefore = session.canUndo();
    blending->setCurrentIndex(blending->currentIndex());
    expectations.expect(document.snapshot().revision() == settled &&
                            session.canUndo() == historyBefore,
                        "re-picking the current mode commits nothing");
}

// Every Transform row carries its own keyframe indicator, and each must light up for its own
// parameter only -- the three new rows are animatable exactly as position and opacity are.
void testTransformRowsShowTheirOwnKeyframeIndicators(Expectations& expectations) {
    auto newProject = document::makeNewProject("Transform Keyframe Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);
    static_cast<void>(animateParameter(document, stack, compositionId, ids.scale));

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);
    properties.resize(420, 600);
    properties.show();

    auto* anchorX = properties.findChild<ui::kit::KValueField*>("anchorXEditor");
    auto* scaleX = properties.findChild<ui::kit::KValueField*>("scaleXEditor");
    auto* rotation = properties.findChild<ui::kit::KValueField*>("rotationEditor");
    expectations.expect(anchorX != nullptr && scaleX != nullptr && rotation != nullptr,
                        "the three transform rows resolve for the indicator check");
    if (anchorX == nullptr || scaleX == nullptr || rotation == nullptr) {
        return;
    }
    // Each row is [label, indicator, value]; a paired X/Y row wraps its two fields in a group, so
    // the row is one level further up than it is for the single rotation field.
    const auto indicatorOf = [](QWidget* field, const bool paired) -> ui::KeyframeDiamond* {
        auto* row = paired ? field->parentWidget()->parentWidget() : field->parentWidget();
        return row == nullptr ? nullptr
                              : row->findChild<ui::KeyframeDiamond*>("propertiesKeyframeIndicator",
                                                                     Qt::FindDirectChildrenOnly);
    };
    auto* anchorIndicator = indicatorOf(anchorX, true);
    auto* scaleIndicator = indicatorOf(scaleX, true);
    auto* rotationIndicator = indicatorOf(rotation, false);
    expectations.expect(anchorIndicator != nullptr && scaleIndicator != nullptr &&
                            rotationIndicator != nullptr,
                        "every transform row carries its own keyframe indicator");
    if (anchorIndicator == nullptr || scaleIndicator == nullptr || rotationIndicator == nullptr) {
        return;
    }
    expectations.expect(indicatorLooksAnimated(*scaleIndicator) &&
                            !indicatorLooksAnimated(*anchorIndicator) &&
                            !indicatorLooksAnimated(*rotationIndicator),
                        "only the animated transform parameter's own indicator reads as animated");
    // Task S5, item 0 inverted this: an animated row stays EDITABLE and shows the curve's exactly
    // sampled value at the current time, because typing into it at a time with no key is how AE
    // inserts one (CompositionSession::effectiveVec2Value() + the existing SetKeyframeAtTime write
    // path). Before this task the field was disabled, which made that gesture unreachable.
    expectations.expect(scaleX->isEnabled() && rotation->isEnabled() && anchorX->isEnabled(),
                        "an animated transform row stays editable so editing it can insert a key");
}

void testRgbaCellsNeverClipNegativeOrHdrChannels(Expectations& expectations) {
    auto newProject = document::makeNewProject("HDR Color Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    constexpr core::Color4d hdrColor{-0.25, 1.5, 0.125, 0.8};
    expectations.expect(session.addSolidLayer(QStringLiteral("HDR"), hdrColor),
                        "a negative/HDR solid can be added for the clipping check");

    ui::PropertiesEditor properties(session);
    auto* red = properties.findChild<ui::kit::KValueField*>("solidColorRedEditor");
    auto* green = properties.findChild<ui::kit::KValueField*>("solidColorGreenEditor");
    auto* blue = properties.findChild<ui::kit::KValueField*>("solidColorBlueEditor");
    auto* alpha = properties.findChild<ui::kit::KValueField*>("solidColorAlphaEditor");
    expectations.expect(red != nullptr && green != nullptr && blue != nullptr && alpha != nullptr,
                        "the RGBA cells resolve for the negative/HDR check");
    if (red == nullptr || green == nullptr || blue == nullptr || alpha == nullptr) {
        return;
    }
    expectations.expect(red->value() == -0.25 && green->value() == 1.5 && blue->value() == 0.125 &&
                            alpha->value() == 0.8,
                        "the RGBA cells display the exact negative and HDR channel values with no "
                        "clipping");

    // Editing an unrelated channel must not clip the OTHER channels' already-HDR/negative values
    // either -- commitSolidColor() reads all four live cell values on every emission.
    alpha->setValue(0.9);
    const auto edited = session.constantColorValue(
        session.parameterForSelection(document::kSolidColorParameterRole)->id);
    expectations.expect(edited.has_value() && edited->red == -0.25 && edited->green == 1.5 &&
                            edited->blue == 0.125 && edited->alpha == 0.9,
                        "committing one channel keeps the other channels' exact negative/HDR "
                        "values, unclipped, in the document");
}

// Task P3: a scrub gesture (press, travel past the drag threshold, release) on an RGBA cell
// changes its value and commits that value into the document, exactly like a Position scrub.
void testScrubOnRgbaCellChangesValue(Expectations& expectations) {
    auto newProject = document::makeNewProject("Color Scrub Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* blueField = properties.findChild<ui::kit::KValueField*>("solidColorBlueEditor");
    expectations.expect(blueField != nullptr, "the Blue cell resolves for the scrub check");
    if (blueField == nullptr) {
        return;
    }

    // Mirrors kit_value_field_tests.cpp's own scrub() helper: a press, a horizontal drag well past
    // the platform drag threshold, and a release, sent straight to the field.
    const QPointF start(blueField->cellRect().center());
    const auto travel = static_cast<qreal>(QApplication::startDragDistance() + 20);
    QMouseEvent press(QEvent::MouseButtonPress, start, blueField->mapToGlobal(start.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(blueField, &press);
    const QPointF moved = start + QPointF(travel, 0.0);
    QMouseEvent move(QEvent::MouseMove, moved, blueField->mapToGlobal(moved.toPoint()),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(blueField, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, moved, blueField->mapToGlobal(moved.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(blueField, &release);

    expectations.expect(blueField->value() != 0.4,
                        "scrubbing the Blue cell changes its displayed value");
    const auto scrubbed = session.constantColorValue(ids.color);
    expectations.expect(scrubbed.has_value() && displayNear(blueField->value(), scrubbed->blue),
                        "the scrub committed through the session into the document value");
}

// Task P4: F1's kit-wide focus/hover border rule (kit::borderForInteraction: focus wins over
// hover, one border, never a second ring), verified here with an actual on-screen grab of a
// focused+hovered cell embedded in this panel, not just borderToken() introspection.
void testFocusedHoveredCellBorderIsAccentOnScreen(Expectations& expectations) {
    auto newProject = document::makeNewProject("Focus Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);
    properties.show();
    properties.activateWindow();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* positionX = properties.findChild<ui::kit::KValueField*>("positionXEditor");
    expectations.expect(positionX != nullptr,
                        "the Position X cell resolves for the focus+hover check");
    if (positionX == nullptr) {
        return;
    }

    positionX->clearFocus();
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(positionX, &leave);
    positionX->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    expectations.expect(positionX->hasFocus(), "the cell can take keyboard focus inside the panel");

    QEnterEvent enter(QPointF(1.0, 1.0), QPointF(1.0, 1.0), QPointF(1.0, 1.0));
    QCoreApplication::sendEvent(positionX, &enter);

    expectations.expect(positionX->borderToken() == ui::kit::Color::Accent,
                        "a focused+hovered cell's border token stays Accent -- focus wins over "
                        "hover, and the panel does not override it");

    const QColor accent = ui::kit::color(ui::kit::Color::Accent);
    const QImage image = positionX->grab().toImage();
    bool sawAccentBorder = false;
    for (int y = 0; y < image.height() && !sawAccentBorder; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (near(image.pixelColor(x, y), accent, 4)) {
                sawAccentBorder = true;
                break;
            }
        }
    }
    expectations.expect(sawAccentBorder,
                        "the focused+hovered cell actually paints an Accent border on screen "
                        "inside the panel, with no separate focus ring painted underneath it");
}

// Task S3: the Text Source group. Content, size, and color are real editable rows that read project
// truth and commit through the session's own commands, and the Solid Source group is hidden while a
// text layer is selected (and the other way round), so the two can never both claim a selection.
void testTextSourceRowsEditThroughCommands(Expectations& expectations) {
    auto newProject = document::makeNewProject("Text Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    expectations.expect(session.addTextLayer(QStringLiteral("Title"), QStringLiteral("Hello"), 48.0,
                                             core::Color4d{0.25, 0.5, 0.75, 1.0}),
                        "a text layer can be added for the text rows");
    ui::PropertiesEditor properties(session);

    auto* textPanel = properties.findChild<QWidget*>("textSourceProperties");
    auto* solidPanel = properties.findChild<QWidget*>("solidColorProperties");
    auto* content = properties.findChild<QLineEdit*>("textContentEditor");
    auto* size = properties.findChild<ui::kit::KValueField*>("textSizeEditor");
    auto* color = properties.findChild<ui::kit::KColorChip*>("textColorChip");
    auto* font = properties.findChild<ui::kit::KDropdown*>("textFontName");
    expectations.expect(textPanel != nullptr && solidPanel != nullptr && content != nullptr &&
                            size != nullptr && color != nullptr && font != nullptr,
                        "the Text Source group exposes content, size, color, and font rows");
    if (textPanel == nullptr || solidPanel == nullptr || content == nullptr || size == nullptr ||
        color == nullptr || font == nullptr) {
        return;
    }

    properties.resize(properties.sizeHint());
    properties.show();
    QCoreApplication::processEvents();
    expectations.expect(textPanel->isVisible() && !solidPanel->isVisible(),
                        "a text selection shows the Text Source group and hides the Solid one");
    expectations.expect(content->text() == QStringLiteral("Hello") && size->value() == 48.0,
                        "the rows read the authored content and em size from project truth");
    expectations.expect(near(color->color()
                                 .converted(ui::kit::ColorSpace::Display, session.colorConverter())
                                 .value_or(ui::kit::KColor{})
                                 .toQColor(),
                             displayColor(0.25, 0.5, 0.75), 2),
                        "and the swatch reads the authored color");
    expectations.expect(content->isEnabled() && size->isEnabled() && color->isEnabled(),
                        "all three are editable, because a command exists for each");
    expectations.expect(font->currentText() == QStringLiteral("DejaVu Sans") &&
                            !font->isEnabled() &&
                            font->toolTip().contains(QStringLiteral("embedded")),
                        "the font row names the one embedded face rather than offering a choice");

    const auto historyBefore = stack.size();
    content->setText(QStringLiteral("Edited"));
    Q_EMIT content->editingFinished();
    expectations.expect(stack.size() == historyBefore + 1,
                        "committing the content field is exactly one history entry");
    size->setValue(96.0);
    expectations.expect(stack.size() == historyBefore + 2,
                        "committing the size field is exactly one more");

    const auto* textNode = [&]() -> const document::NodeRecord* {
        const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary);
        const auto nodeId =
            layerId == nullptr ? std::nullopt : session.directSourceNodeForLayer(*layerId);
        return nodeId.has_value() ? session.composition()->graph().findNode(*nodeId) : nullptr;
    }();
    expectations.expect(textNode != nullptr, "the selection still resolves its text source node");
    if (textNode == nullptr) {
        return;
    }
    const auto* contentParameter = session.parameterForSelection(document::kTextParameterRole);
    const auto* sizeParameter = session.parameterForSelection(document::kTextSizeParameterRole);
    expectations.expect(contentParameter != nullptr &&
                            session.constantStringValue(contentParameter->id) ==
                                QStringLiteral("Edited"),
                        "the committed content reached project truth");
    expectations.expect(sizeParameter != nullptr &&
                            session.constantValue(sizeParameter->id) == 96.0,
                        "and so did the committed size");

    expectations.expect(session.setSelectedTextColor(core::Color4d{0.1, 0.2, 0.3, 0.5}),
                        "the text color commits through its own session command");
    const auto* colorParameter = session.parameterForSelection(document::kTextColorParameterRole);
    expectations.expect(colorParameter != nullptr &&
                            session.constantColorValue(colorParameter->id) ==
                                core::Color4d{0.1, 0.2, 0.3, 0.5},
                        "and reaches project truth with its alpha intact");
    expectations.expect(near(color->color()
                                 .converted(ui::kit::ColorSpace::Display, session.colorConverter())
                                 .value_or(ui::kit::KColor{})
                                 .toQColor(),
                             displayColor(0.1, 0.2, 0.3, 0.5), 2),
                        "and the swatch re-reads it after the snapshot change");

    // Undone through the SESSION, not the bare stack: the session owns the snapshot every later
    // command is based on, so undoing behind its back would leave it on a stale revision.
    expectations.expect(session.undo(), "a text color edit is undoable");
    const auto* colorAfterUndo = session.parameterForSelection(document::kTextColorParameterRole);
    expectations.expect(colorAfterUndo != nullptr &&
                            session.constantColorValue(colorAfterUndo->id) ==
                                core::Color4d{0.25, 0.5, 0.75, 1.0},
                        "and restores the previously authored color exactly");

    // A solid selection swaps the groups back, which is what proves the two are mutually exclusive
    // rather than both keyed off the shared "color" role.
    expectations.expect(
        session.addSolidLayer(QStringLiteral("Plate"), core::Color4d{0.9, 0.1, 0.5, 1.0}),
        "a solid layer can be added for the group swap");
    QCoreApplication::processEvents();
    expectations.expect(!textPanel->isVisible() && solidPanel->isVisible(),
                        "selecting a solid hides the Text Source group and shows the Solid one");
}

// task WIDTH-1 (owner: "let it have min width of something like 300px ... so inner sections and
// users can compromise"): a narrow label column elides its text with an ellipsis rather than
// forcing the row wider or clipping raw, and always keeps the untruncated parameter name
// reachable through its tooltip.
void testLongLabelColumnElidesWhenNarrowAndKeepsTheFullNameAsATooltip(Expectations& expectations) {
    auto newProject = document::makeNewProject("Elide Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);
    properties.resize(properties.sizeHint());
    // show(), not just resize(): an invisible widget's layout is not guaranteed to activate all
    // the way down to leaf widgets from resize() alone
    // (testTransformRowsShowTheirOwnKeyframeIndicators and
    // testWorkspaceHostInsetsItsSingleAreaByTheGutterFromItsOwnRect's own precedent), and this test
    // means to measure a real, laid-out label geometry.
    properties.show();
    QCoreApplication::processEvents();

    auto* positionField = properties.findChild<ui::kit::KValueField*>("positionXEditor");
    expectations.expect(positionField != nullptr, "the Position row resolves its value cell");
    if (positionField == nullptr) {
        return;
    }
    // positionXEditor -> positionFieldGroup -> propertiesRow.
    auto* row = positionField->parentWidget()->parentWidget();
    expectations.expect(row != nullptr, "the Position row's container resolves");
    if (row == nullptr) {
        return;
    }
    auto* label = row->findChild<QLabel*>("propertiesRowLabel");
    expectations.expect(label != nullptr, "the row exposes its outer label");
    if (label == nullptr) {
        return;
    }
    expectations.expect(label->text() == QStringLiteral("Position"),
                        "at its preferred width the label shows the full parameter name");

    // Narrow enough to force eliding, but wide enough to fit at least one glyph plus the ellipsis
    // itself -- narrower than that, Qt::elidedText() gives up and returns an empty string, which
    // would trivially (and wrongly) satisfy a "not equal to the full text" check on its own.
    label->setFixedWidth(24);
    expectations.expect(
        label->text() != QStringLiteral("Position") &&
            label->text().endsWith(QString::fromUtf8("\xE2\x80\xA6")),
        "a narrow label column elides to an ellipsis instead of forcing the row wider");
    expectations.expect(label->toolTip() == QStringLiteral("Position"),
                        "the tooltip always carries the full, untruncated parameter name");
}

// task WIDTH-1: kit::KValueField::minimumSizeHint() is a real floor now (Size::ValueCellMin),
// smaller than its sizeHint() -- the widest-number PREFERRED width -- so a narrow Properties panel
// can shrink every value cell down to the same legible floor instead of each row committing to its
// own widest-possible-number width.
void testValueCellMinimumSizeHintIsAFloorBelowItsPreferredWidth(Expectations& expectations) {
    ui::kit::KValueField field;
    field.setRange(-1'000'000.0, 1'000'000.0);
    field.setDecimals(2);
    field.setUnit(QStringLiteral("px"));

    const int preferredWidth = field.sizeHint().width();
    const int minimumWidth = field.minimumSizeHint().width();
    expectations.expect(minimumWidth < preferredWidth,
                        "a value cell's minimum width is smaller than its widest-number "
                        "preferred width");
    expectations.expect(minimumWidth == ui::kit::px(ui::kit::Size::ValueCellMin),
                        "a label-less cell's floor is exactly Size::ValueCellMin");
}

// Task PROPS-1, deliverable 1: the panel is a stack of kit::KSection groups -- Object, Transform,
// and one per source -- each with a collapsible body, a persisted collapsed flag, and a header menu
// whose Collapse all / Expand all the PANEL answers for every section at once.
void testSectionsGroupCollapseAndPersist(Expectations& expectations) {
    auto newProject = document::makeNewProject("Sections Test", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);
    properties.resize(properties.sizeHint());
    properties.show();
    QCoreApplication::processEvents();

    auto* object = properties.findChild<ui::kit::KSection*>("propertiesSection_object");
    auto* transform = properties.findChild<ui::kit::KSection*>("propertiesSection_transform");
    auto* solid = properties.findChild<ui::kit::KSection*>("propertiesSection_solid");
    expectations.expect(object != nullptr && transform != nullptr && solid != nullptr,
                        "the panel groups a layer as Object, Transform, and one source section");
    if (object == nullptr || transform == nullptr || solid == nullptr) {
        return;
    }
    expectations.expect(
        object->title() == QStringLiteral("Object") &&
            transform->title() == QStringLiteral("Transform") &&
            solid->title() == QStringLiteral("Solid Source"),
        "section titles are Title Case, matching the timeline's own grouping names");

    // Every hand-crafted row still lives inside the section that owns it, so the grouping is real
    // structure rather than a header painted above an undifferentiated list.
    expectations.expect(properties.findChild<ui::kit::KValueField*>("opacityEditor") != nullptr &&
                            object->body()->findChild<ui::kit::KValueField*>("opacityEditor") !=
                                nullptr,
                        "the Opacity row lives in the Object section");
    expectations.expect(transform->body()->findChild<ui::kit::KValueField*>("positionXEditor") !=
                            nullptr,
                        "the Position row lives in the Transform section");
    expectations.expect(solid->body()->findChild<ui::kit::KValueField*>("solidColorRedEditor") !=
                            nullptr,
                        "the RGBA row lives in the source section");

    expectations.expect(!object->isCollapsed() && object->body()->isVisible(),
                        "a section starts expanded");
    object->setCollapsed(true);
    QCoreApplication::processEvents();
    expectations.expect(object->isCollapsed() && !object->body()->isVisible(),
                        "collapsing a section hides its body");
    expectations.expect(object->persistenceKey() ==
                            QStringLiteral("properties/sections/object/collapsed"),
                        "a section persists its collapsed flag under its own id");
    expectations.expect(QSettings().value(object->persistenceKey()).toBool(),
                        "collapsing writes the persisted flag");
    object->setCollapsed(false);
    QSettings().remove(QStringLiteral("properties/sections"));

    // Collapse all reaches every section, not just the one whose menu raised it.
    Q_EMIT transform->collapseAllRequested();
    QCoreApplication::processEvents();
    expectations.expect(object->isCollapsed() && transform->isCollapsed() && solid->isCollapsed(),
                        "Collapse all collapses every section in the panel");
    Q_EMIT transform->expandAllRequested();
    QCoreApplication::processEvents();
    expectations.expect(!object->isCollapsed() && !transform->isCollapsed() &&
                            !solid->isCollapsed(),
                        "Expand all expands every section in the panel");
    QSettings().remove(QStringLiteral("properties/sections"));
}

// The Object section's Visible/Solo/Locked switches author the layer boundary through the same
// commands the timeline's toggle strip uses, so one undo reverses either surface identically.
void testObjectSwitchesAuthorTheLayerBoundary(Expectations& expectations) {
    auto newProject = document::makeNewProject("Object Toggles", "Main", time(10));
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack stack(document);
    const auto ids = addSolidLayer(document, stack);

    ui::CompositionSession session(document, stack, compositionId);
    prepareColor(session);
    session.selectLayer(ids.layer);
    ui::PropertiesEditor properties(session);

    auto* visible = properties.findChild<ui::kit::KSwitch*>("layerVisibleSwitch");
    auto* solo = properties.findChild<ui::kit::KSwitch*>("layerSoloSwitch");
    auto* locked = properties.findChild<ui::kit::KSwitch*>("layerLockedSwitch");
    expectations.expect(visible != nullptr && solo != nullptr && locked != nullptr,
                        "the Object section exposes Visible, Solo and Locked switches");
    if (visible == nullptr || solo == nullptr || locked == nullptr) {
        return;
    }
    expectations.expect(visible->isChecked() && !solo->isChecked() && !locked->isChecked(),
                        "the switches read the layer boundary's own flags");

    solo->setChecked(true);
    const auto* boundary = session.composition()->graph().findLayer(ids.layer);
    expectations.expect(boundary != nullptr && boundary->solo,
                        "toggling Solo writes the boundary through a command");
    expectations.expect(session.canUndo() && session.undo(), "the toggle is one undoable command");
    const auto* restored = session.composition()->graph().findLayer(ids.layer);
    expectations.expect(restored != nullptr && !restored->solo, "undo restores the Solo flag");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testSelectionShowsGroupedRowsWithValuesAndUnits(expectations);
    testAnimatedParameterShowsGoldStaticShowsDim(expectations);
    testRowNeverPaintsWholeRowHover(expectations);
    testNoSelectionShowsDocumentProperties(expectations);
    testSelectionSwapUpdatesRows(expectations);
    testRgbaCellsEditThroughCommandWithUndo(expectations);
    testTransformRowsEditThroughCommandsWithUndo(expectations);
    testBlendingRowEditsThroughOneCommandWithUndo(expectations);
    testTransformRowsShowTheirOwnKeyframeIndicators(expectations);
    testRgbaCellsNeverClipNegativeOrHdrChannels(expectations);
    testScrubOnRgbaCellChangesValue(expectations);
    testFocusedHoveredCellBorderIsAccentOnScreen(expectations);
    testTextSourceRowsEditThroughCommands(expectations);
    testLongLabelColumnElidesWhenNarrowAndKeepsTheFullNameAsATooltip(expectations);
    testValueCellMinimumSizeHintIsAFloorBelowItsPreferredWidth(expectations);
    testSectionsGroupCollapseAndPersist(expectations);
    testObjectSwitchesAuthorTheLayerBoundary(expectations);
    if (expectations.failures() > 0) {
        std::cerr << expectations.failures() << " properties editor expectation(s) failed\n";
        return 1;
    }
    return 0;
}
