#include "node_editor_items.hpp"
#include <bloom/ui/properties_editor.hpp>

#include "composition_editor_support.hpp"
#include "properties_anchor_grid.hpp"
#include "properties_registry_row.hpp"
#include "properties_sections.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/embedded_fonts.hpp>

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace bloom::ui {
namespace {

using properties::addRow;
using properties::makeKeyframeDiamond;
using properties::makeReadOnlyValueLabel;
using properties::makeRowLabel;

// The layer whose Object flags and blending the panel authors: the selection may be the layer
// itself, its Layer Output node, or one of its parameters, and all three mean the same layer.
std::optional<document::LayerId> contextualLayerId(const CompositionSession& session) {
    if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary)) {
        return *direct;
    }
    return session.selection().contextualLayer;
}

// The node that actually owns `role` for the current selection, found through the parameter the
// selection resolves rather than by guessing at the boundary or the source: a role reaches this
// panel from either, and only the owning node names the definition its default lives in.
const document::NodeRecord* nodeOwningRole(const CompositionSession& session,
                                           const std::string_view role) {
    const auto* composition = session.composition();
    const auto* parameter = session.parameterForSelection(role);
    if (composition == nullptr || parameter == nullptr) {
        return nullptr;
    }
    for (const auto& node : composition->graph().nodes()) {
        for (const auto& binding : node.parameters) {
            if (binding.role == role && binding.parameterId == parameter->id) {
                return &node;
            }
        }
    }
    return nullptr;
}

// A role's REGISTERED default, or nullopt when the selection does not expose it. A per-section
// Reset writes exactly this -- the registry's own declared default -- never a value the panel
// invented for itself.
std::optional<document::ParameterValue> registryDefaultFor(const CompositionSession& session,
                                                           const std::string_view role) {
    const auto* node = nodeOwningRole(session, role);
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto* definition =
        document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
    if (definition == nullptr) {
        return std::nullopt;
    }
    for (const auto& parameter : definition->parameters) {
        if (parameter.role == role) {
            return parameter.defaultValue;
        }
    }
    return std::nullopt;
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

// Duration as frame count + exact seconds (decision 3), reusing formatExactSeconds() -- the SAME
// truncated-rational formatter TimelineEditor::updateTimeReadout() uses for the current-time
// readout. Frame count is maxFrameIndex + 1 (the greatest valid index is 0-based).
QString formatDuration(const TimelineFrameContext& context) {
    return PropertiesEditor::tr("%1 frames · %2")
        .arg(context.maxFrameIndexValue + 1)
        .arg(formatExactSeconds(context.duration));
}

} // namespace

QWidget* PropertiesEditor::takeHeaderMenuWidget() {
    if (headerTaken_)
        return nullptr;
    headerTaken_ = true;
    search_->show();
    return search_;
}

PropertiesEditor::PropertiesEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("propertiesEditor");
    setAccessibleName(tr("Properties editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::S), kit::px(kit::Spacing::S),
                               kit::px(kit::Spacing::S), kit::px(kit::Spacing::S));
    layout->setSpacing(kit::px(kit::Spacing::S));

    setFocusPolicy(Qt::StrongFocus);
    search_ = new QLineEdit(this);
    search_->setObjectName("propertiesSearchField");
    search_->setAccessibleName(tr("Search properties"));
    search_->setPlaceholderText(tr("Search properties…"));
    search_->setClearButtonEnabled(true);
    search_->installEventFilter(this);
    search_->setMaximumWidth(kit::px(kit::Size::PropertiesSearchWidth));
    search_->setMinimumWidth(kit::px(kit::Size::PropertiesFieldMinWidth));
    search_->setFixedHeight(kit::px(kit::Size::ControlCompact));
    auto searchFont = kit::font(kit::TypeRole::UiSmall);
    searchFont.setCapitalization(QFont::MixedCase);
    searchFont.setLetterSpacing(QFont::PercentageSpacing, 100.0);
    search_->setFont(searchFont);
    search_->addAction(kit::icon(kit::IconId::Zoom, kit::IconRole::Chrome),
                       QLineEdit::TrailingPosition);
    search_->hide();
    connect(search_, &QLineEdit::textChanged, this, &PropertiesEditor::filterRows);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("propertiesScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    body->setObjectName("propertiesScrollBody");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(kit::px(kit::Spacing::S));
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);

    // Task P1 (owner review 2026-09-12: "should not show 'Nothing selected' or any other selected
    // layer info") removed the selection title row entirely. With nothing selected the panel shows
    // only the document/composition section; with a selection it shows only the Object/Transform/
    // source-specific sections. Section headers are the only grouping left.
    selectionSection_ = new QWidget(this);
    selectionSection_->setObjectName(QStringLiteral("propertiesSelectionSection"));
    auto* selectionLayout = new QVBoxLayout(selectionSection_);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(kit::px(kit::Spacing::S));

    buildObjectSection(selectionLayout);
    buildTransformSection(selectionLayout);
    buildSolidSection(selectionLayout);
    buildTextSection(selectionLayout);

    mergeInputsPanel_ = new QWidget(selectionSection_);
    mergeInputsPanel_->setObjectName(QStringLiteral("mergeInputsPanel"));
    auto* mergeLayout = new QVBoxLayout(mergeInputsPanel_);
    mergeLayout->setContentsMargins(0, 0, 0, 0);
    mergeLayout->setSpacing(kit::px(kit::Spacing::XS));
    mergeSection_ =
        properties::addSection(mergeLayout, mergeInputsPanel_, QStringLiteral("merge-inputs"),
                               tr("Inputs · topmost first"));
    // Nothing in a merge row has a registry default of its own to restore, so the section offers no
    // Reset rather than a control that would silently do nothing.
    mergeSection_->setResetEnabled(false);
    adoptSection(mergeSection_, {});
    selectionLayout->addWidget(mergeInputsPanel_);
    selectionLayout->addStretch(1);
    bodyLayout->addWidget(selectionSection_);

    buildDocumentSection(bodyLayout);
    bindCommits();

    connect(&session_, &CompositionSession::snapshotChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &PropertiesEditor::configureMergeInputs);
    // Same reason the node canvas follows it: every animated row shows its curve's value at the
    // session time, and its keyframe diamond says whether a key sits there. Neither question has
    // the same answer at two different times, so a playhead move is a rebuild here even though the
    // document did not change.
    connect(&session_, &CompositionSession::currentTimeChanged, this, &PropertiesEditor::rebuild);

    rebuild();
}

