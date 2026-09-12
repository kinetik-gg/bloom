#include <bloom/ui/properties_editor.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/embedded_fonts.hpp>

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QSignalBlocker>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <optional>
#include <variant>

namespace bloom::ui {
namespace {

const document::NodeRecord* selectedPresentationSource(const CompositionSession& session) {
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return directSourceNode(session, *layerId);
    }
    return session.selectedNode();
}

// Issue #120 (task U5), decisions 1/2: the properties panel's kit field grid. A row is
// [right-aligned Muted label][optional gold/dimmed Keyframe indicator][value widget]. Task P1/P2
// (owner review 2026-09-12) removed both the selection title row above the grid and the row's own
// whole-row hover fill: the owner's read was "hover is for the component being interacted, not the
// whole row," so a row is now a plain, non-painting QWidget that exists only to lay its
// label/indicator/value out together -- every hover and focus affordance comes from the kit
// control itself (KValueField's own borderToken()/cellBorderColor(), kit::borderForInteraction()),
// never from this container.
QWidget* addPropertyRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label,
                        QLabel* indicator, QWidget* value) {
    auto* row = new QWidget(sectionParent);
    row->setObjectName(QStringLiteral("propertiesRow"));
    row->setMinimumHeight(kit::px(kit::Size::Control));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS),
                               kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS));
    layout->setSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(label);
    if (indicator != nullptr) {
        layout->addWidget(indicator);
    }
    layout->addWidget(value, 1);
    section->addWidget(row);
    return row;
}

// The fixed right-aligned label column every row in the panel shares (decision 1), sized once from
// the widest label this panel can ever show rather than a spelled pixel width -- KValueField's own
// internal sub-label column (kLabelColumnWidth in value_field.cpp) is private to that widget and
// exists for a narrower purpose (a field-local "X"/"Y" prefix inside the cell itself), so the row's
// OUTER label column, which names the whole parameter, is measured independently here.
int propertyLabelColumnWidth() {
    static const std::array<QString, 18> kLabels{
        PropertiesEditor::tr("Position"),     PropertiesEditor::tr("Anchor"),
        PropertiesEditor::tr("Scale"),        PropertiesEditor::tr("Rotation"),
        PropertiesEditor::tr("Opacity"),      PropertiesEditor::tr("Blending"),
        PropertiesEditor::tr("RGBA"),
        PropertiesEditor::tr("Alpha"),        PropertiesEditor::tr("Encoding"),
        PropertiesEditor::tr("Name"),         PropertiesEditor::tr("Format"),
        PropertiesEditor::tr("Frame Rate"),   PropertiesEditor::tr("Duration"),
        PropertiesEditor::tr("Pixel Aspect"), PropertiesEditor::tr("Content"),
        PropertiesEditor::tr("Size"),         PropertiesEditor::tr("Color"),
        PropertiesEditor::tr("Font"),
    };
    const QFontMetrics metrics(kit::font(kit::TypeRole::Ui));
    int widest = 0;
    for (const auto& label : kLabels) {
        widest = std::max(widest, metrics.horizontalAdvance(label));
    }
    return widest;
}

QLabel* makePropertyRowLabel(const QString& text, const int columnWidth, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("propertiesRowLabel"));
    label->setFont(kit::font(kit::TypeRole::Ui));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    label->setPalette(palette);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setFixedWidth(columnWidth);
    return label;
}

QLabel* makeKeyframeIndicator(QWidget* parent) {
    auto* indicator = new QLabel(parent);
    indicator->setObjectName(QStringLiteral("propertiesKeyframeIndicator"));
    indicator->setFixedSize(kit::px(kit::Size::IconSmall), kit::px(kit::Size::IconSmall));
    return indicator;
}

// A read-only value cell's text: `role` is Value (Geist Mono) for numeric-looking content --
// format, frame rate, duration, pixel aspect -- and Ui for prose -- the alpha association
// sentence, the color encoding name, the composition name. None of these are editable through the
// current session API (issue #120, decision 1's read-only carve-out: "do not add editing
// capability that doesn't exist today"), so they stay plain selectable text rather than
// kit::KValueField, which has no way to carry a string and would otherwise misrepresent them as
// steppable controls. Task P3 (owner review 2026-09-12) moved the RGBA row itself off this
// carve-out: it is a real parameter today, so it gets kit::KValueField cells instead -- see
// solidColorRed_/Green_/Blue_/Alpha_ below.
QLabel* makeReadOnlyValueLabel(const kit::TypeRole role, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFont(kit::font(role));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Foreground));
    label->setPalette(palette);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

// A UISmall uppercase group header (decision 1: "Transform", "Appearance", a source-specific
// group) plus its hairline divider (decision 1: "section dividers as hairlines"), appended to
// `section`.
void addSectionHeader(QVBoxLayout* section, QWidget* parent, const QString& title) {
    // "editorSectionTitle" already names TimelineEditor's "Layers" title and MediaEditor's
    // "Project" title (unchanged by this task): reused here rather than a new name so every group
    // header in the workspace -- Transform/Appearance/Solid Source included -- is the same logical
    // widget kind, per decision 4's "existing objectNames preserved."
    auto* header = new QLabel(title.toUpper(), parent);
    header->setObjectName(QStringLiteral("editorSectionTitle"));
    header->setFont(kit::font(kit::TypeRole::UiSmall));
    QPalette headerPalette = header->palette();
    headerPalette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    header->setPalette(headerPalette);
    section->addWidget(header);

    auto* divider = new QWidget(parent);
    divider->setObjectName(QStringLiteral("propertiesSectionDivider"));
    divider->setFixedHeight(static_cast<int>(std::lround(kit::kHairlineWidth)));
    QPalette dividerPalette = divider->palette();
    dividerPalette.setColor(QPalette::Window, kit::color(kit::Color::Border));
    divider->setPalette(dividerPalette);
    divider->setAutoFillBackground(true);
    section->addWidget(divider);
}

