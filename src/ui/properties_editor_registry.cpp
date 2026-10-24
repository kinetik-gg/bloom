#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/document/channel_nodes.hpp>
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
namespace {
[[nodiscard]] QString boundText(const CompositionSession& session, const document::NodeRecord& node,
                                const std::string_view role) {
    for (const auto& binding : node.parameters)
        if (binding.role == role)
            return session.constantStringValue(binding.parameterId).value_or(QString{});
    return {};
}
} // namespace

void PropertiesEditor::configureRegistryRows() {
    awaitingImageParts_ = false;
    awaitingCryptomatteLayers_ = false;
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
            if ((node->typeId == "bloom.image-source" || node->typeId == "bloom.video-source")) {
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
            if (node->typeId == document::kImageSourceNodeType)
                addImageSelectionRows(*node, section);
            if (node->typeId == document::kCompositionOutputNodeType)
                addOutputLayerRow(*node, section);
            if (node->typeId == document::kCryptomatteNodeType)
                addCryptomatteRows(*node, section);
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
    if (node != nullptr && node->typeId == document::kImageSourceNodeType)
        populateImageSelectionRows(*node);
    if (node != nullptr && node->typeId == document::kCryptomatteNodeType)
        populateCryptomatteRows(*node);
    if (node != nullptr && node->typeId == document::kCompositionOutputNodeType &&
        outputLayersField_ != nullptr && !outputLayersField_->hasFocus())
        outputLayersField_->setText(
            boundText(session_, *node, document::kCompositionOutputLayersParameterRole));
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

void PropertiesEditor::imagePartsMayHaveArrived() {
    if (awaitingImageParts_)
        configureRegistryRows();
}

// ADR 0026: the Image source's part and colour-layer pickers read the probe through the Assets
// worker; a selection that no longer exists is shown as missing and diagnosed at evaluation. The
// rows are created with the registry rows and repopulated in place on every configure.
void PropertiesEditor::addImageSelectionRows(const document::NodeRecord& node,
                                             kit::KSection* section) {
    const auto addPicker = [&](const QString& name, const char* object) {
        auto* label = kit::makePropertyRowLabel(name, section->body());
        auto* picker = new kit::KDropdown(section->body());
        picker->setObjectName(object);
        auto* row = new kit::KPropertyRow(label, nullptr, {picker}, section->body());
        row->setProperty("rowLabel", name);
        section->bodyLayout()->addWidget(row);
        return picker;
    };
    imagePartPicker_ = addPicker(tr("Part"), "propertiesImagePart");
    imageLayerPicker_ = addPicker(tr("Layer"), "propertiesImageLayer");
    const auto nodeId = node.id;
    connect(imagePartPicker_, &kit::KDropdown::currentIndexChanged, this,
            [this, nodeId](const int index) {
                if (index < 0 || imagePartPicker_ == nullptr)
                    return;
                const auto value = imagePartPicker_->itemData(index).toString().toStdString();
                // A new part starts on its root layer.
                if (!session_.setNodeParameterValue(nodeId,
                                                    std::string(document::kImagePartParameterRole),
                                                    document::ParameterValue{value},
                                                    tr("Choose Image Part")) ||
                    !session_.setNodeParameterValue(
                        nodeId, std::string(document::kImageLayerParameterRole),
                        document::ParameterValue{std::string{}}, tr("Choose Image Layer")))
                    configureRegistryRows();
            });
    connect(imageLayerPicker_, &kit::KDropdown::currentIndexChanged, this,
            [this, nodeId](const int index) {
                if (index < 0 || imageLayerPicker_ == nullptr)
                    return;
                if (!session_.setNodeParameterValue(
                        nodeId, std::string(document::kImageLayerParameterRole),
                        document::ParameterValue{
                            imageLayerPicker_->itemData(index).toString().toStdString()},
                        tr("Choose Image Layer")))
                    configureRegistryRows();
            });
}

void PropertiesEditor::populateImageSelectionRows(const document::NodeRecord& node) {
    if (imagePartPicker_ == nullptr || imageLayerPicker_ == nullptr)
        return;
    auto* controller = session_.assetController();
    const auto assetText = boundText(session_, node, "asset");
    const auto assetId = document::AssetId::fromRaw(assetText.toULongLong());
    const std::vector<media::ImagePartInfo>* parts =
        controller != nullptr ? controller->imageParts(assetId) : nullptr;
    awaitingImageParts_ = controller != nullptr && parts == nullptr && !assetText.isEmpty();
    if (awaitingImageParts_) {
        controller->requestImageParts(assetId);
        // Repopulate once the Assets worker has read the parts.
        connect(controller, &AssetController::changed, this,
                &PropertiesEditor::imagePartsMayHaveArrived, Qt::UniqueConnection);
    }
    const auto currentPart = boundText(session_, node, document::kImagePartParameterRole);
    const auto currentLayer = boundText(session_, node, document::kImageLayerParameterRole);
    auto* partPicker = imagePartPicker_.data();
    auto* layerPicker = imageLayerPicker_.data();
    const QSignalBlocker partBlocker(partPicker);
    const QSignalBlocker layerBlocker(layerPicker);
    partPicker->clearItems();
    layerPicker->clearItems();
    partPicker->setToolTip({});
    const media::ImagePartInfo* selectedPart = nullptr;
    if (parts == nullptr || parts->empty()) {
        partPicker->addItem(currentPart.isEmpty() ? tr("First part") : currentPart, currentPart);
        layerPicker->addItem(currentLayer.isEmpty() ? tr("Root") : currentLayer, currentLayer);
        partPicker->setToolTip(parts == nullptr ? tr("Reading the image parts…")
                                                : tr("This image has no named parts"));
        partPicker->setEnabled(false);
        layerPicker->setEnabled(false);
        return;
    }
    int partIndex = -1;
    for (std::size_t index = 0; index < parts->size(); ++index) {
        const auto& part = (*parts)[index];
        const auto name = QString::fromStdString(part.name);
        // The first unnamed part (every still and single-part EXR) is stored as "".
        partPicker->addItem(name.isEmpty() ? tr("Image") : name, name);
        if (name == currentPart || (currentPart.isEmpty() && index == 0)) {
            partIndex = static_cast<int>(index);
            selectedPart = &part;
        }
    }
    if (partIndex < 0) {
        partPicker->addItem(tr("%1 (missing)").arg(currentPart), currentPart);
        partIndex = partPicker->count() - 1;
    }
    partPicker->setCurrentIndex(partIndex);
    partPicker->setEnabled(parts->size() > 1 || partIndex >= static_cast<int>(parts->size()));
    int layerIndex = -1;
    if (selectedPart != nullptr)
        for (const auto& layer : selectedPart->colourLayers) {
            const auto name = QString::fromStdString(layer);
            layerPicker->addItem(name.isEmpty() ? tr("Root") : name, name);
            if (name == currentLayer)
                layerIndex = layerPicker->count() - 1;
        }
    if (layerIndex < 0) {
        layerPicker->addItem(currentLayer.isEmpty() ? tr("Root (no colour)")
                                                    : tr("%1 (missing)").arg(currentLayer),
                             currentLayer);
        layerIndex = layerPicker->count() - 1;
    }
    layerPicker->setCurrentIndex(layerIndex);
    layerPicker->setEnabled(true);
}

// ADR 0026 §7: the layers a layered OpenEXR writes from the Composition Output's channel set.
void PropertiesEditor::addOutputLayerRow(const document::NodeRecord& node,
                                         kit::KSection* section) {
    const auto name = tr("Channel Layers");
    auto* label = kit::makePropertyRowLabel(name, section->body());
    auto* field = new kit::KLineEdit(section->body());
    field->setObjectName("propertiesOutputChannelLayers");
    field->setPlaceholderText(tr("Every layer"));
    field->setToolTip(tr("Comma-separated layer names written beside RGBA by layered OpenEXR"));
    auto* row = new kit::KPropertyRow(label, nullptr, {field}, section->body());
    row->setProperty("rowLabel", name);
    section->bodyLayout()->addWidget(row);
    outputLayersField_ = field;
    const auto nodeId = node.id;
    connect(field, &QLineEdit::editingFinished, this, [this, nodeId] {
        if (outputLayersField_ == nullptr)
            return;
        if (!session_.setNodeParameterValue(
                nodeId, std::string(document::kCompositionOutputLayersParameterRole),
                document::ParameterValue{outputLayersField_->text().trimmed().toStdString()},
                tr("Set Channel Layers")))
            configureRegistryRows();
    });
}

void PropertiesEditor::cryptomatteLayersMayHaveArrived() {
    if (awaitingCryptomatteLayers_)
        configureRegistryRows();
}

// #364: the Cryptomatte layer picker lists the types in the node's channel set (a worker read);
// the selected mattes are listed by name with a remove button each, and a field adds a name or a
// raw ID. The Viewer eyedropper edits the same list.
void PropertiesEditor::addCryptomatteRows(const document::NodeRecord& node,
                                          kit::KSection* section) {
    const auto nodeId = node.id;
    {
        const auto name = tr("Layer");
        auto* label = kit::makePropertyRowLabel(name, section->body());
        auto* picker = new kit::KDropdown(section->body());
        picker->setObjectName("propertiesCryptomatteLayer");
        auto* row = new kit::KPropertyRow(label, nullptr, {picker}, section->body());
        row->setProperty("rowLabel", name);
        section->bodyLayout()->addWidget(row);
        cryptomatteLayerPicker_ = picker;
        connect(picker, &kit::KDropdown::currentIndexChanged, this,
                [this, nodeId](const int index) {
                    if (index < 0 || cryptomatteLayerPicker_ == nullptr)
                        return;
                    if (!session_.setNodeParameterValue(
                            nodeId, std::string(document::kCryptomatteLayerParameterRole),
                            document::ParameterValue{
                                cryptomatteLayerPicker_->itemData(index).toString().toStdString()},
                            tr("Choose Cryptomatte Layer")))
                        configureRegistryRows();
                });
    }
    auto* list = new QWidget(section->body());
    list->setObjectName("propertiesCryptomatteMattes");
    auto* listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);
    section->bodyLayout()->addWidget(list);
    cryptomatteMatteList_ = list;
    {
        const auto name = tr("Add Matte");
        auto* label = kit::makePropertyRowLabel(name, section->body());
        auto* field = new kit::KLineEdit(section->body());
        field->setObjectName("propertiesCryptomatteAdd");
        field->setPlaceholderText(tr("Name or <ID>"));
        field->setToolTip(tr("Type an object name, or pick in the Viewer with the Cryptomatte "
                             "eyedropper: click or Shift-click adds, Alt- or Ctrl-click removes"));
        auto* row = new kit::KPropertyRow(label, nullptr, {field}, section->body());
        row->setProperty("rowLabel", name);
        section->bodyLayout()->addWidget(row);
        cryptomatteAddField_ = field;
        connect(field, &QLineEdit::returnPressed, this, [this, nodeId] {
            auto* controller = session_.cryptomatteController();
            if (cryptomatteAddField_ == nullptr || controller == nullptr)
                return;
            const auto text = cryptomatteAddField_->text().trimmed().toStdString();
            if (text.empty())
                return;
            auto mattes = controller->mattes(nodeId);
            for (auto& added : runtime::splitCryptomatteMatteList(text))
                if (std::ranges::find(mattes, added) == mattes.end())
                    mattes.push_back(std::move(added));
            cryptomatteAddField_->clear();
            if (!controller->setMattes(nodeId, mattes, tr("Add Cryptomatte Matte")))
                configureRegistryRows();
        });
    }
}

void PropertiesEditor::populateCryptomatteRows(const document::NodeRecord& node) {
    auto* controller = session_.cryptomatteController();
    const auto currentLayer = boundText(session_, node, document::kCryptomatteLayerParameterRole);
    if (cryptomatteLayerPicker_ != nullptr) {
        const std::vector<std::string>* layers =
            controller != nullptr ? controller->layers(node.id) : nullptr;
        awaitingCryptomatteLayers_ = controller != nullptr && layers == nullptr;
        if (awaitingCryptomatteLayers_) {
            controller->requestLayers(node.id);
            connect(controller, &CryptomatteController::changed, this,
                    &PropertiesEditor::cryptomatteLayersMayHaveArrived, Qt::UniqueConnection);
        }
        auto* picker = cryptomatteLayerPicker_.data();
        const QSignalBlocker blocker(picker);
        picker->clearItems();
        int current = -1;
        if (layers != nullptr)
            for (const auto& layer : *layers) {
                const auto name = QString::fromStdString(layer);
                picker->addItem(name, name);
                if (name == currentLayer)
                    current = picker->count() - 1;
            }
        if (current < 0) {
            picker->addItem(layers == nullptr ? currentLayer : tr("%1 (missing)").arg(currentLayer),
                            currentLayer);
            current = picker->count() - 1;
        }
        picker->setCurrentIndex(current);
        picker->setEnabled(layers != nullptr && !layers->empty());
        const auto diagnostic =
            controller != nullptr ? controller->layerDiagnostic(node.id) : QString{};
        picker->setToolTip(layers == nullptr       ? tr("Reading the Cryptomatte layers…")
                           : !diagnostic.isEmpty() ? diagnostic
                           : layers->empty()       ? tr("The channel set has no Cryptomatte layer")
                                                   : QString{});
    }
    if (cryptomatteMatteList_ == nullptr)
        return;
    auto* list = cryptomatteMatteList_.data();
    for (auto* child : list->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
        child->hide();
        child->deleteLater();
    }
    const auto mattes = controller != nullptr
                            ? controller->mattes(node.id)
                            : runtime::splitCryptomatteMatteList(
                                  boundText(session_, node,
                                            document::kCryptomatteMattesParameterRole)
                                      .toStdString());
    const auto nodeId = node.id;
    const auto addRow = [&](const QString& text, QWidget* value) {
        auto* label = kit::makePropertyRowLabel(text, list);
        auto* row = new kit::KPropertyRow(label, nullptr, {value}, list);
        row->setProperty("rowLabel", text);
        list->layout()->addWidget(row);
    };
    auto* count = new QLabel(mattes.empty() ? tr("None selected")
                                            : tr("%n selected", nullptr,
                                                 static_cast<int>(mattes.size())),
                             list);
    count->setObjectName("propertiesCryptomatteCount");
    addRow(tr("Mattes"), count);
    for (std::size_t index = 0; index < mattes.size(); ++index) {
        const auto name = QString::fromStdString(mattes[index]);
        auto* remove = new kit::KButton(kit::IconId::Subtract, tr("Remove"), list);
        remove->setObjectName("propertiesCryptomatteRemove");
        remove->setVariant(kit::KButton::Variant::Ghost);
        remove->setControlSize(kit::KButton::ControlSize::Compact);
        remove->setToolTip(tr("Remove %1 from the matte").arg(name));
        remove->setProperty("matteName", name);
        connect(remove, &QAbstractButton::clicked, this, [this, nodeId, matte = mattes[index]] {
            auto* current = session_.cryptomatteController();
            if (current == nullptr)
                return;
            auto list = current->mattes(nodeId);
            std::erase(list, matte);
            if (!current->setMattes(nodeId, list, tr("Remove Cryptomatte Matte")))
                configureRegistryRows();
        });
        addRow(name, remove);
    }
}

} // namespace bloom::ui