void PropertiesEditor::adoptSection(kit::KSection* section,
                                    std::vector<std::string_view> resetRoles) {
    sections_.push_back(section);
    connect(section, &kit::KSection::collapseAllRequested, this,
            [this] { setAllSectionsCollapsed(true); });
    connect(section, &kit::KSection::expandAllRequested, this,
            [this] { setAllSectionsCollapsed(false); });
    connect(section, &kit::KSection::resetRequested, this,
            [this, roles = std::move(resetRoles)] { this->resetRoles(roles); });
}

void PropertiesEditor::setAllSectionsCollapsed(const bool collapsed) {
    for (auto* section : sections_) {
        section->setCollapsed(collapsed);
    }
}

void PropertiesEditor::resetRoles(const std::vector<std::string_view>& roles) {
    // Every write goes through the SAME session setter the row itself uses, so a Reset is one
    // ordinary, undoable authoring command and never a second write path into the document.
    for (const auto role : roles) {
        const auto* parameter = session_.parameterForSelection(role);
        if (parameter && std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
            resetPropertiesParameter(session_, parameter->id);
            continue;
        }
        const auto fallback = registryDefaultFor(session_, role);
        if (!fallback.has_value()) {
            continue;
        }
        const auto* vector = std::get_if<document::Vec2d>(&*fallback);
        const auto* scalar = std::get_if<double>(&*fallback);
        const auto* color = std::get_if<core::Color4d>(&*fallback);
        const auto* text = std::get_if<std::string>(&*fallback);
        if (role == document::kPositionParameterRole && vector != nullptr) {
            (void)session_.setSelectedPosition(vector->x, vector->y);
        } else if (role == document::kAnchorParameterRole && vector != nullptr) {
            (void)session_.setSelectedAnchor(vector->x, vector->y);
        } else if (role == document::kScaleParameterRole && vector != nullptr) {
            (void)session_.setSelectedScale(vector->x, vector->y);
        } else if (role == document::kRotationParameterRole && scalar != nullptr) {
            (void)session_.setSelectedRotation(*scalar);
        } else if (role == document::kOpacityParameterRole && scalar != nullptr) {
            (void)session_.setSelectedOpacity(*scalar);
        } else if (role == document::kSolidColorParameterRole && color != nullptr) {
            (void)session_.setSelectedSolidColor(*color);
        } else if (role == document::kTextColorParameterRole && color != nullptr) {
            (void)session_.setSelectedTextColor(*color);
        } else if (role == document::kTextSizeParameterRole && scalar != nullptr) {
            (void)session_.setSelectedTextSize(*scalar);
        } else if (role == document::kTextParameterRole && text != nullptr) {
            (void)session_.setSelectedTextContent(QString::fromStdString(*text));
        }
    }
}

void PropertiesEditor::commitOpacityFromControls() {
    (void)session_.setSelectedOpacity(opacity_->value() / 100.0);
}

void PropertiesEditor::commitRotationFromControls() {
    (void)session_.setSelectedRotation(rotation_->value());
}

