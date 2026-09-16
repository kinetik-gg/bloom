#include <bloom/ui/kit/controls.hpp>
#include <memory>
// Task PROPS-1, deliverable 1: PropertiesEditor's CONSTRUCTION half.
//
// properties_editor.cpp had grown past a thousand lines with two jobs in it: building the panel's
// widget tree once, and projecting document truth onto that tree on every rebuild. They are split
// here along exactly that seam -- this file builds (every build*Section() and the one bindCommits()
// that wires every control to its session setter), properties_editor.cpp projects (rebuild() and
// every configure*()). They are member functions of the same class in two translation units, so the
// split costs no indirection and no new type.
//
// The row and section VOCABULARY they both use -- rows, labels, cell groups, sections -- lives one
// level further down in properties_sections.hpp, which knows nothing about a session at all.

#include "node_editor_items.hpp"
#include <bloom/ui/properties_editor.hpp>

#include "composition_editor_support.hpp"
#include "properties_anchor_grid.hpp"
#include "properties_sections.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace bloom::ui {
namespace {

using properties::addRow;
using properties::makeCellGroup;
using properties::makeKeyframeDiamond;
using properties::makeReadOnlyValueLabel;
using properties::makeRowLabel;
using properties::makeValueCell;
using properties::ValueCellSpec;

// The layer whose Object flags the switches author: the selection may be the layer itself, its
// Layer Output node, or one of its parameters, and all three mean the same layer.
std::optional<document::LayerId> contextualLayerId(const CompositionSession& session) {
    if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary)) {
        return *direct;
    }
    return session.selection().contextualLayer;
}

// The unbounded-in-practice range Position, Anchor and the RGBA cells all share.
// kit::KValueField::setRange() always clamps, and both true infinity and the finite double extremes
// wreck sizeHint(), so this stands in for "no limit": no realistic authored value reaches it, so
// nothing is ever actually clipped, while sizeHint() stays sane.
constexpr double kPracticallyUnbounded = 1'000'000.0;

kit::KSwitch* makeToggle(QWidget* parent, const QString& objectName, const QString& accessible) {
    auto* toggle = new kit::KCheckBox(parent);
    toggle->setObjectName(objectName);
    toggle->setAccessibleName(accessible);
    return toggle;
}

} // namespace