// True when `parameter` carries an animation curve source (issue #120, decision 2: "truth from
// the session snapshot, no new session API") -- the same std::holds_alternative check
// parameterSourceDescription() below already uses to report "Animated", read directly rather than
// by string-comparing that tooltip text.
bool isAnimatedParameter(const document::ParameterRecord* parameter) {
    return parameter != nullptr &&
           std::holds_alternative<document::AnimationCurveSource>(parameter->source);
}

// Paints `indicator` gold-filled when `parameter` is animation-sourced, Muted-dimmed-outline
// otherwise (decision 2). The dim opacity reuses tokens::kDisabledOpacity rather than a new
// literal: "dimmed" and "disabled ink" are the same fade recipe applied to a different ink. The
// weight switch follows docs/ux/visual-language.md's own iconography rule verbatim ("regular is
// the default visual weight and fill for selected or toggled states"): an animated parameter is
// this indicator's "on" state, so it takes the solid diamond-fill glyph rather than the outline
// one static rows show.
void updateKeyframeIndicator(QLabel* indicator, const document::ParameterRecord* parameter) {
    const bool animated = isAnimatedParameter(parameter);
    const QColor tint =
        animated ? kit::color(kit::Color::Keyframe)
                 : kit::withOpacity(kit::color(kit::Color::Muted), kit::kDisabledOpacity);
    const auto weight = animated ? kit::IconWeight::Fill : kit::IconWeight::Regular;
    indicator->setPixmap(
        kit::iconPixmap(kit::IconId::Keyframe, kit::Size::IconSmall, tint, 0.0, weight));
    indicator->setToolTip(animated ? PropertiesEditor::tr("Animated")
                                   : PropertiesEditor::tr("Static"));
}

// Frame rate as an exact rational (decision 3: "exact rational shown honestly"): numerator and
// denominator are exact std::uint32_t, so this never rounds -- it only omits the denominator when
// it is exactly 1 (the common whole-fps case) rather than always spelling "24/1 fps".
QString formatFrameRate(const document::FrameRate rate) {
    if (rate.denominator() == 1) {
        return PropertiesEditor::tr("%1 fps").arg(rate.numerator());
    }
    return PropertiesEditor::tr("%1/%2 fps").arg(rate.numerator()).arg(rate.denominator());
}

QString formatPixelAspect(const core::PixelAspectRatio pixelAspect) {
    return QStringLiteral("%1:%2").arg(pixelAspect.numerator()).arg(pixelAspect.denominator());
}

QString formatCompositionFormat(const document::CompositionFormat format) {
    return PropertiesEditor::tr("%1 × %2 px").arg(format.width()).arg(format.height());
}

// Duration as frame count + exact seconds (decision 3: "duration (frames + seconds via the exact
// formatting rule)"), reusing formatExactSeconds() above verbatim -- the SAME truncated-rational
// formatter TimelineEditor::updateTimeReadout() uses for the current-time readout, rather than a
// second, possibly-inconsistent formatting rule for duration. Frame count is maxFrameIndex + 1
// (the greatest valid index is 0-based).
QString formatDuration(const TimelineFrameContext& context) {
    return PropertiesEditor::tr("%1 frames · %2")
        .arg(context.maxFrameIndexValue + 1)
        .arg(formatExactSeconds(context.duration));
}

} // namespace

