#include "composition_editor_support.hpp"
#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include "properties_sections.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <array>
#include <tuple>

namespace bloom::ui {
void PropertiesEditor::configureRegistryRows() {
    const auto* node = session_.selectedNode();
    if (const auto* layer = std::get_if<document::LayerId>(&session_.selection().primary))
        node = directSourceNode(session_, *layer);
    auto signature = node ? QString("%1/%2/%3")
                                .arg(node->id.value())
                                .arg(QString::fromStdString(node->typeId))
                                .arg(node->schemaVersion)
                          : QString{};
    if (node)
        for (const auto& binding : node->parameters)
            signature += QString("/%1:%2")
                             .arg(QString::fromStdString(binding.role))
                             .arg(binding.parameterId.value());
    if (signature != registrySignature_) {
        for (auto* row : registryRows_) {
            for (auto* section : sections_)
                disconnect(section, nullptr, row, nullptr);
            row->setEnabled(false);
            row->hide();
            row->setParent(nullptr);
            row->deleteLater();
        }
        registryRows_.clear();
        if (registryPanel_) {
            auto* section = registryPanel_->findChild<kit::KSection*>();
            std::erase(sections_, section);
            registryPanel_->hide();
            registryPanel_->setParent(nullptr);
            registryPanel_->deleteLater();
            registryPanel_ = nullptr;
        }
        registrySignature_ = signature;
        const auto* definition =
            node ? document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
                 : nullptr;
        if (definition) {
            kit::KSection* section = nullptr;
            if (!solidColorPanel_->isHidden())
                section = solidColorPanel_->findChild<kit::KSection*>();
            else if (!textSourcePanel_->isHidden())
                section = textSourcePanel_->findChild<kit::KSection*>();
            if (!section) {
                registryPanel_ = new QWidget(selectionSection_);
                registryPanel_->setObjectName("propertiesRegistryPanel");
                auto* layout = new QVBoxLayout(registryPanel_);
                layout->setContentsMargins(0, 0, 0, 0);
                section = properties::addSection(
                    layout, registryPanel_, "registry",
                    node_editor::nodeDisplayName(*session_.composition(), *node));
                adoptSection(section, {});
                auto* selectionLayout = qobject_cast<QVBoxLayout*>(selectionSection_->layout());
                selectionLayout->insertWidget(selectionLayout->count() - 1, registryPanel_);
            }
            if (node->typeId == "bloom.image-source") {
                for (const auto& [name, object] :
                     std::array{std::pair{tr("Dimensions"), "propertiesImageDimensions"},
                                std::pair{tr("Range"), "propertiesImageRange"}}) {
                    auto* label = kit::makePropertyRowLabel(name, section->body());
                    auto* value = new kit::KLabel(section->body());
                    value->setObjectName(object);
                    auto* row = new kit::KPropertyRow(label, nullptr, {value}, section->body());
                    row->setProperty("rowLabel", name);
                    section->bodyLayout()->addWidget(row);
                }
            }
            if (node->typeId == "bloom.audio-source") {
                const document::AssetRecord* asset = nullptr;
                for (const auto& binding : node->parameters) {
                    if (binding.role != "asset")
                        continue;
                    const auto value = session_.constantStringValue(binding.parameterId);
                    if (value.has_value())
                        asset = session_.snapshot().project().findAsset(
                            document::AssetId::fromRaw(value->toULongLong()));
                    break;
                }
                for (const auto& [name, object, value] :
                     std::array{std::tuple{tr("Duration"), "propertiesAudioDuration",
                                           asset ? tr("%1 s").arg(asset->duration.toSeconds(), 0, 'f', 3)
                                                 : tr("Unavailable")},
                                  std::tuple{tr("Sample Rate"), "propertiesAudioRate",
                                             asset ? tr("%1 Hz").arg(asset->rate) : tr("Unavailable")},
                                  std::tuple{tr("Channels"), "propertiesAudioChannels",
                                             asset ? QString::number(asset->channels)
                                                   : tr("Unavailable")}}) {
                    auto* label = kit::makePropertyRowLabel(name, section->body());
                    auto* readout = new kit::KLabel(section->body());
                    readout->setObjectName(object);
                    readout->setElidedText(value);
                    auto* row = new kit::KPropertyRow(label, nullptr, {readout}, section->body());
                    row->setProperty("rowLabel", name);
                    section->bodyLayout()->addWidget(row);
                }
            }
            int textRowIndex = 3;
            for (const auto& declared : definition->parameters) {
                if (propertiesRowVisibility(declared.role, declared.schemaKey) ==
                    PropertiesRowVisibility::Hidden)
                    continue;
                // Only these roles already have purpose-built rows. Everything else comes from
                // the definition, including future source parameters and value-node operands.
                const bool handcrafted =
                    (definition->lowering == document::NodeLoweringKind::LayerOutput &&
                     (declared.role == document::kPositionParameterRole ||
                      declared.role == document::kAnchorParameterRole ||
                      declared.role == document::kScaleParameterRole ||
                      declared.role == document::kRotationParameterRole ||
                      declared.role == document::kOpacityParameterRole ||
                      declared.role == document::kBlendModeParameterRole)) ||
                    (definition->lowering == document::NodeLoweringKind::Solid &&
                     declared.role == document::kSolidColorParameterRole) ||
                    (definition->lowering == document::NodeLoweringKind::Text &&
                     (declared.role == document::kTextParameterRole ||
                      declared.role == document::kTextSizeParameterRole ||
                      declared.role == document::kTextColorParameterRole));
                if (handcrafted)
                    continue;
                const auto found = std::ranges::find(node->parameters, declared.role,
                                                     &document::ParameterBinding::role);
                if (found == node->parameters.end())
                    continue;
                auto* row = new PropertiesRegistryRow(session_, node->id, found->parameterId,
                                                      declared, section->body());
                if (definition->lowering == document::NodeLoweringKind::Text)
                    section->bodyLayout()->insertWidget(textRowIndex++, row);
                else
                    section->bodyLayout()->addWidget(row);
                registryRows_.push_back(row);
                connect(section, &kit::KSection::resetRequested, row, [row] { row->reset(); });
            }
        }
    }
    if (registryPanel_)
        registryPanel_->setVisible(!registryRows_.empty());
    for (auto* row : registryRows_)
        row->refresh();
    if (auto* dimensions = findChild<kit::KLabel*>("propertiesImageDimensions")) {
        const document::AssetRecord* asset = nullptr;
        if (node)
            for (const auto& binding : node->parameters)
                if (binding.role == "asset") {
                    const auto value = session_.constantStringValue(binding.parameterId);
                    if (value)
                        asset = session_.snapshot().project().findAsset(
                            document::AssetId::fromRaw(value->toULongLong()));
                }
        dimensions->setText(imageDimensionsText(asset));
        if (auto* range = findChild<kit::KLabel*>("propertiesImageRange")) {
            range->setText(imageRangeText(asset));
            range->parentWidget()->setProperty("unavailableReadout", range->text().isEmpty());
            range->parentWidget()->setVisible(!range->text().isEmpty());
        }
    }
}
void PropertiesEditor::filterRows() {
    const auto query = search_->text();
    for (auto* section : sections_) {
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
            if (row->property("unavailableReadout").toBool()) {
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