void PropertiesEditor::buildObjectSection(QVBoxLayout* layout) {
    auto* section =
        properties::addSection(layout, selectionSection_, QStringLiteral("object"), tr("Object"));
    adoptSection(section, {document::kOpacityParameterRole});
    auto* body = section->body();
    auto* rows = section->bodyLayout();

    // Visible / Solo / Locked each get their own labelled row rather than sharing one strip: the
    // panel's whole grammar is one right-aligned label naming one control, and three unlabelled
    // glyphs in a single row would be the timeline's toggle strip transplanted into a field grid.
    layerVisible_ = makeToggle(body, QStringLiteral("layerVisibleSwitch"), tr("Visible"));
    addRow(rows, body, makeRowLabel(tr("Visible"), body), nullptr, layerVisible_);
    layerSolo_ = makeToggle(body, QStringLiteral("layerSoloSwitch"), tr("Solo"));
    layerSolo_->setToolTip(tr("Solo: render only soloed layers"));
    addRow(rows, body, makeRowLabel(tr("Solo"), body), nullptr, layerSolo_);
    layerLocked_ = makeToggle(body, QStringLiteral("layerLockedSwitch"), tr("Locked"));
    addRow(rows, body, makeRowLabel(tr("Locked"), body), nullptr, layerLocked_);

    auto* parent = new kit::KDropdown(body);
    parent->setObjectName("propertiesParentDropdown");
    parent->setAccessibleName(tr("Parent"));
    parent->setToolTip(tr("Transform relative to another layer"));
    const auto refreshParent = [this, parent] {
        const QSignalBlocker blocker(parent);
        parent->clearItems();
        parent->addItem(tr("None"), QVariant::fromValue(qulonglong{0}));
        const auto layer = contextualLayerId(session_);
        const auto* composition = session_.composition();
        const auto* boundary =
            composition && layer ? composition->graph().findLayer(*layer) : nullptr;
        parent->setEnabled(boundary && !boundary->locked);
        if (!layer || !composition)
            return;
        for (const auto candidate : session_.candidateParents(*layer)) {
            const auto nodeId = session_.boundaryNodeForLayer(candidate);
            const auto* node = nodeId ? composition->graph().findNode(*nodeId) : nullptr;
            if (node)
                parent->addItem(node_editor::nodeDisplayName(*composition, *node),
                                QVariant::fromValue(static_cast<qulonglong>(candidate.value())));
        }
        const auto selected = session_.parentOf(*layer);
        parent->setCurrentIndex(parent->findData(
            QVariant::fromValue(static_cast<qulonglong>(selected ? selected->value() : 0))));
    };
    connect(&session_, &CompositionSession::selectionChanged, parent, refreshParent);
    connect(&session_, &CompositionSession::snapshotChanged, parent, refreshParent);
    connect(parent, &kit::KDropdown::currentIndexChanged, parent,
            [this, parent, refreshParent](int index) {
                const auto layer = contextualLayerId(session_);
                if (index < 0 || !layer)
                    return;
                const auto raw = parent->itemData(index).toULongLong();
                (void)session_.setLayerParent(
                    *layer, raw ? std::optional(document::LayerId::fromRaw(raw)) : std::nullopt);
                refreshParent();
            });
    refreshParent();
    addRow(rows, body, makeRowLabel(tr("Parent"), body), nullptr, parent);

    // The items are core::kBlendModes in order, named by the one shared vocabulary, with the mode's
    // stored integer as item data so the control never depends on the order it happened to be
    // filled in -- exactly the timeline row's dropdown, because both author the same parameter
    // through the same session method. No keyframe indicator: the schema is not animatable.
    blendMode_ = new kit::KDropdown(body);
    blendMode_->setObjectName("blendModeEditor");
    blendMode_->setAccessibleName(tr("Blending"));
    for (const auto mode : core::kBlendModes) {
        blendMode_->addItem(blendModeDisplayName(mode),
                            QVariant::fromValue(core::blendModeStoredValue(mode)));
    }
    addRow(rows, body, makeRowLabel(tr("Blending Mode"), body), nullptr, blendMode_);

    opacity_ = makeValueCell({.objectName = QStringLiteral("opacityEditor"),
                              .accessibleName = tr("Opacity"),
                              .subLabel = {},
                              .minimum = 0.0,
                              .maximum = 100.0,
                              .decimals = 1,
                              .singleStep = 1.0,
                              .unit = QStringLiteral("%")},
                             body);
    opacitySlider_ = new kit::KSlider(body);
    opacitySlider_->setObjectName(QStringLiteral("opacitySlider"));
    opacitySlider_->setAccessibleName(tr("Opacity"));
    opacitySlider_->setRange(0.0, 100.0);
    opacityKeyframe_ = makeKeyframeDiamond(session_, document::kOpacityParameterRole, body);
    // The slider and the cell are BOTH direct children of the row, not a nested group: every
    // projection assertion in the suite reaches an unpaired row through exactly one parentWidget()
    // hop from its value cell, and wrapping the pair would have moved that row a level away.
    addRow(rows, body, makeRowLabel(tr("Opacity"), body), opacityKeyframe_,
           {opacitySlider_, opacity_});
}

