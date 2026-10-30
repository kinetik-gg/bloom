#include "composition_editor_support.hpp"
#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include <QCoreApplication>
#include <QSignalBlocker>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <algorithm>
#include <bloom/core/scalar_primitives.hpp>
#include <bloom/ui/kit/icons.hpp>
namespace bloom::ui {
QString imageDimensionsText(const document::AssetRecord* asset) {
    return asset ? QObject::tr("%1 × %2").arg(asset->width).arg(asset->height)
                 : QObject::tr("Unavailable");
}
QString imageRangeText(const document::AssetRecord* asset) {
    if (asset && asset->kind == document::AssetKind::Video)
        return QObject::tr("%1 frames · %2 s")
            .arg(asset->frames)
            .arg(asset->duration.toSeconds(), 0, 'f', 2);
    if (!asset || asset->kind != document::AssetKind::Sequence)
        return {};
    return QObject::tr("%1 frames · %2–%3")
        .arg(asset->manifest.members.size())
        .arg(asset->manifest.first)
        .arg(asset->manifest.last);
}
QString videoFieldOrderText(const std::uint32_t fieldOrder) {
    switch (fieldOrder) {
    case 1:
        return QObject::tr("Progressive");
    case 2:
        return QObject::tr("Interlaced, top field first");
    case 3:
        return QObject::tr("Interlaced, bottom field first");
    case 4:
        return QObject::tr("Interlaced, top coded, bottom displayed first");
    case 5:
        return QObject::tr("Interlaced, bottom coded, top displayed first");
    default:
        return QObject::tr("Unknown");
    }
}
QString videoTimecodeText(const document::AssetVideoStream& stream) {
    if (stream.timecodeType == 0)
        return QObject::tr("None");
    return QObject::tr("%1 (%2)").arg(QString::fromStdString(stream.timecode),
                                      stream.timecodeType == 2 ? QObject::tr("drop-frame")
                                                               : QObject::tr("non-drop"));
}
QString imageAssetDisplayName(const document::AssetRecord& asset) {
    return QString::fromStdString(asset.name);
}
QString mediaLayerDisplayName(const CompositionSession& session, document::LayerId id) {
    const auto* composition = session.composition();
    const auto* layer = composition ? composition->graph().findLayer(id) : nullptr;
    const auto* source = directSourceNode(session, id);
    if (layer && source &&
        (source->typeId == "bloom.image-source" || source->typeId == "bloom.video-source" ||
         source->typeId == document::kAudioSourceNodeType)) {
        for (const auto& binding : source->parameters) {
            if (binding.role != "asset")
                continue;
            const auto value = session.constantStringValue(binding.parameterId);
            const auto* asset = value ? session.snapshot().project().findAsset(
                                            document::AssetId::fromRaw(value->toULongLong()))
                                      : nullptr;
            if (!asset)
                break;
            const auto importedName =
                asset->kind == document::AssetKind::Sequence
                    ? asset->manifest.pattern
                    : asset->locator.path.substr(asset->locator.path.find_last_of('/') + 1);
            // Existing import commands store the locator-derived label on the Layer. Project
            // organization changes its displayed default; an authored Layer name stays independent.
            if (layer->name == importedName || layer->name == asset->locator.path)
                return imageAssetDisplayName(*asset);
        }
    }
    return composition ? layerName(*composition, id) : QString{};
}
void refreshAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                          const QString& stored, const bool audioOnly,
                          const bool videoOnly = false) {
    const QSignalBlocker blocker(&selector);
    selector.clearItems();
    selector.addItem(QObject::tr("Choose Asset"), QString{});
    for (const auto& asset : session.snapshot().project().assets()) {
        if (asset.kind == document::AssetKind::Lut)
            continue;
        if (videoOnly != (asset.kind == document::AssetKind::Video) ||
            audioOnly != (asset.kind == document::AssetKind::Audio))
            continue;
        const bool sequence = asset.kind == document::AssetKind::Sequence;
        const auto kind =
            videoOnly   ? QObject::tr("Video")
            : audioOnly ? QObject::tr("Audio · %1 s").arg(asset.duration.toSeconds(), 0, 'f', 2)
            : sequence  ? QObject::tr("Sequence [%1]").arg(asset.manifest.members.size())
                        : QObject::tr("Image");
        selector.addItem(kit::icon(audioOnly  ? kit::IconId::Audio
                                   : sequence ? kit::IconId::Images
                                              : kit::IconId::Image,
                                   kit::IconRole::Chrome, kit::Color::Muted),
                         imageAssetDisplayName(asset) + " · " + kind,
                         QString::number(asset.id.value()));
    }
    auto index = selector.findData(stored);
    const bool missing = index < 0 && !stored.isEmpty();
    if (missing) {
        index = selector.addItem(QObject::tr("Missing asset"), stored);
        selector.setItemToolTip(index, stored);
    }
    selector.setCurrentIndex(index < 0 ? 0 : index);
    selector.setMutedValue(missing);
    selector.setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
    selector.setToolTip(missing ? QObject::tr("Missing asset: %1").arg(stored)
                                : selector.currentText());
}
void refreshImageAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored) {
    refreshAssetSelector(selector, session, stored, false);
}
void refreshVideoAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored) {
    refreshAssetSelector(selector, session, stored, false, true);
}
void refreshAudioAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored) {
    refreshAssetSelector(selector, session, stored, true);
}

