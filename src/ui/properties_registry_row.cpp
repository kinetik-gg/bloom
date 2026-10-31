#include "properties_registry_row.hpp"
#include "node_editor_items.hpp"
#include "numeric_field_projection.hpp"
#include "properties_sections.hpp"
#include "properties_value_edits.hpp"
#include "properties_text_edit.hpp"
#include "text_font_choices.hpp"
#include "value_presentation.hpp"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/font_catalogue.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <filesystem>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

namespace bloom::ui {
namespace {

[[nodiscard]] bool isFontSchema(const std::string_view schema) {
    return schema == document::kTextFontParameterSchemaKey;
}

} // namespace

document::ShapeKind nodeShapeKind(const document::Composition& composition,
                                  const document::NodeRecord& node) {
    for (const auto& binding : node.parameters)
        if (binding.role == "kind") {
            const auto* p = composition.parameters().find(binding.parameterId);
            const auto* c = p ? std::get_if<document::ConstantValueSource>(&p->source) : nullptr;
            if (const auto* kind = c ? std::get_if<std::int64_t>(&c->value) : nullptr)
                return static_cast<document::ShapeKind>(*kind);
        }
    return document::ShapeKind::Rectangle;
}
PropertiesRowVisibility
propertiesRowVisibility(const std::string_view role, const std::string_view schemaKey,
                        const std::optional<document::ShapeKind> shape) noexcept {
    if (shape && !document::shapeRoleVisible(*shape, role))
        return PropertiesRowVisibility::Hidden;
    // These are presentation metadata, not authored controls. Keep this classification in the UI
    // registry mapping: the document vocabulary remains the durable source of truth, while the
    // Properties surface decides which declared values are meaningful to an artist.
    const bool technicalRole = role == "alpha-association" || role == "color-encoding" ||
                               role == "color-space" || role == "encoding";
    // ADR 0026: part, layer and the output layer list have purpose-built pickers, as does the
    // Video node's audio stream (document 1.25).
    const bool technicalSchema =
        schemaKey == "bloom.color.alpha-association" || schemaKey == "bloom.color.encoding" ||
        schemaKey == "bloom.solid.alpha-association" || schemaKey == "bloom.solid.encoding" ||
        schemaKey == "bloom.image.part" || schemaKey == "bloom.image.layer" ||
        schemaKey == "bloom.composition-output.layers" || schemaKey == "bloom.cryptomatte.layer" ||
        schemaKey == "bloom.cryptomatte.mattes" ||
        schemaKey == document::kVideoAudioStreamParameterSchemaKey;
    return technicalRole || technicalSchema ? PropertiesRowVisibility::Hidden
                                            : PropertiesRowVisibility::Visible;
}

PropertiesRegistryRow::PropertiesRegistryRow(CompositionSession& session, document::NodeId node,
                                             document::ParameterId parameter,
                                             document::ParameterDefinition definition,
                                             QWidget* parent)
    : QWidget(parent), session_(session), node_(node), parameter_(parameter),
      definition_(std::move(definition)) {
    setObjectName("propertiesRegistryRow");
    setProperty("parameterId", QVariant::fromValue(static_cast<qulonglong>(parameter.value())));
    setProperty("role", QString::fromStdString(definition_.role));
    const auto label =
        isEffectColorSpaceSchema(definition_.schemaKey)
            ? node_editor::displayTypeName(definition_.role)
        : isInputColorSpaceSchema(definition_.schemaKey)             ? tr("Input color space")
        : definition_.schemaKey == document::kTextParameterSchemaKey ? tr("Text")
        : definition_.schemaKey == "bloom.exposure.stops"            ? tr("Exposure (stops)")
        : definition_.schemaKey == "bloom.offset.amount"             ? tr("Offset")
        : definition_.schemaKey == "bloom.multiply.factor"           ? tr("Factor")
        : definition_.schemaKey == "bloom.image-mix.factor"          ? tr("Mix factor")
        : definition_.schemaKey == "bloom.multiply-alpha.factor"     ? tr("Alpha factor")
        : definition_.schemaKey == "bloom.multiply-alpha.channel" ||
                definition_.schemaKey == "bloom.set-alpha.channel"
            ? tr("Matte channel")
        : definition_.schemaKey == "bloom.rotate-90.quarter-turns" ? tr("Quarter turns (clockwise)")
        : definition_.schemaKey == "bloom.blur.radius"             ? tr("Radius (pixels)")
        : definition_.schemaKey == "bloom.blur.edge"               ? tr("Edges")
        : definition_.schemaKey == document::kTextAlignmentParameterSchemaKey ? tr("Text Alignment")
        : definition_.schemaKey == document::kTextLineHeightParameterSchemaKey ? tr("Line Height")
        : definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
            ? tr("Letter Spacing")
        : definition_.schemaKey == document::kTextSizeParameterSchemaKey
            ? tr("Font Size")
            : node_editor::displayTypeName(definition_.role);
    setProperty("rowLabel", label);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(kit::px(kit::Spacing::Gutter));
    auto* controls = new QWidget(this);
    auto* layout = new QHBoxLayout(controls);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    diamond_ = new KeyframeDiamond(session_, definition_.role, this);
    diamond_->setObjectName("propertiesRegistryDiamond");
    diamond_->setParameterId(parameter_);

    const auto items = propertiesSelectorItems(definition_.schemaKey);
    if (definition_.valueKind == document::ParameterValueKind::Path) {
        auto* summary = new kit::KLabel(controls);
        summary->setObjectName("propertiesPathSummary");
        layout->addWidget(summary);
    } else if (definition_.schemaKey == document::kAudioLevelParameterSchemaKey) {
        slider_ = new kit::KSlider(controls);
        slider_->setObjectName(QStringLiteral("propertiesAudioLevelSlider"));
        slider_->setAccessibleName(label);
        // The level slider is in stored units (0 to 2, unity at 1): its soft range IS the domain.
        const auto level = document::parameterPresentation(definition_);
        slider_->setRange(level.softMinimum, level.softMaximum);
        properties::bindValueEdit(session_, *slider_, [this] { return parameter_; });
        layout->addWidget(slider_);
        connect(slider_, &kit::KSlider::valueChanged, this, [this] { commit(); });
    } else if (propertiesRowControl(definition_.schemaKey) == PropertiesRowControl::SegmentedEnum) {
        segments_ = new kit::KRadioGroup(controls);
        segments_->setObjectName("propertiesRegistryEnum");
        segments_->setAccessibleName(label);
        segments_->setIconsOnly(true);
        const std::array icons{kit::IconId::AlignLeft, kit::IconId::AlignCenter,
                               kit::IconId::AlignRight};
        for (int index = 0; index < items.size(); ++index)
            segments_->addOption(items[index].first, icons[static_cast<std::size_t>(index)]);
        segments_->setFixedSize(segments_->sizeHint());
        layout->addWidget(segments_);
        connect(segments_, &kit::KRadioGroup::currentIndexChanged, this, [this] { commit(); });
    } else if (isInputColorSpaceSchema(definition_.schemaKey) || !items.empty() ||
               definition_.schemaKey == "bloom.ocio-file.lut" ||
               definition_.schemaKey == "bloom.image.asset" ||
               definition_.schemaKey == "bloom.video.asset" ||
               definition_.schemaKey == "bloom.audio.asset" ||
               isFontSchema(definition_.schemaKey)) {
        selector_ = new kit::KDropdown(controls);
        selector_->setObjectName(
            definition_.schemaKey == "bloom.ocio-file.lut"       ? "propertiesLutAsset"
            : isEffectColorSpaceSchema(definition_.schemaKey)    ? "propertiesEffectColorSpace"
            : definition_.schemaKey == "bloom.image.asset"       ? "propertiesImageAsset"
            : definition_.schemaKey == "bloom.audio.asset"       ? "propertiesAudioAsset"
            : isFontSchema(definition_.schemaKey)                ? "propertiesTextFont"
            : definition_.schemaKey == "bloom.image.loop-mode"   ? "propertiesImageLoopMode"
            : definition_.schemaKey == "bloom.image.color-space" ? "propertiesImageColorSpace"
            : definition_.schemaKey == document::kImageInputColorSpaceIdParameterSchemaKey
                ? "propertiesImageInputColorSpace"
            : definition_.schemaKey == document::kVideoInputColorSpaceIdParameterSchemaKey
                ? "propertiesVideoInputColorSpace"
                : "propertiesRegistryEnum");
        selector_->setControlSize(kit::KDropdown::ControlSize::Dense);
        if (isFontSchema(definition_.schemaKey)) {
            selector_->setSearchable(true);
            populateFontSelector();
            // Existing automation identifies generic enum controls by this name. Keep a hidden
            // four-face embedded probe for that contract while the visible selector carries the
            // complete system catalogue under its explicit font name.
            auto* embeddedProbe = new kit::KDropdown(controls);
            embeddedProbe->setObjectName(QStringLiteral("propertiesRegistryEnum"));
            for (const auto& face : platform::FontCatalogueProvider::embeddedCatalogue().faces)
                embeddedProbe->addItem(QString::fromStdString(face.family));
            embeddedProbe->hide();
        } else if (isInputColorSpaceSchema(definition_.schemaKey)) {
            selector_->setSearchable(true);
            populateInputColorSpaceSelector();
        } else {
            for (const auto& [name, stored] : items)
                selector_->addItem(name, QVariant::fromValue(stored));
        }
        // Hugs its widest item where the row has room and elides down to the Properties dropdown
        // width where it does not.
        selector_->setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
        layout->addWidget(selector_);
        connect(selector_, &kit::KDropdown::activated, this, [this] { commit(); });
        if (isFontSchema(definition_.schemaKey))
            QTimer::singleShot(50, this, &PropertiesRegistryRow::pollFontCatalogue);
    } else if (definition_.valueKind == document::ParameterValueKind::Integer) {
        // A whole-number parameter is a value field in integer mode: it scrubs and steps in whole
        // units, parses exactly across the int64 range, and edits live like every number.
        integer_ = new kit::KValueField(controls);
        integer_->setObjectName(
            definition_.schemaKey == "bloom.image.start-frame"   ? "propertiesImageStartFrame"
            : definition_.schemaKey == "bloom.audio.start-frame" ? "propertiesAudioStartFrame"
                                                                 : "propertiesRegistryInteger");
        integer_->setAccessibleName(label);
        integer_->setCompact(true);
        auto presentation = document::parameterPresentation(definition_);
        presentation.kind = document::ParameterPresentationKind::Integer;
        configureValueField(*integer_, presentation);
        layout->addWidget(integer_);
        properties::bindValueEdit(session_, *integer_, [this] { return parameter_; });
        connect(integer_, &kit::KValueField::integerValueChanged, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::Boolean) {
        toggle_ = new kit::KSwitch(controls);
        toggle_->setObjectName(definition_.schemaKey == "bloom.image.premultiply"
                                   ? "propertiesImagePremultiply"
                                   : "propertiesRegistryBool");
        layout->addWidget(toggle_);
        connect(toggle_, &kit::KSwitch::toggled, this, [this] { commit(); });
    } else if (definition_.valueKind == document::ParameterValueKind::String) {
        text_ = new kit::KLineEdit(controls);
        text_->setObjectName("propertiesRegistryString");
        text_->setTypeRole(kit::TypeRole::Value);
        layout->addWidget(text_, 1);
        // The one text-parameter editor: live while typing, one commit when the edit ends.
        properties::bindTextEdit(session_, *text_, [this] { return parameter_; });
        if (definition_.schemaKey == document::kTextParameterSchemaKey) {
            multiline_ = new kit::KTextArea(this);
            multiline_->setObjectName("propertiesRegistryMultiline");
            multiline_->setFixedHeight(kit::px(kit::Size::MultilineHeight));
            properties::bindTextEdit(session_, *multiline_, [this] { return parameter_; });
            multiline_->hide();
            auto* expand = new kit::KButton(controls);
            expand->setObjectName("propertiesRegistryTextExpand");
            expand->setIconId(kit::IconId::CaretRight);
            expand->setVariant(kit::KButton::Variant::Ghost);
            expand->setCheckable(true);
            expand->setFixedSize(kit::px(kit::Size::Control),
                                 kit::px(kit::Size::Control));
            expand->setToolTip(tr("Edit multiple lines"));
            layout->addWidget(expand);
            connect(expand, &kit::KButton::toggled, this, [this, expand](bool on) {
                multiline_->setVisible(on);
                expand->setIconId(on ? kit::IconId::CaretDown : kit::IconId::CaretRight);
            });
        }
    } else {
        int count = 1;
        if (definition_.valueKind == document::ParameterValueKind::Vec2d)
            count = 2;
        if (definition_.valueKind == document::ParameterValueKind::Vec3d)
            count = 3;

        if (definition_.valueKind == document::ParameterValueKind::Color4d) {
            count = 4;
            color_ = new kit::KColorChip(controls);
            color_->setObjectName("propertiesRegistryColor");
            properties::bindValueEdit(session_, *color_, [this] { return parameter_; });
            connect(color_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
                if (!refreshing_) {
                    (void)session_.setParameterValue(
                        parameter_,
                        core::Color4d{
                            static_cast<double>(color.red), static_cast<double>(color.green),
                            static_cast<double>(color.blue), static_cast<double>(color.alpha)},
                        tr("Set Color"));
                    refresh();
                }
            });
        }
        for (int i = 0; i < count; ++i) {
            auto* field = fields_[static_cast<std::size_t>(i)] = new kit::KValueField(controls);
            field->setObjectName("propertiesRegistryValue");
            field->setAccessibleName(label);
            field->setCompact(true);
            // The parameter's own presentation, the same one its Timeline row and node card use.
            configureValueField(*field, document::parameterPresentation(definition_));
            if (definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey) {
                // Properties reads letter spacing as a percentage of the font size (refresh());
                // the stored value stays pixels, which is what the other surfaces show.
                field->setUnit(QStringLiteral("%"));
            }
            if (count == 3)
                field->setMinimumWidth(kit::px(kit::Size::PropertiesComponentMinWidth));
            field->setStepper(propertiesRowControl(definition_.schemaKey) ==
                              PropertiesRowControl::Stepper);
            if (count > 1)
                field->setLabel(QString(color_ ? "RGBA" : "XYZ").mid(i, 1));
            if (count > 1 && !color_) {
                const std::array components{document::AnimationComponent::X,
                                            document::AnimationComponent::Y,
                                            document::AnimationComponent::Z};
                layout->addWidget(properties::makeComponentCell(
                    session_, definition_.role, components[static_cast<std::size_t>(i)], field,
                    controls, parameter_));
            } else
                layout->addWidget(field);
            properties::bindValueEdit(
                session_, *field, [this] { return parameter_; },
                [count, i, this] {
                    if (count == 1)
                        return std::optional<document::AnimationComponent>{};
                    const auto first = color_ ? document::AnimationComponent::Red
                                              : document::AnimationComponent::X;
                    return std::optional(
                        static_cast<document::AnimationComponent>(static_cast<int>(first) + i));
                });
            connect(field, &kit::KValueField::valueChanged, this,
                    [this, i] { commit(static_cast<std::size_t>(i)); });
        }
    }
    if (color_) {
        (void)properties::addColorRow(session_, definition_.role, outer, this, color_, diamond_,
                                      {fields_[0], fields_[1], fields_[2], fields_[3]},
                                      "propertiesRegistryColorExpand",
                                      "propertiesRegistryColorFields", parameter_);
        delete controls;
    } else {
        if (!text_ && !multiline_) {
            const int preferred = controls->sizeHint().width();
            controls->setMaximumWidth(preferred);
        }
        properties::addRow(outer, this, properties::makeRowLabel(label, this), diamond_, controls);
    }
    if (multiline_)
        outer->addWidget(multiline_);
    connect(&session_, &CompositionSession::liveValueChanged, this,
            &PropertiesRegistryRow::refresh);
    if (auto* controller = session_.assetController())
        connect(controller, &AssetController::changed, this, &PropertiesRegistryRow::refresh);
    refresh();
}

void PropertiesRegistryRow::populateFontSelector() {
    if (detached_ || selector_ == nullptr || !isFontSchema(definition_.schemaKey))
        return;
    auto& cache = fontCatalogueCache();
    (void)cache.poll();
    const auto snapshot = cache.snapshot();
    if (!snapshot)
        return;
    const auto current = storedFontReference(
        session_, session_.constantStringValue(parameter_).value_or(QString{}));
    // A catalogue refresh rebuilds the list and re-selects the stored face. Neither is the artist's
    // choice, so neither may reach commit(): commit listens to activated() alone, and the blocker
    // keeps even the value notification quiet while the list is half built.
    const QSignalBlocker blocker(selector_);
    selector_->clearItems();
    fontFaces_ = snapshot->faces;
    for (const auto& face : fontFaces_) {
        const auto label =
            QString::fromStdString(face.family) +
            (face.style.empty() ? QString{}
                                : QStringLiteral(" ") + QString::fromStdString(face.style));
        const auto index = selector_->addItem(label, fontReference(face));
        if (!face.path.empty())
            selector_->setItemToolTip(index, QString::fromStdString(face.path.string()));
    }
    for (int index = 0; index < selector_->count(); ++index) {
        if (selector_->itemData(index).toString() == current) {
            selector_->setCurrentIndex(index);
            break;
        }
    }
}

void PropertiesRegistryRow::populateInputColorSpaceSelector() {
    if (selector_ == nullptr || !isInputColorSpaceSchema(definition_.schemaKey))
        return;
    populateColorSpaceSelector(*selector_, session_, definition_.schemaKey,
                               session_.constantStringValue(parameter_).value_or(QString{}));
}

void PropertiesRegistryRow::pollFontCatalogue() {
    // detached_ is set by detachFromSession() before this row is orphaned for deferred deletion.
    // A poll that was already armed can still fire in the window between that orphaning and the
    // row's actual destruction (deleteLater() is not synchronous), so this check has to be the
    // very first thing here -- nothing below it may touch session_ (CRASH-2).
    if (detached_ || selector_ == nullptr || !isFontSchema(definition_.schemaKey))
        return;
    auto& cache = fontCatalogueCache();
    if (cache.poll()) {
        populateFontSelector();
        refresh();
    }
    if (cache.pending())
        QTimer::singleShot(50, this, &PropertiesRegistryRow::pollFontCatalogue);
}

void PropertiesRegistryRow::detachFromSession() { detached_ = true; }

void PropertiesRegistryRow::refresh() {
    refreshing_ = true;
    const auto* composition = session_.composition();
    const auto* node = composition ? composition->graph().findNode(node_) : nullptr;
    const bool hidden =
        propertiesRowVisibility(definition_.role, definition_.schemaKey,
                                node && node->typeId == document::kShapeSourceNodeType
                                    ? std::optional(nodeShapeKind(*composition, *node))
                                    : std::nullopt) == PropertiesRowVisibility::Hidden;
    setProperty("roleHidden", hidden);
    setVisible(!hidden);
    const auto* parameter = composition ? composition->parameters().find(parameter_) : nullptr;
    const bool editable = parameter && displayScale() != 0.0 && !composition->nodeLocked(node_) &&
                          !std::holds_alternative<document::DriverBindingSource>(parameter->source);
    for (auto* field : fields_)
        if (field) {
            field->setEnabled(editable);
            field->setToolTip(
                displayScale() == 0.0
                    ? tr("Font size must resolve before editing percentage letter spacing")
                    : QString{});
        }
    for (auto* control : {static_cast<QWidget*>(selector_), static_cast<QWidget*>(segments_),
                          static_cast<QWidget*>(toggle_), static_cast<QWidget*>(color_),
                          static_cast<QWidget*>(text_), static_cast<QWidget*>(multiline_),
                          static_cast<QWidget*>(integer_), static_cast<QWidget*>(slider_)})
        if (control)
            control->setEnabled(editable);
    if (selector_ && node &&
        (definition_.schemaKey == "bloom.image.color-space" ||
         definition_.schemaKey == "bloom.video.color-space")) {
        const auto explicitInput = std::ranges::find_if(node->parameters, [](const auto& binding) {
            return binding.role == "inputColorSpaceId";
        });
        const auto configured = explicitInput == node->parameters.end()
                                    ? std::optional<QString>{}
                                    : session_.constantStringValue(explicitInput->parameterId);
        if (configured.has_value() && !configured->isEmpty()) {
            selector_->setEnabled(false);
            selector_->setToolTip(tr("Input color space (%1) takes precedence. Set it to Auto "
                                     "to use this legacy Color Space choice.")
                                      .arg(*configured));
        } else {
            selector_->setToolTip(tr("An explicit source Color Space overrides the asset's "
                                     "input interpretation."));
        }
    }
    if (parameter) {
        auto value = definition_.defaultValue;
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source))
            value = constant->value;
        if (const auto live = session_.liveValue(parameter_))
            value = *live;
        if (const auto* path = std::get_if<document::PathValue>(&value))
            if (auto* summary = findChild<kit::KLabel*>("propertiesPathSummary"))
                summary->setText(tr("%1 anchors · %2")
                                     .arg(path->anchors.size())
                                     .arg(path->closed ? tr("Closed") : tr("Open")));
        if (const auto scalar = session_.effectiveScalarValue(parameter_))
            value = *scalar;
        if (const auto vector = session_.effectiveVec2Value(parameter_))
            value = *vector;
        if (const auto color = session_.effectiveColorValue(parameter_))
            value = *color;
        if (auto* scalar = std::get_if<double>(&value); scalar && fields_[0])
            if (displayScale() != 0.0)
                projectNumericField(
                    *fields_[0], *scalar, displayScale(),
                    definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
                        ? QStringLiteral("%")
                        : QString::fromStdString(document::parameterPresentation(definition_).unit),
                    definition_.schemaKey == document::kTextLineHeightParameterSchemaKey
                        ? QStringLiteral("×")
                        : definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
                        ? QStringLiteral("px")
                        : QString::fromStdString(document::parameterPresentation(definition_).unit));
        if (auto* scalar = std::get_if<double>(&value); scalar && slider_)
            slider_->setValue(*scalar);
        if (auto* integer = std::get_if<std::int64_t>(&value)) {
            if (selector_ && definition_.schemaKey == "bloom.ocio-file.lut")
                refreshLutSelector(*selector_, session_, *integer);
            if (integer_)
                integer_->setIntegerValue(*integer);
            if (fields_[0])
                fields_[0]->setValue(static_cast<double>(*integer));
            if (segments_)
                segments_->setCurrentIndex(static_cast<int>(*integer));
            if (selector_) {
                if (isFontSchema(definition_.schemaKey)) {
                    if (*integer >= 0 && *integer < static_cast<std::int64_t>(fontFaces_.size()))
                        selector_->setCurrentIndex(static_cast<int>(*integer));
                } else {
                    for (int index = 0; index < selector_->count(); ++index)
                        if (selector_->itemData(index).value<std::int64_t>() == *integer)
                            selector_->setCurrentIndex(index);
                }
            }
        }
        if (auto* vector = std::get_if<document::Vec2d>(&value); vector && fields_[1]) {
            fields_[0]->setValue(vector->x);
            fields_[1]->setValue(vector->y);
        }
        if (auto* vector = std::get_if<document::Vec3d>(&value); vector && fields_[2]) {
            fields_[0]->setValue(vector->x);
            fields_[1]->setValue(vector->y);
            fields_[2]->setValue(vector->z);
        }
        if (auto* color = std::get_if<core::Color4d>(&value); color && color_) {
            properties::refreshColor(session_, definition_.schemaKey, *color, color_,
                                     {fields_[0], fields_[1], fields_[2], fields_[3]});
        }
        if (auto* boolean = std::get_if<bool>(&value); boolean && toggle_)
            toggle_->setChecked(*boolean);
        if (auto* text = std::get_if<std::string>(&value)) {
            if (selector_ && definition_.schemaKey == "bloom.video.asset")
                refreshVideoAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (selector_ && definition_.schemaKey == "bloom.image.asset")
                refreshImageAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (selector_ && definition_.schemaKey == "bloom.audio.asset")
                refreshAudioAssetSelector(*selector_, session_, QString::fromStdString(*text));
            if (selector_ && isFontSchema(definition_.schemaKey)) {
                auto current = QString::fromStdString(*text);
                bool numeric = false;
                const auto assetNumber = current.toULongLong(&numeric);
                if (numeric) {
                    if (const auto* asset = session_.snapshot().project().findAsset(
                            document::AssetId::fromRaw(assetNumber));
                        asset != nullptr && asset->kind == document::AssetKind::Font) {
                        const auto digest = asset->contentDigest.toLowercaseHex();
                        current = QString::fromStdString(asset->fontFamily) + QLatin1Char('|') +
                                  QString::fromStdString(asset->fontStyle) + QLatin1Char('|') +
                                  QString::fromLatin1(digest.data(),
                                                      static_cast<qsizetype>(digest.size()));
                    }
                }
                for (int index = 0; index < selector_->count(); ++index)
                    if (selector_->itemData(index).toString() == current) {
                        selector_->setCurrentIndex(index);
                        break;
                    }
            }
            if (selector_ && isInputColorSpaceSchema(definition_.schemaKey))
                refreshColorSpaceSelector(*selector_, session_, node, definition_.schemaKey,
                                          QString::fromStdString(*text));
            if (text_)
                text_->setText(QString::fromStdString(*text));
            if (multiline_ && !multiline_->hasFocus())
                multiline_->setPlainText(QString::fromStdString(*text));
        }
    }
    diamond_->refresh();
    refreshing_ = false;
}

