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
    solidColorChip_->setEnabled(canEditColor);
    if (value) {
        properties::refreshColor(
            session_, parameter->schemaKey, *value, solidColorChip_,
            {solidColorRed_, solidColorGreen_, solidColorBlue_, solidColorAlpha_});
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

    if (!session_.isValueEditing(content->id)) {
        textContent_->setProperty("ownsTextEdit", false);
        if (auto* multiline = findChild<QWidget*>("propertiesTextMultiline"))
            multiline->setProperty("ownsTextEdit", false);
    }
    const auto contentValue = session_.effectiveStringValue(content->id);
    textContent_->setEnabled(contentValue.has_value() && !session_.driverBindingFor(content->id) &&
                             !session_.composition()->parameterLocked(content->id));
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
    if (colorValue) {
        for (auto* field : textColorFields_)
            field->setEnabled(true);
        properties::refreshColor(
            session_, color->schemaKey, *colorValue, textColor_,
            {textColorFields_[0], textColorFields_[1], textColorFields_[2], textColorFields_[3]});
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