namespace {
[[nodiscard]] QString boundNodeText(const CompositionSession& session,
                                    const document::NodeRecord& node, const std::string_view role) {
    for (const auto& binding : node.parameters)
        if (binding.role == role)
            return session.constantStringValue(binding.parameterId).value_or(QString{});
    return {};
}
// The probed parts of the node's asset, requesting them when unknown.
[[nodiscard]] const std::vector<media::ImagePartInfo>* probedParts(const CompositionSession& session,
                                                                   const document::NodeRecord& node) {
    auto* controller = session.assetController();
    const auto text = boundNodeText(session, node, "asset");
    if (controller == nullptr || text.isEmpty())
        return nullptr;
    const auto asset = document::AssetId::fromRaw(text.toULongLong());
    const auto* parts = controller->imageParts(asset);
    if (parts == nullptr)
        controller->requestImageParts(asset);
    return parts;
}
[[nodiscard]] const media::ImagePartInfo* selectedPart(const std::vector<media::ImagePartInfo>& parts,
                                                       const QString& name) {
    if (name.isEmpty())
        return parts.empty() ? nullptr : &parts.front();
    const auto found = std::ranges::find(parts, name.toStdString(), &media::ImagePartInfo::name);
    return found == parts.end() ? nullptr : &*found;
}
// Fills `selector` with (label, value) items and selects `current`, adding "(missing)" for a
// value the probe does not list.
bool fillProbeSelector(kit::KDropdown& selector, const QList<std::pair<QString, QString>>& items,
                       const QString& current, const bool known, const QString& pending) {
    const QSignalBlocker blocker(selector);
    selector.clearItems();
    int index = -1;
    for (const auto& [label, value] : items) {
        selector.addItem(label, value);
        if (value == current)
            index = selector.count() - 1;
    }
    const bool listed = index >= 0;
    if (!listed) {
        selector.addItem(known ? QObject::tr("%1 (missing)").arg(current)
                               : (current.isEmpty() ? pending : current),
                         current);
        index = selector.count() - 1;
    }
    selector.setCurrentIndex(index);
    selector.setEnabled(known && (items.size() > 1 || !listed));
    selector.setMutedValue(known && !listed);
    selector.setToolTip(known ? QString{} : QObject::tr("Reading the file…"));
    return listed;
}
} // namespace

bool refreshImagePartSelector(kit::KDropdown& selector, const CompositionSession& session,
                              const document::NodeRecord& node) {
    const auto* parts = probedParts(session, node);
    QList<std::pair<QString, QString>> items{{QObject::tr("First part"), QString{}}};
    if (parts != nullptr)
        for (std::size_t index = 1; index < parts->size(); ++index)
            if (!(*parts)[index].name.empty())
                items.push_back({QString::fromStdString((*parts)[index].name),
                                 QString::fromStdString((*parts)[index].name)});
    return fillProbeSelector(selector, items,
                             boundNodeText(session, node, document::kImagePartParameterRole),
                             parts != nullptr, QObject::tr("First part"));
}

