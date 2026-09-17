#include "node_editor_items.hpp"
#include "properties_value_edits.hpp"
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <memory>

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
#include <bloom/document/data_block.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/render/embedded_fonts.hpp>

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
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

void PropertiesEditor::buildFilterStrip() {
    filterStrip_ = new kit::KToolColumn(this);
    filterStrip_->setObjectName(QStringLiteral("propertiesFilterStrip"));
    filterStrip_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    const auto addFilter = [this](const kit::IconId icon, const QString& group,
                                  const QString& label, const QString& objectName) {
        auto* toggle = filterStrip_->addTool(icon, label, objectName);
        toggle->setProperty("propertiesFilterGroup", group);
        connect(toggle, &kit::KIconToggle::toggled, this,
                [this, toggle, group](const bool checked) {
                    if (checked && toggle->isEnabled())
                        selectFilter(group, true);
                });
        return toggle;
    };

    filterToggles_ = {
        addFilter(kit::IconId::Stack, QStringLiteral("all"), tr("Show all property sections"),
                  QStringLiteral("propertiesFilterAll")),
        addFilter(kit::IconId::Composition, QStringLiteral("object"),
                  tr("Show Object and Composition sections"),
                  QStringLiteral("propertiesFilterObject")),
        addFilter(kit::IconId::SlidersHorizontal, QStringLiteral("transform"),
                  tr("Show Transform sections"), QStringLiteral("propertiesFilterTransform")),
        addFilter(kit::IconId::Image, QStringLiteral("source"), tr("Show Source sections"),
                  QStringLiteral("propertiesFilterSource")),
        addFilter(kit::IconId::Graph, QStringLiteral("graph"),
                  tr("Show Merge inputs and upstream sections"),
                  QStringLiteral("propertiesFilterGraph")),
    };

    const auto stored = QSettings()
                            .value(QStringLiteral("properties/filter"), QStringLiteral("all"))
                            .toString()
                            .toLower();
    const auto valid = stored == QStringLiteral("all") || stored == QStringLiteral("object") ||
                       stored == QStringLiteral("transform") ||
                       stored == QStringLiteral("source") || stored == QStringLiteral("graph");
    filterGroup_ = valid ? stored : QStringLiteral("all");
    for (auto* toggle : filterToggles_) {
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(toggle->property("propertiesFilterGroup").toString() == filterGroup_);
    }
}

bool PropertiesEditor::filterGroupAvailable(const QString& group) const {
    if (group == QStringLiteral("all"))
        return session_.composition() != nullptr;
    if (group == QStringLiteral("object"))
        return session_.composition() != nullptr;
    if (group == QStringLiteral("transform"))
        return !std::holds_alternative<std::monostate>(session_.selection().primary);
    if (group == QStringLiteral("graph"))
        return (mergeInputsPanel_ != nullptr && !mergeInputsPanel_->isHidden()) ||
               !upstreamSignature_.isEmpty();
    if (group != QStringLiteral("source") ||
        std::holds_alternative<std::monostate>(session_.selection().primary))
        return false;

    const auto* node = session_.selectedNode();
    if (const auto* layer = std::get_if<document::LayerId>(&session_.selection().primary)) {
        const auto sourceId = session_.directSourceNodeForLayer(*layer);
        node = sourceId && session_.composition()
                   ? session_.composition()->graph().findNode(*sourceId)
                   : nullptr;
    }
    if (node != nullptr &&
        (node->typeId == document::kSolidSourceNodeType ||
         node->typeId == document::kTextSourceNodeType ||
         node->typeId == document::kShapeSourceNodeType ||
         node->typeId == document::kAudioSourceNodeType ||
         (node->typeId == "bloom.image-source" || node->typeId == "bloom.video-source")))
        return true;
    return !registryRows_.empty();
}