bool PropertiesEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == search_ && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        search_->clear();
        setFocus(Qt::ShortcutFocusReason);
        return true;
    }
    if (event->type() == QEvent::FocusOut && watched->objectName() == "propertiesTextMultiline" &&
        !rebuilding_) {
        if (auto* text = qobject_cast<QPlainTextEdit*>(watched))
            (void)session_.setParameterValue(
                document::ParameterId::fromRaw(text->property("parameterId").toULongLong()),
                text->toPlainText().toStdString(), tr("Set Text"));
    }
    if (event->type() == QEvent::MouseButtonRelease && !rebuilding_) {
        if (watched == opacitySlider_) {
            commitOpacityFromControls();
        } else if (watched == rotationSlider_) {
            commitRotationFromControls();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PropertiesEditor::rebuild() {
    if (rebuilding_)
        return;
    rebuilding_ = true;
    configureObjectToggles();
    configurePosition();
    configureAnchor();
    configureScale();
    configureRotation();
    configureOpacity();
    configureBlendMode();
    configureSolidColor();
    configureTextSource();
    configureDocumentProperties();
    configureMergeInputs();
    configureRegistryRows();
    configureUpstream();
    configureDrivenRows();
    const auto* composition = session_.composition();
    const auto* selected = session_.selectedNode();
    const auto context = session_.selection().contextualLayer;
    const auto* boundary =
        composition && context ? composition->graph().findLayer(*context) : nullptr;
    if (composition &&
        ((selected && composition->nodeLocked(selected->id)) || (boundary && boundary->locked))) {
        for (auto* field :
             {positionX_, positionY_, anchorX_, anchorY_, scaleX_, scaleY_, rotation_, opacity_,
              solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_, textSize_})
            field->setEnabled(false);
        blendMode_->setEnabled(false);
        textContent_->setEnabled(false);
        textColor_->setEnabled(false);
        solidColorChip_->setEnabled(false);
        anchorGrid_->setEnabled(false);
        for (auto* field : textColorFields_)
            field->setEnabled(false);
        opacitySlider_->setEnabled(false);
        rotationSlider_->setEnabled(false);
    }
    rebuilding_ = false;
    filterRows();
}

void PropertiesEditor::configureObjectToggles() {
    const auto layerId = contextualLayerId(session_);
    const auto* composition = session_.composition();
    const auto* boundary =
        composition && layerId ? composition->graph().findLayer(*layerId) : nullptr;
    for (auto* toggle : {layerVisible_, layerSolo_, layerLocked_}) {
        toggle->setEnabled(boundary != nullptr);
    }
    // The Locked switch stays live even on a locked layer: it is the only way back out, and
    // disabling it would make the lock a one-way door from this panel.
    const QSignalBlocker blockVisible(layerVisible_);
    const QSignalBlocker blockSolo(layerSolo_);
    const QSignalBlocker blockLocked(layerLocked_);
    layerVisible_->setChecked(boundary != nullptr && boundary->enabled);
    layerSolo_->setChecked(boundary != nullptr && boundary->solo);
    layerLocked_->setChecked(boundary != nullptr && boundary->locked);
}

void PropertiesEditor::configureMergeInputs() {
    auto* layout = mergeSection_->bodyLayout();
    while (auto* item = layout->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    const auto* composition = session_.composition();
    const auto* node = session_.selectedNode();
    const auto* merge = composition && node ? composition->graph().merge(node->id) : nullptr;
    mergeInputsPanel_->setVisible(merge != nullptr);
    if (!merge)
        return;
    auto* body = mergeSection_->body();
    for (const auto& entry : merge->entries()) {
        const auto edges = composition->graph().edges();
        const auto edge = std::ranges::find_if(edges, [&](const auto& candidate) {
            const auto* input = std::get_if<document::LayerStackInputRef>(&candidate.destination);
            return input && input->stackNodeId == merge->nodeId() && input->slotId == entry.slotId;
        });
        const auto* source =
            edge == edges.end() ? nullptr : composition->graph().findNode(edge->source.nodeId);
        if (!source)
            continue;
        auto* row = new QWidget(body);
        row->setObjectName(QStringLiteral("mergeInputRow"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* name = new QLabel(node_editor::nodeDisplayName(*composition, *source), row);
        name->setTextFormat(Qt::PlainText);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->setProperty("rowLabel", name->text());
        name->setObjectName(QStringLiteral("mergeInputName"));
        rowLayout->addWidget(name, 1);
        auto* blend = new kit::KDropdown(row);
        blend->setObjectName(QStringLiteral("mergeInputBlendMode"));
        for (const auto mode : core::kBlendModes)
            blend->addItem(blendModeDisplayName(mode));
        const auto mode =
            session_.blendModeForLayer(entry.layerId).value_or(core::BlendMode::Normal);
        blend->setCurrentIndex(static_cast<int>(
            std::distance(core::kBlendModes.begin(), std::ranges::find(core::kBlendModes, mode))));
        const auto* boundary = composition->graph().findLayer(entry.layerId);
        blend->setEnabled(boundary && !boundary->locked);
        for (const auto& binding : source->parameters)
            if (binding.role == document::kBlendModeParameterRole)
                blend->setProperty("parameterId", QVariant::fromValue(static_cast<qulonglong>(
                                                      binding.parameterId.value())));
        rowLayout->addWidget(blend);
        const auto layerId = entry.layerId;
        connect(blend, &kit::KDropdown::currentIndexChanged, row, [this, layerId](int index) {
            if (index >= 0 && static_cast<std::size_t>(index) < core::kBlendModes.size())
                (void)session_.setLayerBlendMode(
                    layerId, core::kBlendModes[static_cast<std::size_t>(index)]);
        });
        auto* opacity = new kit::KValueField(row);
        opacity->setObjectName(QStringLiteral("mergeInputOpacity"));
        opacity->setRange(0, 100);
        opacity->setUnit(QStringLiteral("%"));
        document::ParameterId opacityId;
        if (boundary)
            for (const auto& binding : source->parameters)
                if (binding.role == document::kOpacityParameterRole)
                    opacityId = binding.parameterId;
        opacity->setProperty("parameterId",
                             QVariant::fromValue(static_cast<qulonglong>(opacityId.value())));
        const auto value = opacityId.isValid() ? session_.effectiveScalarValue(opacityId)
                                               : std::optional<double>{1.0};
        opacity->setValue(value.value_or(1.0) * 100);
        opacity->setEnabled(boundary && !boundary->locked && value.has_value());
        rowLayout->addWidget(opacity);
        connect(opacity, &kit::KValueField::valueChanged, row, [this, opacityId](double newValue) {
            if (opacityId.isValid())
                (void)session_.setParameterValue(opacityId, newValue / 100.0,
                                                 tr("Set Layer Opacity"));
        });
        layout->addWidget(row);
    }
}

void PropertiesEditor::configurePosition() {
    const auto* position = session_.parameterForSelection(document::kPositionParameterRole);
    // effectiveVec2Value(), not constantVec2Value(): an ANIMATED parameter's row must stay live and
    // show the curve's exactly sampled value at the current time, because editing it there is how
    // AE inserts a key. A driven source still yields nullopt and still disables the field.
    const auto positionValue = session_.effectiveVec2Value(document::kPositionParameterRole);
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
    positionKeyframe_->refresh();
}

void PropertiesEditor::configureAnchor() {
    anchorGrid_->refresh();
    const auto* anchor = session_.parameterForSelection(document::kAnchorParameterRole);
    const auto value = session_.effectiveVec2Value(document::kAnchorParameterRole);
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
    anchorKeyframe_->refresh();
}

void PropertiesEditor::configureScale() {
    const auto* scale = session_.parameterForSelection(document::kScaleParameterRole);
    const auto value = session_.effectiveVec2Value(document::kScaleParameterRole);
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
    scaleKeyframe_->refresh();
}

void PropertiesEditor::configureRotation() {
    const auto* parameter = session_.parameterForSelection(document::kRotationParameterRole);
    const auto value = session_.effectiveScalarValue(document::kRotationParameterRole);
    rotation_->setEnabled(value.has_value());
    rotationSlider_->setEnabled(value.has_value());
    const QSignalBlocker blocker(rotation_);
    const QSignalBlocker sliderBlocker(rotationSlider_);
    rotation_->setValue(value.value_or(0.0));
    rotationSlider_->setValue(value.value_or(0.0));
    rotation_->setToolTip(parameter == nullptr ? tr("Rotation is not exposed by this selection")
                                               : parameterSourceDescription(*parameter));
    rotationKeyframe_->refresh();
}

void PropertiesEditor::configureOpacity() {
    const auto* parameter = session_.parameterForSelection(document::kOpacityParameterRole);
    const auto value = session_.effectiveScalarValue(document::kOpacityParameterRole);
    opacity_->setEnabled(value.has_value());
    opacitySlider_->setEnabled(value.has_value());
    const QSignalBlocker blocker(opacity_);
    const QSignalBlocker sliderBlocker(opacitySlider_);
    opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
    opacitySlider_->setValue(value.has_value() ? *value * 100.0 : 100.0);
    opacity_->setToolTip(parameter == nullptr ? tr("Opacity is not exposed by this selection")
                                              : parameterSourceDescription(*parameter));
    opacityKeyframe_->refresh();
}

void PropertiesEditor::configureBlendMode() {
    const auto layerId = contextualLayerId(session_);
    const auto mode = layerId.has_value() ? session_.blendModeForLayer(*layerId) : std::nullopt;
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