PropertiesEditor::PropertiesEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("propertiesEditor");
    setAccessibleName(tr("Properties editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    // Task P1 (owner review 2026-09-12: "should not show 'Nothing selected' or any other selected
    // layer info") removed the selection title row that used to sit here ("Solid 1", "Nothing
    // selected", ...) entirely. With nothing selected the panel shows only the document/composition
    // section below; with a selection it shows only the Transform/Appearance/source-specific
    // groups. Section headers (TRANSFORM/APPEARANCE/SOLID SOURCE/COMPOSITION) are the only grouping
    // left.

    // Issue #120 (task U5), decision 1: the kit field grid. Every row's label lives in ONE fixed,
    // right-aligned column shared across the whole panel -- Transform, Appearance, the
    // source-specific group, and the no-selection Composition group alike -- computed once from
    // the widest label text this panel can ever show (propertyLabelColumnWidth() above), not from
    // a spelled pixel width.
    const int labelColumnWidth = propertyLabelColumnWidth();

    // --- Selection-driven groups (Transform / Appearance / source-specific) --------------------
    selectionSection_ = new QWidget(this);
    selectionSection_->setObjectName(QStringLiteral("propertiesSelectionSection"));
    auto* selectionLayout = new QVBoxLayout(selectionSection_);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(kit::px(kit::Spacing::XS));

    addSectionHeader(selectionLayout, selectionSection_, tr("Transform"));

    positionX_ = new kit::KValueField(selectionSection_);
    positionY_ = new kit::KValueField(selectionSection_);
    for (auto* field : {positionX_, positionY_}) {
        field->setRange(-1'000'000.0, 1'000'000.0);
        field->setDecimals(2);
        field->setSingleStep(1.0);
        field->setUnit(QStringLiteral("px"));
    }
    positionX_->setObjectName("positionXEditor");
    positionX_->setAccessibleName(tr("Position X"));
    positionX_->setLabel(QStringLiteral("X"));
    positionY_->setObjectName("positionYEditor");
    positionY_->setAccessibleName(tr("Position Y"));
    positionY_->setLabel(QStringLiteral("Y"));

    auto* positionFields = new QWidget(selectionSection_);
    positionFields->setObjectName(QStringLiteral("positionFieldGroup"));
    auto* positionFieldsLayout = new QHBoxLayout(positionFields);
    positionFieldsLayout->setContentsMargins(0, 0, 0, 0);
    positionFieldsLayout->setSpacing(kit::px(kit::Spacing::S));
    positionFieldsLayout->addWidget(positionX_);
    positionFieldsLayout->addWidget(positionY_);

    positionKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Position"), labelColumnWidth, selectionSection_),
                   positionKeyframe_, positionFields);

    // Anchor, Scale, and Rotation complete the Transform group, in the registered parameter order
    // (see src/document/node_definition_registry.cpp). Each mirrors Position's own construction --
    // one paired X/Y field group or one single field, one keyframe indicator, one row -- so the
    // three read as siblings of Position rather than as a separate kind of control.
    //
    // The anchor is in the same pixel space Position is, so it takes Position's range, decimals,
    // step, and unit verbatim. Like Position's, that range is "unbounded in practice" rather than
    // truly unbounded: kit::KValueField's setRange() always clamps, and +-infinity or the finite
    // double extremes both wreck sizeHint() (see the solid colour cells below for the full
    // reasoning), so +-1'000'000 px stands in for no limit.
    anchorX_ = new kit::KValueField(selectionSection_);
    anchorY_ = new kit::KValueField(selectionSection_);
    for (auto* field : {anchorX_, anchorY_}) {
        field->setRange(-1'000'000.0, 1'000'000.0);
        field->setDecimals(2);
        field->setSingleStep(1.0);
        field->setUnit(QStringLiteral("px"));
    }
    anchorX_->setObjectName("anchorXEditor");
    anchorX_->setAccessibleName(tr("Anchor X"));
    anchorX_->setLabel(QStringLiteral("X"));
    anchorY_->setObjectName("anchorYEditor");
    anchorY_->setAccessibleName(tr("Anchor Y"));
    anchorY_->setLabel(QStringLiteral("Y"));

    auto* anchorFields = new QWidget(selectionSection_);
    anchorFields->setObjectName(QStringLiteral("anchorFieldGroup"));
    auto* anchorFieldsLayout = new QHBoxLayout(anchorFields);
    anchorFieldsLayout->setContentsMargins(0, 0, 0, 0);
    anchorFieldsLayout->setSpacing(kit::px(kit::Spacing::S));
    anchorFieldsLayout->addWidget(anchorX_);
    anchorFieldsLayout->addWidget(anchorY_);

    anchorKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Anchor"), labelColumnWidth, selectionSection_),
                   anchorKeyframe_, anchorFields);

    // Scale is authored as a PERCENTAGE in the panel and stored as a unitless factor, exactly the
    // way Opacity is authored as a percentage and stored in [0, 1]. The scrub step is 1%, the
    // smallest step that reads as a deliberate size change. Negative percentages are reachable
    // because a negative factor is a legitimate mirror of the axis.
    scaleX_ = new kit::KValueField(selectionSection_);
    scaleY_ = new kit::KValueField(selectionSection_);
    for (auto* field : {scaleX_, scaleY_}) {
        field->setRange(-100'000.0, 100'000.0);
        field->setDecimals(2);
        field->setSingleStep(1.0);
        field->setUnit(QStringLiteral("%"));
    }
    scaleX_->setObjectName("scaleXEditor");
    scaleX_->setAccessibleName(tr("Scale X"));
    scaleX_->setLabel(QStringLiteral("X"));
    scaleY_->setObjectName("scaleYEditor");
    scaleY_->setAccessibleName(tr("Scale Y"));
    scaleY_->setLabel(QStringLiteral("Y"));

    auto* scaleFields = new QWidget(selectionSection_);
    scaleFields->setObjectName(QStringLiteral("scaleFieldGroup"));
    auto* scaleFieldsLayout = new QHBoxLayout(scaleFields);
    scaleFieldsLayout->setContentsMargins(0, 0, 0, 0);
    scaleFieldsLayout->setSpacing(kit::px(kit::Spacing::S));
    scaleFieldsLayout->addWidget(scaleX_);
    scaleFieldsLayout->addWidget(scaleY_);

    scaleKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Scale"), labelColumnWidth, selectionSection_),
                   scaleKeyframe_, scaleFields);

    // Rotation is a single degree field with a 1 degree scrub step. Its range is deliberately wider
    // than one turn: the schema accepts any finite angle so a rotation curve can wind, and a field
    // that clamped at 360 would silently refuse an authored 450.
    rotation_ = new kit::KValueField(selectionSection_);
    rotation_->setObjectName("rotationEditor");
    rotation_->setAccessibleName(tr("Rotation"));
    rotation_->setRange(-100'000.0, 100'000.0);
    rotation_->setDecimals(2);
    rotation_->setSingleStep(1.0);
    rotation_->setUnit(QString::fromUtf8("\u00b0"));

    rotationKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Rotation"), labelColumnWidth, selectionSection_),
                   rotationKeyframe_, rotation_);

    addSectionHeader(selectionLayout, selectionSection_, tr("Appearance"));

    opacity_ = new kit::KValueField(selectionSection_);
    opacity_->setObjectName("opacityEditor");
    opacity_->setAccessibleName(tr("Opacity"));
    opacity_->setRange(0.0, 100.0);
    opacity_->setDecimals(1);
    opacity_->setSingleStep(1.0);
    opacity_->setUnit(QStringLiteral("%"));

    opacityKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Opacity"), labelColumnWidth, selectionSection_),
                   opacityKeyframe_, opacity_);

    // Blending completes the Appearance group, in the registered parameter order. The items are
    // core::kBlendModes in order, named by the one shared vocabulary, with the mode's stored integer
    // as item data so the control never depends on the order it happened to be filled in -- exactly
    // the timeline row's dropdown, because both author the same parameter through the same session
    // method. No keyframe indicator: the schema is not animatable.
    blendMode_ = new kit::KDropdown(selectionSection_);
    blendMode_->setObjectName("blendModeEditor");
    blendMode_->setAccessibleName(tr("Blending"));
    for (const auto mode : core::kBlendModes) {
        blendMode_->addItem(blendModeDisplayName(mode),
                            QVariant::fromValue(core::blendModeStoredValue(mode)));
    }
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Blending"), labelColumnWidth, selectionSection_),
                   nullptr, blendMode_);

    solidColorPanel_ = new QWidget(selectionSection_);
    solidColorPanel_->setObjectName("solidColorProperties");
    auto* solidColorLayout = new QVBoxLayout(solidColorPanel_);
    solidColorLayout->setContentsMargins(0, 0, 0, 0);
    solidColorLayout->setSpacing(kit::px(kit::Spacing::XS));
    addSectionHeader(solidColorLayout, solidColorPanel_, tr("Solid Source"));

    // Task P3 (owner review 2026-09-12: "params not yet editable like the RGBA values of a
    // solid... simple input fields are not being implemented yet and that sucks"): four
    // kit::KValueField cells, one per channel, in place of the former read-only solidColorValue_
    // label. Straight scene-linear authoring values are the schema (document::
    // kSolidColorParameterSchemaKey), and per FORMAL AMENDMENT 1 (2026-09-12) these cells are
    // UNBOUNDED, exactly like the read-only label they replace: they never clip negative or HDR
    // channels, display the exact stored value, and commit exactly what was typed or scrubbed.
    //
    // kit::KValueField has no true "no limit" range -- setRange() always clamps in commitValue()
    // -- and this task cannot edit the kit (no kit changes). A literal +-infinity range clamps
    // nothing either, but it wrecks sizeHint(): KValueField sizes its cell from
    // QString::number(max(|minimum|, |maximum|), 'f', decimals), and Qt's own formatter special-
    // cases +-infinity to the 3-4 character strings "inf"/"-inf" rather than a wide number,
    // producing a cell too narrow for any real value. The finite extremes
    // (std::numeric_limits<double>::lowest()/max()) are worse: that same call produces a
    // 300+ character string. -1'000'000/1'000'000 is exactly Position's own existing "unbounded in
    // practice" bound two rows up in this same panel: no realistic scene-linear authoring value
    // (this task's own test fixtures included) reaches it, so nothing is ever actually clipped,
    // while sizeHint() stays sane.
    solidColorRed_ = new kit::KValueField(solidColorPanel_);
    solidColorGreen_ = new kit::KValueField(solidColorPanel_);
    solidColorBlue_ = new kit::KValueField(solidColorPanel_);
    solidColorAlpha_ = new kit::KValueField(solidColorPanel_);
    for (auto* field : {solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_}) {
        field->setRange(-1'000'000.0, 1'000'000.0);
        field->setDecimals(3);
        field->setSingleStep(0.01);
    }
    solidColorRed_->setObjectName("solidColorRedEditor");
    solidColorRed_->setAccessibleName(tr("Solid color red"));
    solidColorRed_->setLabel(QStringLiteral("R"));
    solidColorGreen_->setObjectName("solidColorGreenEditor");
    solidColorGreen_->setAccessibleName(tr("Solid color green"));
    solidColorGreen_->setLabel(QStringLiteral("G"));
    solidColorBlue_->setObjectName("solidColorBlueEditor");
    solidColorBlue_->setAccessibleName(tr("Solid color blue"));
    solidColorBlue_->setLabel(QStringLiteral("B"));
    solidColorAlpha_->setObjectName("solidColorAlphaEditor");
    solidColorAlpha_->setAccessibleName(tr("Solid color alpha"));
    solidColorAlpha_->setLabel(QStringLiteral("A"));

    auto* solidColorFields = new QWidget(solidColorPanel_);
    solidColorFields->setObjectName(QStringLiteral("solidColorFieldGroup"));
    auto* solidColorFieldsLayout = new QHBoxLayout(solidColorFields);
    solidColorFieldsLayout->setContentsMargins(0, 0, 0, 0);
    solidColorFieldsLayout->setSpacing(kit::px(kit::Spacing::S));
    solidColorFieldsLayout->addWidget(solidColorRed_);
    solidColorFieldsLayout->addWidget(solidColorGreen_);
    solidColorFieldsLayout->addWidget(solidColorBlue_);
    solidColorFieldsLayout->addWidget(solidColorAlpha_);

    solidColorKeyframe_ = makeKeyframeIndicator(solidColorPanel_);
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("RGBA"), labelColumnWidth, solidColorPanel_),
                   solidColorKeyframe_, solidColorFields);

    solidAlphaAssociation_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, solidColorPanel_);
    solidAlphaAssociation_->setObjectName("solidAlphaAssociation");
    solidAlphaAssociation_->setAccessibleName(tr("Solid alpha association"));
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("Alpha"), labelColumnWidth, solidColorPanel_), nullptr,
                   solidAlphaAssociation_);

    solidColorEncoding_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, solidColorPanel_);
    solidColorEncoding_->setObjectName("solidColorEncoding");
    solidColorEncoding_->setAccessibleName(tr("Solid color encoding"));
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("Encoding"), labelColumnWidth, solidColorPanel_),
                   nullptr, solidColorEncoding_);

    selectionLayout->addWidget(solidColorPanel_);

    // --- Text Source (task S3) ----------------------------------------------------------------
    textSourcePanel_ = new QWidget(selectionSection_);
    textSourcePanel_->setObjectName("textSourceProperties");
    auto* textLayout = new QVBoxLayout(textSourcePanel_);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(kit::px(kit::Spacing::XS));
    addSectionHeader(textLayout, textSourcePanel_, tr("Text Source"));

    // The kit has no string field, and adding one is a kit change outside this task's fence, so the
    // content cell is a plain QLineEdit styled by the application palette like every other text
    // entry. It commits on editingFinished and returnPressed rather than on textChanged: a
    // per-keystroke commit would make typing one word a dozen undo steps and a dozen recompiles.
    textContent_ = new QLineEdit(textSourcePanel_);
    textContent_->setObjectName("textContentEditor");
    textContent_->setAccessibleName(tr("Text content"));
    textContent_->setFont(kit::font(kit::TypeRole::Ui));
    textContent_->setClearButtonEnabled(false);
    addPropertyRow(textLayout, textSourcePanel_,
                   makePropertyRowLabel(tr("Content"), labelColumnWidth, textSourcePanel_), nullptr,
                   textContent_);

    textSize_ = new kit::KValueField(textSourcePanel_);
    textSize_->setObjectName("textSizeEditor");
    textSize_->setAccessibleName(tr("Text size"));
    // The range is the text size schema's own domain, not a spelled UI guess: the document refuses
    // anything outside it, so a cell that could scrub past it would only produce refusals.
    textSize_->setRange(1.0, document::kMaximumTextSizePixels);
    textSize_->setDecimals(1);
    textSize_->setSingleStep(1.0);
    textSize_->setUnit(QStringLiteral("px"));
    addPropertyRow(textLayout, textSourcePanel_,
                   makePropertyRowLabel(tr("Size"), labelColumnWidth, textSourcePanel_), nullptr,
                   textSize_);

    textColor_ = new kit::KColorChip(textSourcePanel_);
    textColor_->setObjectName("textColorChip");
    textColor_->setAccessibleName(tr("Text color"));
    textColorKeyframe_ = makeKeyframeIndicator(textSourcePanel_);
    addPropertyRow(textLayout, textSourcePanel_,
                   makePropertyRowLabel(tr("Color"), labelColumnWidth, textSourcePanel_),
                   textColorKeyframe_, textColor_);

    // The face is fixed, so this row is read-only by honesty rather than by omission: showing a
    // font dropdown would promise a selection neither the document schema nor the renderer has.
    textFontName_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, textSourcePanel_);
    textFontName_->setObjectName("textFontName");
    textFontName_->setAccessibleName(tr("Text font"));
    addPropertyRow(textLayout, textSourcePanel_,
                   makePropertyRowLabel(tr("Font"), labelColumnWidth, textSourcePanel_), nullptr,
                   textFontName_);

    selectionLayout->addWidget(textSourcePanel_);
    selectionLayout->addStretch(1);
    layout->addWidget(selectionSection_);

    // --- No-selection document/composition view (decision 3) ----------------------------------
    documentSection_ = new QWidget(this);
    documentSection_->setObjectName(QStringLiteral("propertiesDocumentSection"));
    auto* documentLayout = new QVBoxLayout(documentSection_);
    documentLayout->setContentsMargins(0, 0, 0, 0);
    documentLayout->setSpacing(kit::px(kit::Spacing::XS));
    addSectionHeader(documentLayout, documentSection_, tr("Composition"));

    documentName_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, documentSection_);
    documentName_->setObjectName(QStringLiteral("documentName"));
    documentName_->setAccessibleName(tr("Composition name"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Name"), labelColumnWidth, documentSection_), nullptr,
                   documentName_);

    documentFormat_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentFormat_->setObjectName(QStringLiteral("documentFormat"));
    documentFormat_->setAccessibleName(tr("Composition format"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Format"), labelColumnWidth, documentSection_), nullptr,
                   documentFormat_);

    documentFrameRate_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentFrameRate_->setObjectName(QStringLiteral("documentFrameRate"));
    documentFrameRate_->setAccessibleName(tr("Composition frame rate"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Frame Rate"), labelColumnWidth, documentSection_),
                   nullptr, documentFrameRate_);

    documentDuration_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentDuration_->setObjectName(QStringLiteral("documentDuration"));
    documentDuration_->setAccessibleName(tr("Composition duration"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Duration"), labelColumnWidth, documentSection_),
                   nullptr, documentDuration_);

    documentPixelAspect_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentPixelAspect_->setObjectName(QStringLiteral("documentPixelAspect"));
    documentPixelAspect_->setAccessibleName(tr("Composition pixel aspect ratio"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Pixel Aspect"), labelColumnWidth, documentSection_),
                   nullptr, documentPixelAspect_);

    // Color settings (process space + config name) are read from ProjectSession, not from
    // anything CompositionSession exposes (src/host/include/bloom/host/project_session.hpp) --
    // document::Composition/Snapshot carry no ColorSettings at all (verified: grep finds
    // ColorSettings only under src/host and src/project, never src/document). Per decision 3 ("if
    // a listed fact is not reachable via existing read-only API, omit it and report rather than
    // adding API"), the color settings summary row is omitted here; see this task's raw report.

    documentLayout->addStretch(1);
    layout->addWidget(documentSection_);

    const auto commitPosition = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedPosition(positionX_->value(), positionY_->value());
        }
    };
    connect(positionX_, &kit::KValueField::valueChanged, this, commitPosition);
    connect(positionY_, &kit::KValueField::valueChanged, this, commitPosition);
    const auto commitAnchor = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedAnchor(anchorX_->value(), anchorY_->value());
        }
    };
    connect(anchorX_, &kit::KValueField::valueChanged, this, commitAnchor);
    connect(anchorY_, &kit::KValueField::valueChanged, this, commitAnchor);
    const auto commitScale = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedScale(scaleX_->value() / 100.0, scaleY_->value() / 100.0);
        }
    };
    connect(scaleX_, &kit::KValueField::valueChanged, this, commitScale);
    connect(scaleY_, &kit::KValueField::valueChanged, this, commitScale);
    connect(rotation_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (!rebuilding_) {
            (void)session_.setSelectedRotation(value);
        }
    });
    connect(blendMode_, &kit::KDropdown::currentIndexChanged, this, [this](const int index) {
        if (rebuilding_ || index < 0) {
            return;
        }
        const auto mode =
            core::blendModeFromStoredValue(blendMode_->itemData(index).value<std::int64_t>());
        if (mode.has_value()) {
            (void)session_.setSelectedBlendMode(*mode);
        }
    });
    connect(opacity_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (!rebuilding_) {
            (void)session_.setSelectedOpacity(value / 100.0);
        }
    });
    // Task P3: exactly Position's own commitPosition shape (read every cell in the group, write
    // the whole value through one session call) -- one SetSolidColor command per emitted
    // valueChanged, the same one-command-per-emission parity Position already has (BASE FACTS:
    // "accept that parity; do not add coalescing").
    const auto commitSolidColor = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedSolidColor(
                core::Color4d{solidColorRed_->value(), solidColorGreen_->value(),
                              solidColorBlue_->value(), solidColorAlpha_->value()});
        }
    };
    const auto commitTextContent = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedTextContent(textContent_->text());
        }
    };
    connect(textContent_, &QLineEdit::editingFinished, this, commitTextContent);
    connect(textSize_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (!rebuilding_) {
            (void)session_.setSelectedTextSize(value);
        }
    });
    connect(textColor_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
        if (!rebuilding_) {
            (void)session_.setSelectedTextColor(
                core::Color4d{static_cast<double>(color.red), static_cast<double>(color.green),
                              static_cast<double>(color.blue), static_cast<double>(color.alpha)});
        }
    });
    connect(solidColorRed_, &kit::KValueField::valueChanged, this, commitSolidColor);
    connect(solidColorGreen_, &kit::KValueField::valueChanged, this, commitSolidColor);
    connect(solidColorBlue_, &kit::KValueField::valueChanged, this, commitSolidColor);
    connect(solidColorAlpha_, &kit::KValueField::valueChanged, this, commitSolidColor);
    connect(&session_, &CompositionSession::snapshotChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this, &PropertiesEditor::rebuild);

    rebuild();
}