void PropertiesEditor::selectFilter(const QString& group, const bool persist) {
    if (group != QStringLiteral("all") && !filterGroupAvailable(group))
        return;
    filterGroup_ = group;
    for (auto* toggle : filterToggles_) {
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(toggle->property("propertiesFilterGroup").toString() == group);
    }
    if (persist)
        QSettings().setValue(QStringLiteral("properties/filter"), filterGroup_);
    filterRows();
}

void PropertiesEditor::updateFilterAvailability() {
    for (auto* toggle : filterToggles_) {
        const auto group = toggle->property("propertiesFilterGroup").toString();
        toggle->setEnabled(group == QStringLiteral("all") || filterGroupAvailable(group));
    }
    if (filterGroup_ != QStringLiteral("all") && !filterGroupAvailable(filterGroup_))
        selectFilter(QStringLiteral("all"), true);
}

bool PropertiesEditor::sectionMatchesFilter(const kit::KSection* section) const {
    if (std::holds_alternative<document::DataBlockRecordId>(session_.selection().primary))
        return filterGroup_ == QStringLiteral("all") ||
               section->property("propertiesSectionGroup").toString() == QStringLiteral("data");
    return filterGroup_ == QStringLiteral("all") ||
           section->property("propertiesSectionGroup").toString() == filterGroup_;
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
    search_ = new kit::KSearchField(this);
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
    chrome_.header.addWidget(search_);
    chrome_.header.objectName = "propertiesHeaderControls";
    (void)EditorArea::buildChromeRow(chrome_.header, this);
    connect(search_, &QLineEdit::textChanged, this, &PropertiesEditor::filterRows);

    buildFilterStrip();
    chrome_.leading = filterStrip_;
    chrome_.leadingWidth = kit::px(kit::Size::ToolColumnWidth);
    chrome_.leadingName = QStringLiteral("propertiesFilterStrip");

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("propertiesScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    body->setObjectName("propertiesScrollBody");
    // The fixed filter strip consumes part of the existing panel minimum. Let the scroll body
    // follow the remaining viewport instead of preserving the old section-content hint as a new
    // horizontal minimum; property rows already elide and shrink their value cells at this width.
    body->setMinimumWidth(0);
    body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(kit::px(kit::Spacing::S));
    scroll->setWidget(body);
    auto* bodyChrome = new QWidget(this);
    bodyChrome->setObjectName(QStringLiteral("propertiesBodyChrome"));
    auto* bodyChromeLayout = new QHBoxLayout(bodyChrome);
    bodyChromeLayout->setContentsMargins(0, 0, 0, 0);
    bodyChromeLayout->setSpacing(0);
    bodyChromeLayout->addWidget(filterStrip_);
    bodyChromeLayout->addWidget(scroll, 1);
    layout->addWidget(bodyChrome, 1);

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
    buildDataBlockSection(bodyLayout);
    bindCommits();

    connect(&session_, &CompositionSession::snapshotChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::liveValueChanged, this,
            &PropertiesEditor::refreshLiveValues);
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
    const bool textField =
        watched == textContent_ || watched->objectName() == "propertiesTextMultiline";
    if (textField && !rebuilding_ && watched->property("ownsTextEdit").toBool()) {
        const auto* parameter = session_.parameterForSelection(document::kTextParameterRole);
        if (parameter && session_.isValueEditing(parameter->id)) {
            if (event->type() == QEvent::KeyPress) {
                const auto* key = static_cast<QKeyEvent*>(event);
                if (key->key() == Qt::Key_Escape) {
                    watched->setProperty("ownsTextEdit", false);
                    const auto parameterId = parameter->id;
                    session_.cancelValueEdit();
                    if (auto* multiline = qobject_cast<QPlainTextEdit*>(watched)) {
                        const QSignalBlocker blocker(multiline);
                        multiline->setPlainText(
                            session_.effectiveStringValue(parameterId).value_or(QString{}));
                    }
                    return true;
                }
                if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                    (watched == textContent_ || key->modifiers().testFlag(Qt::ControlModifier) ||
                     key->modifiers().testFlag(Qt::MetaModifier))) {
                    watched->setProperty("ownsTextEdit", false);
                    (void)session_.commitValueEdit();
                    return true;
                }
            }
            if (event->type() == QEvent::FocusOut) {
                watched->setProperty("ownsTextEdit", false);
                (void)session_.commitValueEdit();
            }
        }
    }
    if (!rebuilding_ && (watched == opacitySlider_ || watched == rotationSlider_)) {
        const auto role = watched == opacitySlider_ ? document::kOpacityParameterRole
                                                    : document::kRotationParameterRole;
        const auto* parameter = session_.parameterForSelection(role);
        if (parameter && event->type() == QEvent::MouseButtonPress)
            (void)session_.beginValueEdit(parameter->id);
        if (parameter && session_.isValueEditing(parameter->id)) {
            if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::FocusOut)
                (void)session_.commitValueEdit();
            else if (event->type() == QEvent::KeyPress &&
                     static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                session_.cancelValueEdit();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PropertiesEditor::refreshLiveValues() {
    if (rebuilding_)
        return;
    rebuilding_ = true;
    configurePosition();
    configureAnchor();
    configureScale();
    configureRotation();
    configureOpacity();
    configureSolidColor();
    configureTextSource();
    configureRegistryRows();
    configureUpstream();
    rebuilding_ = false;
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
    configureDataBlockProperties();
    configureMergeInputs();
    configureRegistryRows();
    configureUpstream();
    configureDrivenRows();
    updateFilterAvailability();
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
        auto* name = new kit::KLabel(node_editor::nodeDisplayName(*composition, *source), row);
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
        properties::bindValueEdit(session_, *opacity, [opacityId] { return opacityId; });
        connect(&session_, &CompositionSession::liveValueChanged, opacity,
                [this, opacity, opacityId] {
                    const QSignalBlocker blocker(opacity);
                    if (const auto value = session_.effectiveScalarValue(opacityId))
                        opacity->setValue(*value * 100);
                });
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
    const bool hasDataBlock =
        std::holds_alternative<document::DataBlockRecordId>(session_.selection().primary);
    const bool showDocument = composition != nullptr && !hasSelection && !hasDataBlock;
    documentSection_->setVisible(showDocument);
    selectionSection_->setVisible(!showDocument && !hasDataBlock);
    if (!showDocument) {
        return;
    }

    const auto background = composition->backgroundColor();
    const QSignalBlocker blockBackground(documentBackground_);
    documentBackground_->setColor(
        {static_cast<float>(background.red), static_cast<float>(background.green),
         static_cast<float>(background.blue), static_cast<float>(background.alpha)});
    documentName_->setText(QString::fromStdString(composition->name()));
    const auto format = composition->format();
    documentFormat_->setText(formatCompositionFormat(format));
    documentFrameRate_->setText(formatFrameRate(format.frameRate()));
    documentPixelAspect_->setText(formatPixelAspect(format.pixelAspect()));
    {
        const QSignalBlocker blockWorkingSpace(documentWorkingColorSpace_);
        documentWorkingColorSpace_->clearItems();
        documentWorkingColorSpace_->addItem(tr("Inherit"), QString{});
        const auto& settings = session_.colorSettings();
        const bool aces =
            std::holds_alternative<document::BuiltInOcioConfigLocator>(
                settings.ocioConfig.locator) &&
            std::get<document::BuiltInOcioConfigLocator>(settings.ocioConfig.locator).uri ==
                color::kAcesCgV1ConfigUri;
        const std::array<std::string_view, 5> spaces =
            aces ? std::array<std::string_view, 5>{"ACEScg", "ACES2065-1", "Linear Rec.709 (sRGB)",
                                                   "Linear P3-D65", "Linear Rec.2020"}
                 : std::array<std::string_view, 5>{settings.processColorSpaceId, {}, {}, {}, {}};
        for (const auto id : spaces) {
            if (!id.empty())
                documentWorkingColorSpace_->addItem(
                    QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())),
                    QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())));
        }
        const auto& overrideId = composition->workingColorSpaceId();
        documentWorkingColorSpace_->setCurrentIndex(
            overrideId.has_value()
                ? documentWorkingColorSpace_->findData(QString::fromStdString(*overrideId))
                : 0);
    }
    const auto context = frameContextFor(session_);
    documentDuration_->setText(context.has_value() ? formatDuration(*context)
                                                   : QStringLiteral("—"));
}

