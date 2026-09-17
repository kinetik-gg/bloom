#include "asset_drop.hpp"
#include "composition_editor_support.hpp"
#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include "properties_sections.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <limits>

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
    if (node) {
        if (const auto* definition =
                document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)) {
            for (const auto& output : definition->outputs)
                signature += QString("/output:%1:%2")
                                 .arg(QString::fromStdString(output.name))
                                 .arg(static_cast<int>(output.valueKind));
        }
    }
    if (signature != registrySignature_) {
        for (auto* row : registryRows_) {
            for (auto* section : sections_)
                disconnect(section, nullptr, row, nullptr);
            // CRASH-2: cancel any in-flight font-catalogue poll before orphaning the row -- see
            // PropertiesRegistryRow::detachFromSession().
            row->detachFromSession();
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
            if (node->typeId == document::kCompositionSourceNodeType) {
                auto* label = kit::makePropertyRowLabel(tr("Composition"), section->body());
                auto* selector = new kit::KDropdown(section->body());
                selector->setObjectName("propertiesCompositionSource");
                auto* open = new kit::KButton(tr("Open"), section->body());
                open->setObjectName("propertiesOpenComposition");
                open->setControlSize(kit::KButton::ControlSize::Compact);
                auto* row =
                    new kit::KPropertyRow(label, nullptr, {selector, open}, section->body());
                row->setProperty("rowLabel", tr("Composition"));
                section->bodyLayout()->addWidget(row);
                for (const auto& binding : node->parameters)
                    if (binding.role == "composition") {
                        const auto parameterId = binding.parameterId;
                        connect(selector, &kit::KDropdown::currentIndexChanged, this,
                                [this, selector, parameterId](int index) {
                                    if (index >= 0 &&
                                        !session_.setParameterValue(
                                            parameterId,
                                            static_cast<std::int64_t>(
                                                selector->itemData(index).toLongLong()),
                                            tr("Change Source Composition")))
                                        configureRegistryRows();
                                });
                        break;
                    }
                connect(open, &kit::KButton::clicked, this, [this, selector] {
                    (void)session_.setComposition(
                        document::CompositionId::fromRaw(selector->currentData().toULongLong()));
                });
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
                for (const auto& [name, object, value] : std::array{
                         std::tuple{tr("Duration"), "propertiesAudioDuration",
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
            bool hasValueOutputs = false;
            for (const auto& output : definition->outputs) {
                if (output.valueKind == document::SocketValueKind::Image ||
                    output.valueKind == document::SocketValueKind::Audio)
                    continue;
                hasValueOutputs = true;
                const auto label = node_editor::displayTypeName(output.name);
                auto* rowLabel = kit::makePropertyRowLabel(label, section->body());
                auto* readout = new kit::KLabel(section->body());
                readout->setObjectName(QStringLiteral("propertiesValueOutput.%1")
                                           .arg(QString::fromStdString(output.name)));
                readout->setProperty(
                    "valueOutputNodeId",
                    QVariant::fromValue(static_cast<qulonglong>(node->id.value())));
                readout->setProperty("valueOutputPort", QString::fromStdString(output.name));
                const auto resolved = session_.valueOutputText(node->id, output.name);
                readout->setText(resolved.isEmpty() ? tr("Resolving…") : resolved);
                auto* row = new kit::KPropertyRow(rowLabel, nullptr, {readout}, section->body());
                row->setProperty("rowLabel", label);
                section->bodyLayout()->addWidget(row);
            }
            if (registryPanel_)
                registryPanel_->setProperty("hasValueOutputs", hasValueOutputs);
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
                if (handcrafted || (node->typeId == document::kCompositionSourceNodeType &&
                                    declared.role == "composition"))
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
        registryPanel_->setVisible(!registryRows_.empty() ||
                                   registryPanel_->property("hasValueOutputs").toBool());
    for (auto* row : registryRows_)
        row->refresh();
    if (auto* selector = findChild<kit::KDropdown*>("propertiesCompositionSource");
        selector && node && session_.composition()) {
        const QSignalBlocker blocker(selector);
        const auto target = compositionSourceId(*session_.composition(), *node);
        selector->clearItems();
        selector->addItem(tr("Choose Composition"), QVariant::fromValue(qlonglong{0}));
        for (const auto& composition : session_.snapshot().project().compositions()) {
            if (composition.id() == session_.compositionId() ||
                composition.id().value() >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                continue;
            selector->addItem(
                QString::fromStdString(composition.name()),
                QVariant::fromValue(static_cast<qlonglong>(composition.id().value())));
        }
        const auto stored = QVariant::fromValue(static_cast<qlonglong>(target.value()));
        auto index = selector->findData(stored);
        if (index < 0)
            index = selector->addItem(tr("Missing composition"), stored);
        selector->setCurrentIndex(index);
        selector->setEnabled(!session_.composition()->nodeLocked(node->id));
        if (auto* open = findChild<kit::KButton*>("propertiesOpenComposition"))
            open->setEnabled(session_.snapshot().project().findComposition(target) != nullptr);
    }
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
} // namespace bloom::ui
