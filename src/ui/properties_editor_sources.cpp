#include "composition_editor_support.hpp"
#include "node_editor_items.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/properties_editor.hpp>

namespace bloom::ui {
namespace {
const document::NodeRecord* selectedPresentationSource(const CompositionSession& session) {
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return directSourceNode(session, *layerId);
    }
    return session.selectedNode();
}

} // namespace

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

    solidColorKeyframe_->refresh();
    const auto value = session_.effectiveColorValue(document::kSolidColorParameterRole);
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
    solidColorChip_->setEnabled(canEditColor);
    if (value) {
        const QSignalBlocker blocker(solidColorChip_);
        solidColorChip_->setColor(kit::KColor::fromRgba(
            static_cast<float>(value->red), static_cast<float>(value->green),
            static_cast<float>(value->blue), static_cast<float>(value->alpha)));
        solidColorChip_->setToolTip(exactColorText(*value));
    }
    // Mirrors Position/Opacity's own tooltip shape exactly.
    const QString colorTip = parameterSourceDescription(*parameter);
    for (auto* field : {solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_}) {
        field->setToolTip(colorTip);
    }
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

    const auto sizeValue = session_.effectiveScalarValue(document::kTextSizeParameterRole);
    textSize_->setEnabled(sizeValue.has_value());
    if (sizeValue.has_value()) {
        const QSignalBlocker blocker(textSize_);
        textSize_->setValue(*sizeValue);
    }
    textSize_->setToolTip(parameterSourceDescription(*size));

    const auto colorValue = session_.effectiveColorValue(document::kTextColorParameterRole);
    textColor_->setEnabled(colorValue.has_value());
    if (colorValue.has_value()) {
        const QSignalBlocker blocker(textColor_);
        textColor_->setColor(kit::KColor::fromRgba(
            static_cast<float>(colorValue->red), static_cast<float>(colorValue->green),
            static_cast<float>(colorValue->blue), static_cast<float>(colorValue->alpha)));
    }
    if (colorValue) {
        const std::array channels{colorValue->red, colorValue->green, colorValue->blue,
                                  colorValue->alpha};
        for (std::size_t index = 0; index < channels.size(); ++index) {
            const QSignalBlocker blocker(textColorFields_[index]);
            textColorFields_[index]->setValue(channels[index]);
            textColorFields_[index]->setEnabled(textColor_->isEnabled());
        }
    }
    // The chip's own value model is 8-bit-displayable straight RGBA in [0, 1], so an HDR or
    // negative authored channel cannot be shown in the swatch or round-tripped through the picker.
    // The exact stored value travels in the tooltip.
    textColor_->setToolTip(
        colorValue.has_value()
            ? tr("%1\nEditing here commits a color inside the displayable [0, 1] range")
                  .arg(exactColorText(*colorValue))
            : parameterSourceDescription(*color));
    textColorKeyframe_->refresh();
    textSizeKeyframe_->refresh();
}

} // namespace bloom::ui