void PropertiesEditor::configureDataBlockProperties() {
    const auto* id = std::get_if<document::DataBlockRecordId>(&session_.selection().primary);
    const auto* block = id ? session_.snapshot().project().findDataBlock(*id) : nullptr;
    dataBlockSection_->setVisible(block != nullptr);
    if (block == nullptr)
        return;

    const auto source = std::visit(
        [](const auto& value) -> QString {
            using Source = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Source, document::AssetLocator>) {
                return QStringLiteral("Source · %1").arg(QString::fromStdString(value.path));
            } else {
                return QStringLiteral("Producer · %1 %2")
                    .arg(QString::fromStdString(value.name), QString::fromStdString(value.version));
            }
        },
        block->provenance.source);
    const auto digest = block->provenance.contentDigest.toLowercaseHex();
    QStringList tags;
    for (const auto& tag : block->tags)
        tags.push_back(QString::fromStdString(tag));
    const auto payload =
        tr("%1 bytes · %2")
            .arg(static_cast<qulonglong>(block->payloadBytes()))
            .arg(QString::fromStdString(std::string(document::dataBlockKindName(block->kind))));
    dataBlockKind_->setText(
        QString::fromStdString(std::string(document::dataBlockKindName(block->kind))));
    dataBlockProvenance_->setText(source);
    dataBlockDigest_->setText(QString::fromStdString(std::string(digest.data(), digest.size())));
    dataBlockPayload_->setText(payload);
    dataBlockTags_->setText(tags.isEmpty() ? tr("None") : tags.join(QStringLiteral(", ")));
    dataBlockReaders_->setText(tr("%1 nodes").arg(session_.dataBlockReaders(*id).size()));
}