void PropertiesEditor::buildTransformSection(QVBoxLayout* layout) {
    auto* section = properties::addSection(layout, selectionSection_, QStringLiteral("transform"),
                                           tr("Transform"));
    adoptSection(section, {document::kPositionParameterRole, document::kRotationParameterRole,
                           document::kScaleParameterRole, document::kAnchorParameterRole});
    auto* body = section->body();
    auto* rows = section->bodyLayout();

    const ValueCellSpec pixelCell{.objectName = {},
                                  .accessibleName = {},
                                  .subLabel = {},
                                  .minimum = -kPracticallyUnbounded,
                                  .maximum = kPracticallyUnbounded,
                                  .decimals = 2,
                                  .singleStep = 1.0,
                                  .unit = QStringLiteral("px")};

    auto positionSpec = pixelCell;
    positionSpec.objectName = QStringLiteral("positionXEditor");
    positionSpec.accessibleName = tr("Position X");
    positionSpec.subLabel = QStringLiteral("X");
    positionX_ = makeValueCell(positionSpec, body);
    positionSpec.objectName = QStringLiteral("positionYEditor");
    positionSpec.accessibleName = tr("Position Y");
    positionSpec.subLabel = QStringLiteral("Y");
    positionY_ = makeValueCell(positionSpec, body);
    positionLink_ = qobject_cast<kit::KButton*>(properties::makeLinkToggle(
        QStringLiteral("positionLinkToggle"),
        tr("Link X and Y: moving one axis moves the other by the same amount"), body));
    positionKeyframe_ = makeKeyframeDiamond(session_, document::kPositionParameterRole, body);
    addRow(rows, body, makeRowLabel(tr("Position"), body), positionKeyframe_,
           makeCellGroup(
               QStringLiteral("positionFieldGroup"),
               {properties::makeComponentCell(session_, document::kPositionParameterRole,
                                              document::AnimationComponent::X, positionX_, body),
                positionLink_,
                properties::makeComponentCell(session_, document::kPositionParameterRole,
                                              document::AnimationComponent::Y, positionY_, body)},
               body));

    // Rotation is a single degree field with a 1 degree scrub step. Its range is deliberately wider
    // than one turn: the schema accepts any finite angle so a rotation curve can wind, and a field
    // that clamped at 360 would silently refuse an authored 450. The slider beside it spans exactly
    // one turn each way and pins at its ends -- it is the coarse gesture, and the cell stays the
    // authority for a wound value the slider cannot reach.
    rotation_ = makeValueCell({.objectName = QStringLiteral("rotationEditor"),
                               .accessibleName = tr("Rotation"),
                               .subLabel = {},
                               .minimum = -100'000.0,
                               .maximum = 100'000.0,
                               .decimals = 2,
                               .singleStep = 1.0,
                               .unit = QString::fromUtf8("°")},
                              body);
    rotationSlider_ = new kit::KSlider(body);
    rotationSlider_->setObjectName(QStringLiteral("rotationSlider"));
    rotationSlider_->setAccessibleName(tr("Rotation"));
    rotationSlider_->setRange(-360.0, 360.0);
    rotationKeyframe_ = makeKeyframeDiamond(session_, document::kRotationParameterRole, body);
    addRow(rows, body, makeRowLabel(tr("Rotation"), body), rotationKeyframe_,
           {rotationSlider_, rotation_});

    // Scale is authored as a PERCENTAGE and stored as a unitless factor, exactly the way Opacity is
    // authored as a percentage and stored in [0, 1]. Negative percentages are reachable because a
    // negative factor is a legitimate mirror of the axis.
    const ValueCellSpec scaleCell{.objectName = {},
                                  .accessibleName = {},
                                  .subLabel = {},
                                  .minimum = -100'000.0,
                                  .maximum = 100'000.0,
                                  .decimals = 2,
                                  .singleStep = 1.0,
                                  .unit = QStringLiteral("%")};
    auto scaleSpec = scaleCell;
    scaleSpec.objectName = QStringLiteral("scaleXEditor");
    scaleSpec.accessibleName = tr("Scale X");
    scaleSpec.subLabel = QStringLiteral("X");
    scaleX_ = makeValueCell(scaleSpec, body);
    scaleSpec.objectName = QStringLiteral("scaleYEditor");
    scaleSpec.accessibleName = tr("Scale Y");
    scaleSpec.subLabel = QStringLiteral("Y");
    scaleY_ = makeValueCell(scaleSpec, body);
    scaleLink_ = qobject_cast<kit::KButton*>(properties::makeLinkToggle(
        QStringLiteral("scaleLinkToggle"), tr("Constrain proportions"), body));
    // On by default: constraining proportions is what an artist expects of a scale pair, and the
    // toggle is right there to break the link for a deliberate non-uniform scale.
    scaleLink_->setChecked(true);
    scaleKeyframe_ = makeKeyframeDiamond(session_, document::kScaleParameterRole, body);
    addRow(rows, body, makeRowLabel(tr("Scale"), body), scaleKeyframe_,
           makeCellGroup(
               QStringLiteral("scaleFieldGroup"),
               {properties::makeComponentCell(session_, document::kScaleParameterRole,
                                              document::AnimationComponent::X, scaleX_, body),
                scaleLink_,
                properties::makeComponentCell(session_, document::kScaleParameterRole,
                                              document::AnimationComponent::Y, scaleY_, body)},
               body));

    // The anchor is in the same pixel space Position is, so it takes Position's range, decimals,
    // step, and unit verbatim.
    auto anchorSpec = pixelCell;
    anchorSpec.objectName = QStringLiteral("anchorXEditor");
    anchorSpec.accessibleName = tr("Anchor X");
    anchorSpec.subLabel = QStringLiteral("X");
    anchorX_ = makeValueCell(anchorSpec, body);
    anchorSpec.objectName = QStringLiteral("anchorYEditor");
    anchorSpec.accessibleName = tr("Anchor Y");
    anchorSpec.subLabel = QStringLiteral("Y");
    anchorY_ = makeValueCell(anchorSpec, body);
    anchorKeyframe_ = makeKeyframeDiamond(session_, document::kAnchorParameterRole, body);
    addRow(rows, body, makeRowLabel(tr("Anchor"), body), anchorKeyframe_,
           makeCellGroup(
               QStringLiteral("anchorFieldGroup"),
               {properties::makeComponentCell(session_, document::kAnchorParameterRole,
                                              document::AnimationComponent::X, anchorX_, body),
                properties::makeComponentCell(session_, document::kAnchorParameterRole,
                                              document::AnimationComponent::Y, anchorY_, body)},
               body));
    anchorGrid_ = new PropertiesAnchorGrid(session_, body);
    addRow(rows, body, makeRowLabel(tr("Anchor Point"), body), nullptr, anchorGrid_);
}