void PropertiesEditor::rebuild() {
    rebuilding_ = true;
    configurePosition();
    configureAnchor();
    configureScale();
    configureRotation();
    configureOpacity();
    configureBlendMode();
    configureSolidColor();
    configureTextSource();
    configureDocumentProperties();
    rebuilding_ = false;
}

void PropertiesEditor::configurePosition() {
    const auto* position = session_.parameterForSelection(document::kPositionParameterRole);
    const auto positionValue =
        position == nullptr ? std::nullopt : session_.constantVec2Value(position->id);
    const bool canEditPosition = positionValue.has_value();
    positionX_->setEnabled(canEditPosition);
    positionY_->setEnabled(canEditPosition);
    if (canEditPosition) {
        const QSignalBlocker blockX(positionX_);
        const QSignalBlocker blockY(positionY_);
        positionX_->setValue(positionValue->x);
        positionY_->setValue(positionValue->y);
    }
    const QString positionTip = position == nullptr
                                    ? tr("Position is not exposed by this selection")
                                    : parameterSourceDescription(*position);
    positionX_->setToolTip(positionTip);
    positionY_->setToolTip(positionTip);
    updateKeyframeIndicator(positionKeyframe_, position);
}

void PropertiesEditor::configureAnchor() {
    const auto* anchor = session_.parameterForSelection(document::kAnchorParameterRole);
    const auto value = anchor == nullptr ? std::nullopt : session_.constantVec2Value(anchor->id);
    const bool editable = value.has_value();
    for (auto* field : {anchorX_, anchorY_}) {
        field->setEnabled(editable);
        field->setToolTip(anchor == nullptr ? tr("Anchor is not exposed by this selection")
                                            : parameterSourceDescription(*anchor));
    }
    if (editable) {
        const QSignalBlocker blockX(anchorX_);
        const QSignalBlocker blockY(anchorY_);
        anchorX_->setValue(value->x);
        anchorY_->setValue(value->y);
    }
    updateKeyframeIndicator(anchorKeyframe_, anchor);
}