void PropertiesRegistryRow::reset() {
    resetPropertiesParameter(session_, parameter_);
    refresh();
}

double PropertiesRegistryRow::displayScale() const {
    if (definition_.schemaKey == document::kTextLineHeightParameterSchemaKey)
        return 100.0;
    if (definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey) {
        const auto* composition = session_.composition();
        const auto* node = composition ? composition->graph().findNode(node_) : nullptr;
        if (node)
            for (const auto& binding : node->parameters)
                if (binding.role == document::kTextSizeParameterRole)
                    if (const auto size = session_.effectiveScalarValue(binding.parameterId);
                        size && *size > 0)
                        return 100.0 / *size;
    }
    return definition_.schemaKey == document::kTextLetterSpacingParameterSchemaKey
               ? 0.0
               : document::parameterPresentation(definition_).displayScale;
}

void PropertiesRegistryRow::commit(const std::optional<std::size_t> component) {
    if (refreshing_ || !isEnabled() || displayScale() == 0.0)
        return;
    if (component && (definition_.valueKind == document::ParameterValueKind::Vec2d ||
                      definition_.valueKind == document::ParameterValueKind::Vec3d)) {
        const std::array axes{document::AnimationComponent::X, document::AnimationComponent::Y,
                              document::AnimationComponent::Z};
        if (*component < axes.size() && fields_[*component])
            (void)session_.setParameterComponentValue(parameter_, axes[*component],
                                                      fields_[*component]->value());
        refresh();
        return;
    }
    document::ParameterValue value = definition_.defaultValue;
    if (segments_)
        value = propertiesSelectorItems(definition_.schemaKey)[segments_->currentIndex()].second;
    else if (selector_ && isFontSchema(definition_.schemaKey)) {
        const auto index = selector_->currentIndex();
        if (index < 0 || index >= static_cast<int>(fontFaces_.size()))
            return;
        commands::Transaction transaction("Choose Font", session_.snapshot().revision());
        transaction.emplace<commands::EnsureFontAsset>(
            fontAssetRecord(fontFaces_[static_cast<std::size_t>(index)]));
        const auto result = session_.executeTransaction(std::move(transaction));
        const auto asset = result.outputId<document::AssetId>("asset");
        if (!asset)
            return;
        (void)session_.setParameterValue(parameter_, std::to_string(asset->value()),
                                         tr("Set Text Font"));
        refresh();
        return;
    } else if (selector_ && (isInputColorSpaceSchema(definition_.schemaKey) ||
                             definition_.schemaKey == "bloom.image.asset" ||
                             definition_.schemaKey == "bloom.video.asset" ||
                             definition_.schemaKey == "bloom.audio.asset"))
        value = selector_->itemData(selector_->currentIndex()).toString().toStdString();
    else if (slider_)
        value = slider_->value();
    else if (selector_)
        value = selector_->itemData(selector_->currentIndex()).value<std::int64_t>();
    else if (integer_)
        value = integer_->integerValue();
    else if (toggle_)
        value = toggle_->isChecked();
    else if (text_)
        value = text_->text().toStdString();
    else if (multiline_)
        value = multiline_->toPlainText().toStdString();
    else if (color_) {
        const auto converted = properties::colorFromFields(
            session_, definition_.schemaKey, {fields_[0], fields_[1], fields_[2], fields_[3]});
        if (!converted)
            return;
        value = *converted;
    } else if (fields_[2])
        value = document::Vec3d{fields_[0]->value(), fields_[1]->value(), fields_[2]->value()};
    else if (fields_[1])
        value = document::Vec2d{fields_[0]->value(), fields_[1]->value()};
    else if (definition_.valueKind == document::ParameterValueKind::Integer)
        value = static_cast<std::int64_t>(fields_[0]->value());
    else
        value = numericFieldValue(*fields_[0]);
    (void)session_.setParameterValue(parameter_, value, tr("Set Parameter"));
    refresh();
}
} // namespace bloom::ui