void PropertiesEditor::buildSolidSection(QVBoxLayout* layout) {
    solidColorPanel_ = new QWidget(selectionSection_);
    solidColorPanel_->setObjectName("solidColorProperties");
    auto* panelLayout = new QVBoxLayout(solidColorPanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(kit::px(kit::Spacing::XS));
    auto* section = properties::addSection(panelLayout, solidColorPanel_, QStringLiteral("solid"),
                                           tr("Solid Source"));
    adoptSection(section, {document::kSolidColorParameterRole});
    auto* body = section->body();
    auto* rows = section->bodyLayout();

    // Task P3 and FORMAL AMENDMENT 1 (2026-09-12): four kit::KValueField cells, one per channel,
    // UNBOUNDED in practice -- they never clip negative or HDR channels, display the exact stored
    // value, and commit exactly what was typed or scrubbed.
    const ValueCellSpec channel{.objectName = {},
                                .accessibleName = {},
                                .subLabel = {},
                                .minimum = -kPracticallyUnbounded,
                                .maximum = kPracticallyUnbounded,
                                .decimals = 3,
                                .singleStep = 0.01,
                                .unit = {}};
    auto spec = channel;
    spec.objectName = QStringLiteral("solidColorRedEditor");
    spec.accessibleName = tr("Solid color red");
    spec.subLabel = QStringLiteral("R");
    solidColorRed_ = makeValueCell(spec, body);
    spec.objectName = QStringLiteral("solidColorGreenEditor");
    spec.accessibleName = tr("Solid color green");
    spec.subLabel = QStringLiteral("G");
    solidColorGreen_ = makeValueCell(spec, body);
    spec.objectName = QStringLiteral("solidColorBlueEditor");
    spec.accessibleName = tr("Solid color blue");
    spec.subLabel = QStringLiteral("B");
    solidColorBlue_ = makeValueCell(spec, body);
    spec.objectName = QStringLiteral("solidColorAlphaEditor");
    spec.accessibleName = tr("Solid color alpha");
    spec.subLabel = QStringLiteral("A");
    solidColorAlpha_ = makeValueCell(spec, body);

    solidColorKeyframe_ = makeKeyframeDiamond(session_, document::kSolidColorParameterRole, body);
    solidColorChip_ = new kit::KColorChip(body);
    solidColorChip_->setObjectName("propertiesSolidColorChip");
    (void)properties::addColorRow(
        session_, document::kSolidColorParameterRole, rows, body, solidColorChip_,
        solidColorKeyframe_, {solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_},
        "propertiesSolidColorExpand", "solidColorFieldGroup");
    connect(solidColorChip_, &kit::KColorChip::colorChanged, this,
            [this](const kit::KColor& color) {
                if (!rebuilding_)
                    (void)session_.setSelectedSolidColor(
                        core::Color4d{static_cast<double>(static_cast<double>(color.red)),
                                      static_cast<double>(static_cast<double>(color.green)),
                                      static_cast<double>(static_cast<double>(color.blue)),
                                      static_cast<double>(static_cast<double>(color.alpha))});
            });

    layout->addWidget(solidColorPanel_);
}

void PropertiesEditor::buildTextSection(QVBoxLayout* layout) {
    textSourcePanel_ = new QWidget(selectionSection_);
    textSourcePanel_->setObjectName("textSourceProperties");
    auto* panelLayout = new QVBoxLayout(textSourcePanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(kit::px(kit::Spacing::XS));
    auto* section = properties::addSection(panelLayout, textSourcePanel_, QStringLiteral("text"),
                                           tr("Text Source"));
    adoptSection(section, {document::kTextParameterRole, document::kTextSizeParameterRole,
                           document::kTextColorParameterRole});
    auto* body = section->body();
    auto* rows = section->bodyLayout();

    // The kit has no string field, so the content cell is a plain QLineEdit styled by the
    // application palette. It commits on editingFinished, not on textChanged: a per-keystroke
    // commit would make typing one word a dozen undo steps and a dozen recompiles.
    textContent_ = new kit::KLineEdit(body);
    textContent_->setObjectName("textContentEditor");
    textContent_->setAccessibleName(tr("Text content"));
    textContent_->setFont(kit::font(kit::TypeRole::Value));
    textContent_->setFixedHeight(kit::px(kit::Size::ControlCompact));
    textContent_->setClearButtonEnabled(false);
    addRow(rows, body, makeRowLabel(tr("Text"), body), nullptr, textContent_);

    // The range is the text size schema's own domain, not a spelled UI guess: the document refuses
    // anything outside it, so a cell that could scrub past it would only produce refusals.
    textSize_ = makeValueCell({.objectName = QStringLiteral("textSizeEditor"),
                               .accessibleName = tr("Text size"),
                               .subLabel = {},
                               .minimum = 1.0,
                               .maximum = document::kMaximumTextSizePixels,
                               .decimals = 1,
                               .singleStep = 1.0,
                               .unit = QStringLiteral("px")},
                              body);
    textSize_->setStepper(true);
    textSizeKeyframe_ = makeKeyframeDiamond(session_, document::kTextSizeParameterRole, body);
    addRow(rows, body, makeRowLabel(tr("Font Size"), body), textSizeKeyframe_, textSize_);

    textColor_ = new kit::KColorChip(body);
    textColor_->setObjectName("textColorChip");
    textColor_->setAccessibleName(tr("Text color"));
    textColorKeyframe_ = makeKeyframeDiamond(session_, document::kTextColorParameterRole, body);
    for (std::size_t index = 0; index < textColorFields_.size(); ++index) {
        const auto channel = QString("RGBA").mid(static_cast<qsizetype>(index), 1);
        textColorFields_[index] = makeValueCell({.objectName = "propertiesTextColor" + channel,
                                                 .accessibleName = tr("Text color %1").arg(channel),
                                                 .subLabel = channel,
                                                 .singleStep = 0.01,
                                                 .unit = {}},
                                                body);
        bindCell(textColorFields_[index], [this] {
            if (const auto value =
                    properties::colorFromFields(session_, document::kTextColorParameterSchemaKey,
                                                {textColorFields_[0], textColorFields_[1],
                                                 textColorFields_[2], textColorFields_[3]}))
                (void)session_.setSelectedTextColor(*value);
        });
    }
    (void)properties::addColorRow(
        session_, document::kTextColorParameterRole, rows, body, textColor_, textColorKeyframe_,
        {textColorFields_[0], textColorFields_[1], textColorFields_[2], textColorFields_[3]},
        "propertiesTextColorExpand", "propertiesTextColorFields");

    textFontName_ = new kit::KDropdown(body);
    textFontName_->setObjectName("textFontName");
    textFontName_->setAccessibleName(tr("Text font"));
    textFontName_->addItem(tr("DejaVu Sans"));
    textFontName_->setEnabled(false);
    textFontName_->setToolTip(tr("The embedded DejaVu Sans face is the only supported font"));
    auto* fontRow = addRow(rows, body, makeRowLabel(tr("Font"), body), nullptr, textFontName_);
    rows->removeWidget(fontRow);
    rows->insertWidget(1, fontRow);

    layout->addWidget(textSourcePanel_);
    auto* multilineRow = new QWidget(body);
    multilineRow->setProperty("disclosureFor", tr("Text"));
    multilineRow->setProperty("expanded", false);
    auto* multilineLayout = new QVBoxLayout(multilineRow);
    multilineLayout->setContentsMargins(0, 0, 0, 0);
    auto* multiline = new QPlainTextEdit(multilineRow);
    multiline->setObjectName("propertiesTextMultiline");
    multiline->setFixedHeight(kit::px(kit::Size::MultilineHeight));
    multiline->installEventFilter(this);
    auto* expand = new kit::KButton(body);
    expand->setObjectName("propertiesTextExpand");
    expand->setIconId(kit::IconId::CaretRight);
    expand->setVariant(kit::KButton::Variant::Ghost);
    expand->setFixedSize(kit::px(kit::Size::ControlCompact), kit::px(kit::Size::ControlCompact));
    expand->setToolTip(tr("Edit multiple lines"));
    expand->setCheckable(true);
    auto* textRowLayout = qobject_cast<QHBoxLayout*>(textContent_->parentWidget()->layout());
    textRowLayout->insertWidget(textRowLayout->count() - 1, expand);
    multilineLayout->addWidget(multiline);
    rows->addWidget(multilineRow);
    multilineRow->hide();
    connect(expand, &kit::KButton::toggled, multilineRow, [multilineRow, expand](bool on) {
        multilineRow->setProperty("expanded", on);
        multilineRow->setVisible(on);
        expand->setIconId(on ? kit::IconId::CaretDown : kit::IconId::CaretRight);
    });
    connect(&session_, &CompositionSession::snapshotChanged, multiline, [this, multiline] {
        if (!multiline->hasFocus())
            multiline->setPlainText(textContent_->text());
    });
    connect(expand, &kit::KButton::toggled, multiline, [this, multiline](bool expanded) {
        if (expanded) {
            const auto* parameter = session_.parameterForSelection(document::kTextParameterRole);
            multiline->setProperty("parameterId", QVariant::fromValue(static_cast<qulonglong>(
                                                      parameter ? parameter->id.value() : 0)));
            multiline->setEnabled(parameter && textContent_->isEnabled());
            multiline->setPlainText(textContent_->text());
        }
    });
}

void PropertiesEditor::buildDocumentSection(QVBoxLayout* layout) {
    documentSection_ = new QWidget(this);
    documentSection_->setObjectName(QStringLiteral("propertiesDocumentSection"));
    auto* panelLayout = new QVBoxLayout(documentSection_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(kit::px(kit::Spacing::XS));
    auto* section = properties::addSection(panelLayout, documentSection_,
                                           QStringLiteral("composition"), tr("Composition"));
    // Every row here is read-only composition truth, so there is nothing for Reset to write.
    section->setResetEnabled(false);
    adoptSection(section, {});
    auto* body = section->body();
    auto* rows = section->bodyLayout();

    const auto addReadOnly = [&](QLabel*& target, const QString& objectName,
                                 const QString& accessible, const QString& label,
                                 const kit::TypeRole role) {
        target = makeReadOnlyValueLabel(role, body);
        target->setObjectName(objectName);
        target->setAccessibleName(accessible);
        addRow(rows, body, makeRowLabel(label, body), nullptr, target);
    };
    addReadOnly(documentName_, QStringLiteral("documentName"), tr("Composition name"), tr("Name"),
                kit::TypeRole::Ui);
    addReadOnly(documentFormat_, QStringLiteral("documentFormat"), tr("Composition format"),
                tr("Format"), kit::TypeRole::Value);
    addReadOnly(documentFrameRate_, QStringLiteral("documentFrameRate"),
                tr("Composition frame rate"), tr("Frame Rate"), kit::TypeRole::Value);
    addReadOnly(documentDuration_, QStringLiteral("documentDuration"), tr("Composition duration"),
                tr("Duration"), kit::TypeRole::Value);
    addReadOnly(documentPixelAspect_, QStringLiteral("documentPixelAspect"),
                tr("Composition pixel aspect ratio"), tr("Pixel Aspect"), kit::TypeRole::Value);

    documentBackground_ = new kit::KColorChip(body);
    documentBackground_->setObjectName(QStringLiteral("compositionBackgroundColor"));
    documentBackground_->setAccessibleName(tr("Background Colour"));
    addRow(rows, body, makeRowLabel(tr("Background Colour"), body), nullptr, documentBackground_);
    connect(documentBackground_, &kit::KColorChip::colorChanged, this,
            [this](const kit::KColor& color) {
                commands::Transaction transaction("Set Composition Background",
                                                  session_.snapshot().revision());
                transaction.emplace<commands::SetCompositionBackgroundColor>(
                    session_.compositionId(),
                    core::Color4d{static_cast<double>(color.red), static_cast<double>(color.green),
                                  static_cast<double>(color.blue),
                                  static_cast<double>(color.alpha)});
                static_cast<void>(session_.executeTransaction(std::move(transaction)));
            });
    // Color settings (process space + config name) are read from ProjectSession, not from anything
    // CompositionSession exposes -- document::Composition/Snapshot carry no ColorSettings at all.
    // Per issue #120 decision 3 ("if a listed fact is not reachable via existing read-only API,
    // omit it and report rather than adding API"), the color settings summary row is omitted here.
    panelLayout->addStretch(1);
    layout->addWidget(documentSection_);
}

void PropertiesEditor::bindCommits() {
    // Every numeric row is bound through bindCell(), never straight to valueChanged: a cell emits
    // that for every pixel of a scrub, and ADR 0017 is explicit that a drag does not mutate the
    // document on pointer motion and that one completed gesture is one undo step.
    //
    // A linked paired row reads the CURRENT document value before it writes rather than caching one
    // at build time: the other axis follows by the same delta (Position) or by the stored ratio
    // (Scale), and both answers are only correct against the value the document actually holds now.
    const auto commitPosition = [this](const bool fromX) {
        double x = positionX_->value();
        double y = positionY_->value();
        if (positionLink_->isChecked()) {
            if (const auto current =
                    session_.effectiveVec2Value(document::kPositionParameterRole)) {
                if (fromX) {
                    y = current->y + (x - current->x);
                } else {
                    x = current->x + (y - current->y);
                }
            }
        }
        (void)session_.setSelectedPosition(x, y);
    };
    bindCell(positionX_, [commitPosition] { commitPosition(true); });
    bindCell(positionY_, [commitPosition] { commitPosition(false); });

    const auto commitAnchor = [this] {
        (void)session_.setSelectedAnchor(anchorX_->value(), anchorY_->value());
    };
    bindCell(anchorX_, commitAnchor);
    bindCell(anchorY_, commitAnchor);

    const auto commitScale = [this](const bool fromX) {
        double x = scaleX_->value() / 100.0;
        double y = scaleY_->value() / 100.0;
        if (scaleLink_->isChecked()) {
            if (const auto current = session_.effectiveVec2Value(document::kScaleParameterRole)) {
                // A zero axis carries no ratio to preserve, so proportional linking falls back to
                // mirroring the edited axis rather than dividing by zero.
                if (fromX) {
                    y = current->x == 0.0 ? x : x * (current->y / current->x);
                } else {
                    x = current->y == 0.0 ? y : y * (current->x / current->y);
                }
            }
        }
        (void)session_.setSelectedScale(x, y);
    };
    bindCell(scaleX_, [commitScale] { commitScale(true); });
    bindCell(scaleY_, [commitScale] { commitScale(false); });

    bindCell(rotation_, [this] { commitRotationFromControls(); });
    bindCell(opacity_, [this] { commitOpacityFromControls(); });

    const auto bindSlider = [this](kit::KSlider* slider, kit::KValueField* mirror, auto commit) {
        slider->installEventFilter(this);
        connect(slider, &kit::KSlider::valueChanged, this, [this, slider, mirror, commit] {
            if (rebuilding_) {
                return;
            }
            const QSignalBlocker blocker(mirror);
            mirror->setValue(slider->value());
            if (!slider->isDragging()) {
                commit();
            }
        });
    };
    bindSlider(opacitySlider_, opacity_, [this] { commitOpacityFromControls(); });
    bindSlider(rotationSlider_, rotation_, [this] { commitRotationFromControls(); });

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

    // The Object switches author the layer boundary through the same commands the timeline's toggle
    // strip uses, so the two surfaces cannot drift into two different write paths.
    const auto toggleLayerFlag = [this](const int which, const bool on) {
        const auto layerId = contextualLayerId(session_);
        if (rebuilding_ || !layerId.has_value() || session_.composition() == nullptr) {
            return;
        }
        commands::Transaction transaction("Toggle Layer", session_.snapshot().revision());
        if (which == 0) {
            transaction.emplace<commands::SetLayerEnabled>(session_.compositionId(), *layerId, on);
        } else if (which == 1) {
            transaction.emplace<commands::SetLayerSolo>(session_.compositionId(), *layerId, on);
        } else {
            transaction.emplace<commands::SetLayerLocked>(session_.compositionId(), *layerId, on);
        }
        (void)session_.executeTransaction(std::move(transaction));
    };
    connect(layerVisible_, &kit::KSwitch::toggled, this,
            [toggleLayerFlag](const bool on) { toggleLayerFlag(0, on); });
    connect(layerSolo_, &kit::KSwitch::toggled, this,
            [toggleLayerFlag](const bool on) { toggleLayerFlag(1, on); });
    connect(layerLocked_, &kit::KSwitch::toggled, this,
            [toggleLayerFlag](const bool on) { toggleLayerFlag(2, on); });

    // Task P3: exactly Position's own commit shape (read every cell in the group, write the whole
    // value through one session call) -- one SetSolidColor command per emitted valueChanged.
    const auto commitSolidColor = [this] {
        if (const auto value = properties::colorFromFields(
                session_, document::kSolidColorParameterSchemaKey,
                {solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_}))
            (void)session_.setSelectedSolidColor(*value);
    };
    bindCell(solidColorRed_, commitSolidColor);
    bindCell(solidColorGreen_, commitSolidColor);
    bindCell(solidColorBlue_, commitSolidColor);
    bindCell(solidColorAlpha_, commitSolidColor);

    connect(textContent_, &QLineEdit::editingFinished, this, [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedTextContent(textContent_->text());
        }
    });
    bindCell(textSize_, [this] { (void)session_.setSelectedTextSize(textSize_->value()); });
    connect(textColor_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
        if (!rebuilding_) {
            (void)session_.setSelectedTextColor(
                core::Color4d{static_cast<double>(static_cast<double>(color.red)),
                              static_cast<double>(static_cast<double>(color.green)),
                              static_cast<double>(static_cast<double>(color.blue)),
                              static_cast<double>(static_cast<double>(color.alpha))});
        }
    });
}

} // namespace bloom::ui