void PropertiesEditor::configureScale() {
    const auto* scale = session_.parameterForSelection(document::kScaleParameterRole);
    const auto value = scale == nullptr ? std::nullopt : session_.constantVec2Value(scale->id);
    const bool editable = value.has_value();
    for (auto* field : {scaleX_, scaleY_}) {
        field->setEnabled(editable);
        field->setToolTip(scale == nullptr ? tr("Scale is not exposed by this selection")
                                           : parameterSourceDescription(*scale));
    }
    const QSignalBlocker blockX(scaleX_);
    const QSignalBlocker blockY(scaleY_);
    // Stored as a unitless factor, shown as a percentage: an unscaled layer reads 100%.
    scaleX_->setValue(editable ? value->x * 100.0 : 100.0);
    scaleY_->setValue(editable ? value->y * 100.0 : 100.0);
    updateKeyframeIndicator(scaleKeyframe_, scale);
}

void PropertiesEditor::configureRotation() {
    const auto* parameter = session_.parameterForSelection(document::kRotationParameterRole);
    const auto value = parameter == nullptr ? std::nullopt : session_.constantValue(parameter->id);
    rotation_->setEnabled(value.has_value());
    const QSignalBlocker blocker(rotation_);
    rotation_->setValue(value.value_or(0.0));
    rotation_->setToolTip(parameter == nullptr ? tr("Rotation is not exposed by this selection")
                                               : parameterSourceDescription(*parameter));
    updateKeyframeIndicator(rotationKeyframe_, parameter);
}

