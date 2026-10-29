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
#include <bloom/document/channel_nodes.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/commands/video_source_operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/dropdown.hpp>
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
            // The purpose-built rows go with the panel.
            imagePartPicker_ = nullptr;
            imageLayerPicker_ = nullptr;
            imageColourNote_ = nullptr;
            imageColourNoteRow_ = nullptr;
            outputLayersField_ = nullptr;
            cryptomatteLayerPicker_ = nullptr;
            cryptomatteMatteList_ = nullptr;
            cryptomatteAddField_ = nullptr;
            cryptomattePreview_ = nullptr;
            cryptomatteSearch_ = nullptr;
            cryptomatteNames_ = nullptr;
            cryptomatteNamesNote_ = nullptr;
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
                        connect(selector, &kit::KDropdown::activated, this,
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
            if (node->typeId == "bloom.video-source") {
                // Document 1.25: the played audio stream and the source's field order/timecode.
                auto* label = kit::makePropertyRowLabel(tr("Audio Stream"), section->body());
                auto* selector = new kit::KDropdown(section->body());
                selector->setObjectName(QStringLiteral("propertiesVideoAudioStream"));
                selector->setControlSize(kit::KDropdown::ControlSize::Dense);
                auto* row = new kit::KPropertyRow(label, nullptr, {selector}, section->body());
                row->setProperty("rowLabel", tr("Audio Stream"));
                section->bodyLayout()->addWidget(row);
                const auto nodeId = node->id;
                connect(selector, &kit::KDropdown::activated, this,
                        [this, selector, nodeId](int index) {
                            if (index < 0 || !selector->isEnabled())
                                return;
                            commands::Transaction transaction(
                                tr("Choose Audio Stream").toStdString(),
                                session_.snapshot().revision());
                            transaction.emplace<commands::SetVideoAudioStream>(
                                session_.compositionId(), nodeId,
                                static_cast<std::uint32_t>(selector->itemData(index).toUInt()));
                            (void)session_.executeTransaction(std::move(transaction));
                        });
                for (const auto& [name, object] :
                     std::array{std::pair{tr("Field Order"), "propertiesVideoFieldOrder"},
                                std::pair{tr("Timecode"), "propertiesVideoTimecode"}}) {
                    auto* rowLabel = kit::makePropertyRowLabel(name, section->body());
                    auto* value = new kit::KLabel(section->body());
                    value->setObjectName(object);
                    auto* infoRow =
                        new kit::KPropertyRow(rowLabel, nullptr, {value}, section->body());
                    infoRow->setProperty("rowLabel", name);
                    section->bodyLayout()->addWidget(infoRow);
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
                // Pixel and sound outputs carry no value to read out (a Mask or a channel set
                // never resolves to text, which left "Resolving…" on screen for good).
                if (output.valueKind == document::SocketValueKind::Image ||
                    output.valueKind == document::SocketValueKind::Audio ||
                    output.valueKind == document::SocketValueKind::Mask ||
                    output.valueKind == document::SocketValueKind::ChannelSet)
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
        // Purpose-built rows (Image part/layer, the Cryptomatte, the output layers) count too: a
        // node whose every parameter has a purpose-built row still shows its section.
        registryPanel_->setVisible(!registryRows_.empty() ||
                                   registryPanel_->property("hasValueOutputs").toBool() ||
                                   imagePartPicker_ != nullptr || cryptomatteLayerPicker_ != nullptr ||
                                   outputLayersField_ != nullptr);
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
        refreshVideoSourceRows(node, asset);
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
    {
        // ADR 0026 Q1: a data-only file has no Image output; say so plainly, as a note.
        const auto name = tr("Image");
        auto* label = kit::makePropertyRowLabel(name, section->body());
        auto* note = new kit::KLabel(section->body());
        note->setObjectName("propertiesImageColourNote");
        auto* row = new kit::KPropertyRow(label, nullptr, {note}, section->body());
        row->setProperty("rowLabel", name);
        row->setVisible(false);
        section->bodyLayout()->addWidget(row);
        imageColourNote_ = note;
        imageColourNoteRow_ = row;
    }
    const auto nodeId = node.id;
    connect(imagePartPicker_, &kit::KDropdown::activated, this,
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
    connect(imageLayerPicker_, &kit::KDropdown::activated, this,
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
    awaitingImageParts_ =
        controller != nullptr && !assetText.isEmpty() &&
        controller->imageParts(document::AssetId::fromRaw(assetText.toULongLong())) == nullptr;
    if (awaitingImageParts_)
        // Repopulate once the Assets worker has read the parts.
        connect(controller, &AssetController::changed, this,
                &PropertiesEditor::imagePartsMayHaveArrived, Qt::UniqueConnection);
    refreshImagePartSelector(*imagePartPicker_, session_, node);
    refreshImageLayerSelector(*imageLayerPicker_, session_, node);
    if (imageColourNote_ != nullptr) {
        const auto note = imageSourceColourNote(session_, node);
        imageColourNote_->setText(note);
        imageColourNoteRow_->setVisible(!note.isEmpty());
    }
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
        connect(picker, &kit::KDropdown::activated, this,
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
    {
        // The Viewer's ID-colour preview while this node is selected (a display choice).
        const auto name = tr("Preview");
        auto* label = kit::makePropertyRowLabel(name, section->body());
        auto* toggle = new kit::KSwitch(section->body());
        toggle->setObjectName("propertiesCryptomattePreview");
        toggle->setCheckable(true);
        toggle->setAccessibleName(name);
        toggle->setToolTip(tr("Show every object in its own colour in the Viewer"));
        auto* row = new kit::KPropertyRow(label, nullptr, {toggle}, section->body());
        row->setProperty("rowLabel", name);
        section->bodyLayout()->addWidget(row);
        cryptomattePreview_ = toggle;
        connect(toggle, &kit::KSwitch::toggled, this, [this, nodeId](const bool on) {
            if (auto* controller = session_.cryptomatteController())
                controller->setPreview(nodeId, on);
        });
    }
    {
        // One obvious step to the eyedropper, and one plain line saying what it does.
        const auto name = tr("Pick");
        auto* label = kit::makePropertyRowLabel(name, section->body());
        auto* pick = new kit::KButton(kit::IconId::Eyedropper, tr("Pick in Viewer"), section->body());
        pick->setObjectName("propertiesCryptomattePick");
        pick->setVariant(kit::KButton::Variant::Secondary);
        pick->setToolTip(tr("Click adds, Shift-click adds, Alt- or Ctrl-click removes"));
        auto* row = new kit::KPropertyRow(label, nullptr, {pick}, section->body());
        row->setProperty("rowLabel", name);
        section->bodyLayout()->addWidget(row);
        connect(pick, &QAbstractButton::clicked, this, [this] {
            if (auto* controller = session_.cryptomatteController())
                controller->requestPick();
        });
        auto* hint = new kit::KLabel(tr("Click objects in the Viewer to add them"), section->body(),
                                     kit::TypeRole::UiSmall);
        hint->setObjectName("propertiesCryptomatteHint");
        section->bodyLayout()->addWidget(hint);
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
    {
        // The manifest's names for the chosen layer, as a searchable checklist.
        const auto name = tr("Names");
        auto* label = kit::makePropertyRowLabel(name, section->body());
        auto* search = new kit::KSearchField(section->body());
        search->setObjectName("propertiesCryptomatteSearch");
        search->setPlaceholderText(tr("Search names"));
        auto* row = new kit::KPropertyRow(label, nullptr, {search}, section->body());
        row->setProperty("rowLabel", name);
        section->bodyLayout()->addWidget(row);
        cryptomatteSearch_ = search;
        connect(search, &QLineEdit::textChanged, this, [this] { filterCryptomatteNames(); });
        auto* note = new kit::KLabel(QString{}, section->body(), kit::TypeRole::UiSmall);
        note->setObjectName("propertiesCryptomatteNamesNote");
        section->bodyLayout()->addWidget(note);
        cryptomatteNamesNote_ = note;
        auto* names = new QWidget(section->body());
        names->setObjectName("propertiesCryptomatteNames");
        auto* namesLayout = new QVBoxLayout(names);
        namesLayout->setContentsMargins(0, 0, 0, 0);
        namesLayout->setSpacing(0);
        section->bodyLayout()->addWidget(names);
        cryptomatteNames_ = names;
        cryptomatteListedNames_.clear();
    }
}

void PropertiesEditor::filterCryptomatteNames() {
    if (cryptomatteNames_ == nullptr)
        return;
    const auto filter = cryptomatteSearch_ ? cryptomatteSearch_->text().trimmed() : QString{};
    for (auto* row : cryptomatteNames_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        row->setVisible(filter.isEmpty() ||
                        row->property("rowLabel").toString().contains(filter, Qt::CaseInsensitive));
}

void PropertiesEditor::populateCryptomatteRows(const document::NodeRecord& node) {
    auto* controller = session_.cryptomatteController();
    if (cryptomatteLayerPicker_ != nullptr) {
        awaitingCryptomatteLayers_ = controller != nullptr && !controller->listingCurrent(node.id);
        if (awaitingCryptomatteLayers_)
            connect(controller, &CryptomatteController::changed, this,
                    &PropertiesEditor::cryptomatteLayersMayHaveArrived, Qt::UniqueConnection);
        refreshCryptomatteLayerSelector(*cryptomatteLayerPicker_, session_, node);
    }
    if (cryptomattePreview_ != nullptr) {
        const QSignalBlocker blocker(cryptomattePreview_.data());
        cryptomattePreview_->setChecked(controller == nullptr || controller->preview(node.id));
    }
    if (cryptomatteNames_ != nullptr) {
        const auto* listing = controller != nullptr ? controller->listing(node.id) : nullptr;
        const auto selected = controller != nullptr ? controller->mattes(node.id)
                                                    : std::vector<std::string>{};
        const auto names = listing != nullptr ? listing->names : std::vector<std::string>{};
        auto* container = cryptomatteNames_.data();
        if (names != cryptomatteListedNames_) {
            for (auto* child : container->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
                // Detached first, so the search filter can never show a retiring row again.
                child->hide();
                child->setParent(nullptr);
                child->deleteLater();
            }
            cryptomatteListedNames_ = names;
            const auto nodeId = node.id;
            for (const auto& entry : names) {
                const auto text = QString::fromStdString(entry);
                auto* label = kit::makePropertyRowLabel(text, container);
                auto* check = new kit::KCheckBox(container);
                check->setObjectName("propertiesCryptomatteName");
                check->setCheckable(true);
                check->setAccessibleName(text);
                check->setProperty("matteName", text);
                auto* row = new kit::KPropertyRow(label, nullptr, {check}, container);
                row->setProperty("rowLabel", text);
                container->layout()->addWidget(row);
                connect(check, &kit::KSwitch::toggled, this,
                        [this, nodeId, entry](const bool on) {
                            auto* current = session_.cryptomatteController();
                            if (current == nullptr)
                                return;
                            auto list = current->mattes(nodeId);
                            const auto found = std::ranges::find(list, entry);
                            if (on && found == list.end())
                                list.push_back(entry);
                            else if (!on && found != list.end())
                                list.erase(found);
                            else
                                return;
                            if (!current->setMattes(nodeId, list,
                                                    on ? tr("Add Cryptomatte Matte")
                                                       : tr("Remove Cryptomatte Matte")))
                                configureRegistryRows();
                        });
            }
        }
        for (auto* check : container->findChildren<kit::KCheckBox*>("propertiesCryptomatteName")) {
            const QSignalBlocker blocker(check);
            check->setChecked(std::ranges::find(selected,
                                                check->property("matteName").toString().toStdString()) !=
                              selected.end());
        }
        filterCryptomatteNames();
        if (cryptomatteNamesNote_ != nullptr)
            cryptomatteNamesNote_->setText(
                listing == nullptr  ? tr("Reading the names…")
                : !listing->manifest ? tr("This layer has no name list — pick in the Viewer or type a name")
                                     : QString{});
        if (cryptomatteSearch_ != nullptr)
            cryptomatteSearch_->setEnabled(!names.empty());
    }
    if (cryptomatteMatteList_ == nullptr)
        return;
    auto* list = cryptomatteMatteList_.data();
    for (auto* child : list->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
        child->hide();
        child->deleteLater();
    }
    const auto mattes =
        controller != nullptr
            ? controller->mattes(node.id)
            : runtime::splitCryptomatteMatteList(
                  boundText(session_, node, document::kCryptomatteMattesParameterRole)
                      .toStdString());
    const auto nodeId = node.id;
    const auto addRow = [&](const QString& text, QWidget* value) {
        auto* label = kit::makePropertyRowLabel(text, list);
        auto* row = new kit::KPropertyRow(label, nullptr, {value}, list);
        row->setProperty("rowLabel", text);
        list->layout()->addWidget(row);
    };
    auto* count =
        new kit::KLabel(mattes.empty() ? tr("None selected")
                                  : tr("%n selected", nullptr, static_cast<int>(mattes.size())),
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

void PropertiesEditor::refreshVideoSourceRows(const document::NodeRecord* node,
                                              const document::AssetRecord* asset) {
    auto* selector = findChild<kit::KDropdown*>(QStringLiteral("propertiesVideoAudioStream"));
    if (selector == nullptr || node == nullptr)
        return;
    std::uint32_t selected = 0;
    for (const auto& binding : node->parameters)
        if (binding.role == document::kVideoAudioStreamParameterRole)
            if (const auto value = session_.constantValue(binding.parameterId))
                selected = static_cast<std::uint32_t>(*value);
    const QSignalBlocker blocker(selector);
    selector->clearItems();
    std::uint32_t ordinal = 0;
    if (asset != nullptr)
        for (const auto& stream : asset->videoStreams) {
            if (stream.kind != 2)
                continue;
            selector->addItem(tr("Stream %1 — %2 ch, %3 Hz (%4)")
                                  .arg(ordinal + 1)
                                  .arg(stream.channelLayout.size())
                                  .arg(stream.sampleRate)
                                  .arg(QString::fromStdString(stream.codec)),
                              QVariant::fromValue(ordinal));
            ++ordinal;
        }
    if (ordinal == 0)
        selector->addItem(tr("No audio stream"), QVariant::fromValue(0U));
    selector->setCurrentIndex(static_cast<int>(std::min(selected, ordinal == 0 ? 0 : ordinal - 1)));
    selector->setEnabled(ordinal > 1 && !session_.composition()->nodeLocked(node->id));
    const auto* video = asset == nullptr ? nullptr
                                         : &*std::ranges::find(asset->videoStreams, 1U,
                                                               &document::AssetVideoStream::kind);
    if (auto* fieldOrder = findChild<kit::KLabel*>(QStringLiteral("propertiesVideoFieldOrder")))
        fieldOrder->setText(video ? videoFieldOrderText(video->fieldOrder) : tr("Unavailable"));
    if (auto* timecode = findChild<kit::KLabel*>(QStringLiteral("propertiesVideoTimecode")))
        timecode->setText(video ? videoTimecodeText(*video) : tr("Unavailable"));
}
} // namespace bloom::ui