bool refreshImageLayerSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const document::NodeRecord& node) {
    const auto* parts = probedParts(session, node);
    const auto* part =
        parts == nullptr
            ? nullptr
            : selectedPart(*parts, boundNodeText(session, node, document::kImagePartParameterRole));
    QList<std::pair<QString, QString>> items;
    if (part != nullptr)
        for (const auto& layer : part->colourLayers)
            items.push_back({layer.empty() ? QObject::tr("Colour (root)")
                                           : QString::fromStdString(layer),
                             QString::fromStdString(layer)});
    const auto current = boundNodeText(session, node, document::kImageLayerParameterRole);
    if (part != nullptr && items.empty()) {
        // A data-only part: nothing to choose; the note says what to use instead.
        const QSignalBlocker blocker(selector);
        selector.clearItems();
        selector.addItem(QObject::tr("No colour layer"), current);
        selector.setCurrentIndex(0);
        selector.setEnabled(false);
        selector.setMutedValue(true);
        selector.setToolTip(QObject::tr("No colour layer — use Channels"));
        return true;
    }
    return fillProbeSelector(selector, items, current, part != nullptr,
                             QObject::tr("Colour (root)"));
}

bool refreshCryptomatteLayerSelector(kit::KDropdown& selector, const CompositionSession& session,
                                     const document::NodeRecord& node) {
    auto* controller = session.cryptomatteController();
    const auto* layers = controller != nullptr ? controller->layers(node.id) : nullptr;
    if (controller != nullptr && !controller->listingCurrent(node.id))
        controller->requestLayers(node.id);
    QList<std::pair<QString, QString>> items;
    if (layers != nullptr)
        for (const auto& layer : *layers)
            items.push_back({QString::fromStdString(layer), QString::fromStdString(layer)});
    const auto current = boundNodeText(session, node, document::kCryptomatteLayerParameterRole);
    const bool listed = fillProbeSelector(selector, items, current, layers != nullptr, current);
    if (layers != nullptr && layers->empty())
        selector.setToolTip(QObject::tr("The channel set has no Cryptomatte layer"));
    return listed;
}

QString imageSourceColourNote(const CompositionSession& session, const document::NodeRecord& node) {
    auto* controller = session.assetController();
    const auto text = boundNodeText(session, node, "asset");
    if (controller == nullptr || text.isEmpty())
        return {};
    const auto* parts = controller->imageParts(document::AssetId::fromRaw(text.toULongLong()));
    const auto* part =
        parts == nullptr
            ? nullptr
            : selectedPart(*parts, boundNodeText(session, node, document::kImagePartParameterRole));
    const auto layer = boundNodeText(session, node, document::kImageLayerParameterRole).toStdString();
    return part != nullptr && std::ranges::find(part->colourLayers, layer) == part->colourLayers.end()
               ? QObject::tr("No colour layer — use Channels")
               : QString{};
}

PropertiesRowControl propertiesRowControl(std::string_view schemaKey) {
    if (schemaKey == document::kTextAlignmentParameterSchemaKey)
        return PropertiesRowControl::SegmentedEnum;
    if (schemaKey == document::kTextSizeParameterSchemaKey ||
        schemaKey == document::kTextLineHeightParameterSchemaKey ||
        schemaKey == document::kTextLetterSpacingParameterSchemaKey)
        return PropertiesRowControl::Stepper;
    return PropertiesRowControl::Automatic;
}

// The labels come from the document's one enum table (document::parameterEnumOptions), the same
// members scripting, MCP and the CLI accept by name; only the translation is applied here, at
// display time. Properties, the Nodes cards and the Timeline rows cannot offer a member the table
// does not have, nor spell one differently.
QList<std::pair<QString, std::int64_t>> propertiesSelectorItems(std::string_view schemaKey) {
    QList<std::pair<QString, std::int64_t>> items;
    for (const auto& option : document::parameterEnumOptions(schemaKey))
        items.append({QCoreApplication::translate("PropertiesSelector", option.label.c_str()),
                      option.value});
    return items;
}

} // namespace bloom::ui