void PropertiesEditor::configureOpacity() {
    const auto* parameter = session_.parameterForSelection(document::kOpacityParameterRole);
    const auto value = parameter == nullptr ? std::nullopt : session_.constantValue(parameter->id);
    opacity_->setEnabled(value.has_value());
    const QSignalBlocker blocker(opacity_);
    opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
    opacity_->setToolTip(parameter == nullptr ? tr("Opacity is not exposed by this selection")
                                              : parameterSourceDescription(*parameter));
    updateKeyframeIndicator(opacityKeyframe_, parameter);
}

void PropertiesEditor::configureBlendMode() {
    // The contextual layer, not a parameter lookup on the selection: the selection may be the layer,
    // its Layer Output node, or one of its parameters, and all three mean the same layer's blending.
    const auto* direct = std::get_if<document::LayerId>(&session_.selection().primary);
    const auto layerId =
        direct != nullptr ? std::optional(*direct) : session_.selection().contextualLayer;
    const auto mode =
        layerId.has_value() ? session_.blendModeForLayer(*layerId) : std::nullopt;
    blendMode_->setEnabled(mode.has_value());
    const QSignalBlocker blocker(blendMode_);
    int row = 0;
    if (mode.has_value()) {
        const auto stored = core::blendModeStoredValue(*mode);
        for (int index = 0; index < blendMode_->count(); ++index) {
            if (blendMode_->itemData(index).value<std::int64_t>() == stored) {
                row = index;
                break;
            }
        }
    }
    blendMode_->setCurrentIndex(row);
    blendMode_->setToolTip(mode.has_value()
                               ? tr("How this layer combines with the layers beneath it")
                               : tr("Blending is not exposed by this selection"));
}