void PropertiesEditor::filterRows() {
    const auto query = search_->text();
    for (auto* section : sections_) {
        if (!sectionMatchesFilter(section)) {
            section->hide();
            continue;
        }
        bool any = false;
        auto* rows = section->bodyLayout();
        for (int index = 0; index < rows->count(); ++index) {
            auto* row = rows->itemAt(index)->widget();
            if (!row)
                continue;
            const auto disclosure = row->property("disclosureFor").toString();
            if (!disclosure.isEmpty()) {
                auto* owner = qobject_cast<QWidget*>(row->property("colorOwner").value<QObject*>());
                const auto* display =
                    owner ? owner->findChild<QWidget*>("propertiesDrivenDisplay") : nullptr;
                row->setVisible(row->property("expanded").toBool() &&
                                disclosure.contains(query, Qt::CaseInsensitive) &&
                                (!display || display->isHidden()));
                continue;
            }
            if (row->property("unavailableReadout").toBool() ||
                row->property("roleHidden").toBool()) {
                row->hide();
                continue;
            }
            const auto label = row->property("rowLabel").toString();
            if (label.isEmpty())
                continue;
            const bool match = label.contains(query, Qt::CaseInsensitive);
            row->setVisible(match);
            any = any || match;
        }
        section->setVisible(query.isEmpty() || any);
        section->body()->setVisible(!section->isCollapsed() || !query.isEmpty());
    }
    if (auto* more = findChild<QLabel*>("propertiesMoreUpstream"))
        more->setVisible(more->text().contains(query, Qt::CaseInsensitive));
}

} // namespace bloom::ui