void PropertiesEditor::configureSolidColor() {
    const auto* parameter = session_.parameterForSelection(document::kSolidColorParameterRole);
    const auto* sourceNode = selectedPresentationSource(session_);
    const bool isSolid = isKnownSource(sourceNode, document::kSolidSourceNodeType,
                                       document::kSolidSourceNodeSchemaVersion) &&
                         parameter != nullptr &&
                         parameter->schemaKey == document::kSolidColorParameterSchemaKey;
    solidColorPanel_->setVisible(isSolid);
    if (!isSolid) {
        return;
    }

    updateKeyframeIndicator(solidColorKeyframe_, parameter);
    const auto value = session_.constantColorValue(parameter->id);
    const bool canEditColor = value.has_value();
    for (auto* field : {solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_}) {
        field->setEnabled(canEditColor);
    }
    if (canEditColor) {
        const QSignalBlocker blockRed(solidColorRed_);
        const QSignalBlocker blockGreen(solidColorGreen_);
        const QSignalBlocker blockBlue(solidColorBlue_);
        const QSignalBlocker blockAlpha(solidColorAlpha_);
        solidColorRed_->setValue(value->red);
        solidColorGreen_->setValue(value->green);
        solidColorBlue_->setValue(value->blue);
        solidColorAlpha_->setValue(value->alpha);
    }
    // Mirrors Position/Opacity's own tooltip shape exactly (parameterSourceDescription() for a
    // resolvable parameter).
    const QString colorTip = parameterSourceDescription(*parameter);
    for (auto* field : {solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_}) {
        field->setToolTip(colorTip);
    }
    solidAlphaAssociation_->setText(tr("Straight (unassociated)"));
    solidColorEncoding_->setText(
        QString::fromUtf8(document::kSolidColorEncoding.data(),
                          static_cast<qsizetype>(document::kSolidColorEncoding.size())));
}

void PropertiesEditor::configureTextSource() {
    const auto* sourceNode = selectedPresentationSource(session_);
    const bool isText = isKnownSource(sourceNode, document::kTextSourceNodeType,
                                      document::kTextSourceNodeSchemaVersion);
    const auto* content = session_.parameterForSelection(document::kTextParameterRole);
    const auto* size = session_.parameterForSelection(document::kTextSizeParameterRole);
    const auto* color = session_.parameterForSelection(document::kTextColorParameterRole);
    const bool resolved = isText && content != nullptr && size != nullptr && color != nullptr &&
                          content->schemaKey == document::kTextParameterSchemaKey &&
                          size->schemaKey == document::kTextSizeParameterSchemaKey &&
                          color->schemaKey == document::kTextColorParameterSchemaKey;
    textSourcePanel_->setVisible(resolved);
    if (!resolved) {
        return;
    }

    const auto contentValue = session_.constantStringValue(content->id);
    textContent_->setEnabled(contentValue.has_value());
    if (contentValue.has_value() && textContent_->text() != *contentValue) {
        const QSignalBlocker blocker(textContent_);
        textContent_->setText(*contentValue);
    }
    textContent_->setPlaceholderText(tr("Type the layer's text"));
    textContent_->setToolTip(parameterSourceDescription(*content));

    const auto sizeValue = session_.constantValue(size->id);
    textSize_->setEnabled(sizeValue.has_value());
    if (sizeValue.has_value()) {
        const QSignalBlocker blocker(textSize_);
        textSize_->setValue(*sizeValue);
    }
    textSize_->setToolTip(parameterSourceDescription(*size));

    const auto colorValue = session_.constantColorValue(color->id);
    textColor_->setEnabled(colorValue.has_value());
    if (colorValue.has_value()) {
        const QSignalBlocker blocker(textColor_);
        textColor_->setColor(kit::KColor::fromRgba(
            static_cast<float>(colorValue->red), static_cast<float>(colorValue->green),
            static_cast<float>(colorValue->blue), static_cast<float>(colorValue->alpha)));
    }
    // The chip's own value model is 8-bit-displayable straight RGBA in [0, 1], so an HDR or
    // negative authored channel cannot be shown in the swatch or round-tripped through the picker.
    // The exact stored value travels in the tooltip, and the tooltip says what committing through
    // the chip would do to such a value.
    textColor_->setToolTip(
        colorValue.has_value()
            ? tr("%1\nEditing here commits a color inside the displayable [0, 1] range")
                  .arg(exactColorText(*colorValue))
            : parameterSourceDescription(*color));
    updateKeyframeIndicator(textColorKeyframe_, color);

    textFontName_->setText(
        tr("%1 %2 (embedded)")
            .arg(QString::fromUtf8(
                render::kEmbeddedDejaVuSansFamilyName.data(),
                static_cast<qsizetype>(render::kEmbeddedDejaVuSansFamilyName.size())))
            .arg(QString::fromUtf8(
                render::kEmbeddedDejaVuSansStyleName.data(),
                static_cast<qsizetype>(render::kEmbeddedDejaVuSansStyleName.size()))));
}

void PropertiesEditor::configureDocumentProperties() {
    const auto* composition = session_.composition();
    const bool hasSelection = !std::holds_alternative<std::monostate>(session_.selection().primary);
    const bool showDocument = composition != nullptr && !hasSelection;
    documentSection_->setVisible(showDocument);
    selectionSection_->setVisible(!showDocument);
    if (!showDocument) {
        return;
    }

    documentName_->setText(QString::fromStdString(composition->name()));
    const auto format = composition->format();
    documentFormat_->setText(formatCompositionFormat(format));
    documentFrameRate_->setText(formatFrameRate(format.frameRate()));
    documentPixelAspect_->setText(formatPixelAspect(format.pixelAspect()));
    const auto context = frameContextFor(session_);
    documentDuration_->setText(context.has_value() ? formatDuration(*context)
                                                   : QStringLiteral("—"));
}

} // namespace bloom::ui
