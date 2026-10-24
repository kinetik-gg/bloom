#include "composition_editor_support.hpp"
#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include <QSignalBlocker>
#include <bloom/document/channel_nodes.hpp>
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

QList<std::pair<QString, std::int64_t>> propertiesSelectorItems(std::string_view schemaKey) {
    if (schemaKey == document::kGradientRepeatParameterSchemaKey)
        return {{QObject::tr("Clamp"), 0}, {QObject::tr("Repeat"), 1},
                {QObject::tr("Mirror"), 2}};
    if (schemaKey == "bloom.blur.edge")
        return {{QObject::tr("Transparent"), 0}, {QObject::tr("Clamp"), 1}};
    // #361: the closed matte selector (render::MatteSelection) in track-matte order.
    if (schemaKey == "bloom.set-alpha.channel" || schemaKey == "bloom.multiply-alpha.channel")
        return {{QObject::tr("Alpha"), 0},
                {QObject::tr("Red"), 1},
                {QObject::tr("Green"), 2},
                {QObject::tr("Blue"), 3},
                {QObject::tr("Luma"), 4},
                {QObject::tr("Alpha Inverted"), 5},
                {QObject::tr("Red Inverted"), 6},
                {QObject::tr("Green Inverted"), 7},
                {QObject::tr("Blue Inverted"), 8},
                {QObject::tr("Luma Inverted"), 9}};
    QList<std::pair<QString, std::int64_t>> items;
    const auto add = [&items](const QString& text, const std::int64_t stored) {
        items.append({text, stored});
    };
    if (schemaKey == "bloom.resize-fit.filter") {
        add(QObject::tr("Bilinear"), 0);
        return items;
    }
    if (schemaKey == "bloom.resize-fit.policy") {
        add(QObject::tr("Fit"), 0);
        add(QObject::tr("Fill"), 1);
        return items;
    }
    if (schemaKey == "bloom.ocio-file.interpolation") {
        add(QObject::tr("Linear"), 0);
        add(QObject::tr("Tetrahedral"), 1);
        add(QObject::tr("Best"), 2);
        return items;
    }
    if (schemaKey == "bloom.ocio-file.direction") {
        add(QObject::tr("Forward"), 0);
        add(QObject::tr("Inverse"), 1);
        return items;
    }
    if (schemaKey == "bloom.shape.kind") {
        for (std::int64_t i = 0; i <= 6; ++i)
            add(QString::fromUtf8(
                    document::shapeKindName(static_cast<document::ShapeKind>(i)).data()),
                i);
        return items;
    }
    if (schemaKey == "bloom.shape.stroke-align") {
        add(QObject::tr("Center"), 0);
        add(QObject::tr("Inside"), 1);
        add(QObject::tr("Outside"), 2);
        return items;
    }
    if (schemaKey == "bloom.shape.stroke-join") {
        add(QObject::tr("Miter"), 0);
        add(QObject::tr("Round"), 1);
        add(QObject::tr("Bevel"), 2);
        return items;
    }
    if (schemaKey == "bloom.shape.stroke-cap") {
        add(QObject::tr("Butt"), 0);
        add(QObject::tr("Round"), 1);
        add(QObject::tr("Square"), 2);
        return items;
    }
    if (schemaKey == "bloom.shape.fill-rule") {
        add(QObject::tr("Nonzero"), 0);
        add(QObject::tr("Even-odd"), 1);
        return items;
    }
    if (schemaKey == "bloom.image.loop-mode" || schemaKey == "bloom.video.loop-mode" ||
        schemaKey == "bloom.composition-source.loop-mode") {
        add(QObject::tr("Hold"), 0);
        add(QObject::tr("Loop"), 1);
        add(QObject::tr("Ping-pong"), 2);
        return items;
    }
    if (schemaKey == "bloom.image.color-space" || schemaKey == "bloom.video.color-space") {
        add(QObject::tr("Auto"), 0);
        add(QObject::tr("sRGB"), 1);
        add(QObject::tr("Linear"), 2);
        add(QObject::tr("Raw"), 3);
        return items;
    }
    if (schemaKey == document::kBlendModeParameterSchemaKey) {
        for (const auto mode : core::kBlendModes)
            add(blendModeDisplayName(mode), core::blendModeStoredValue(mode));
        return items;
    }
    if (schemaKey == document::kTextAlignmentParameterSchemaKey) {
        add(QObject::tr("Left"), 0);
        add(QObject::tr("Center"), 1);
        add(QObject::tr("Right"), 2);
        return items;
    }
    if (schemaKey == document::kTextFontParameterSchemaKey) {
        add(QObject::tr("DejaVu Sans"), document::kTextFontDejaVuSans);
        add(QObject::tr("Inter Regular"), document::kTextFontInterRegular);
        add(QObject::tr("Inter Medium"), document::kTextFontInterMedium);
        add(QObject::tr("Inter SemiBold"), document::kTextFontInterSemiBold);
        return items;
    }
    if (schemaKey == document::kScalarOperationParameterSchemaKey) {
        for (const auto operation : document::kScalarOperations) {
            const auto* signature = core::primitives::scalarPrimitiveSignature(operation);
            // The frozen signature's own id is the name, with its namespace trimmed: the card
            // must not invent a second spelling for an operation the kernel already names.
            const auto id = signature == nullptr ? std::string_view{} : signature->id;
            add(node_editor::displayTypeName(id.substr(id.rfind('.') + 1)),
                document::scalarOperationStoredValue(operation));
        }
        return items;
    }
    if (schemaKey == document::kVectorOperationParameterSchemaKey) {
        static constexpr std::array kNames{"Add",   "Subtract",  "Multiply",     "Divide",
                                           "Scale", "Normalize", "Cross Product"};
        for (std::size_t index = 0; index < document::kVectorOperations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::vectorOperationStoredValue(document::kVectorOperations[index]));
        }
        return items;
    }
    if (schemaKey == document::kVectorReductionParameterSchemaKey) {
        static constexpr std::array kNames{"Length", "Dot Product", "Distance"};
        for (std::size_t index = 0; index < document::kVectorReductions.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::vectorReductionStoredValue(document::kVectorReductions[index]));
        }
        return items;
    }
    if (schemaKey == document::kRangeInterpolationParameterSchemaKey) {
        static constexpr std::array kNames{"Linear", "Smoothstep", "Smootherstep"};
        for (std::size_t index = 0; index < document::kRangeInterpolations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::rangeInterpolationStoredValue(document::kRangeInterpolations[index]));
        }
        return items;
    }
    // Task UTIL-1's selectors. Each offers its own closed vocabulary in the order the document
    // numbers it, so the card cannot offer a member the schema would refuse.
    if (schemaKey == document::kRoundingModeParameterSchemaKey) {
        static constexpr std::array kNames{"Round", "Floor", "Ceiling", "Truncate"};
        for (std::size_t index = 0; index < document::kRoundingModes.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::selectorStoredValue(document::kRoundingModes[index]));
        }
        return items;
    }
    if (schemaKey == document::kIntegerOperationParameterSchemaKey) {
        static constexpr std::array kNames{"Add",    "Subtract", "Multiply", "Divide",
                                           "Modulo", "Minimum",  "Maximum"};
        for (std::size_t index = 0; index < document::kIntegerOperations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::selectorStoredValue(document::kIntegerOperations[index]));
        }
        return items;
    }
    if (schemaKey == document::kBooleanOperationParameterSchemaKey) {
        static constexpr std::array kNames{"And", "Or", "Xor", "Nand", "Nor"};
        for (std::size_t index = 0; index < document::kBooleanOperations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::selectorStoredValue(document::kBooleanOperations[index]));
        }
        return items;
    }
    if (schemaKey == document::kStringCaseParameterSchemaKey) {
        static constexpr std::array kNames{"Upper", "Lower", "Title"};
        for (std::size_t index = 0; index < document::kStringCaseModes.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::selectorStoredValue(document::kStringCaseModes[index]));
        }
        return items;
    }
    if (schemaKey == document::kStringPadSideParameterSchemaKey) {
        static constexpr std::array kNames{"Start", "End"};
        for (std::size_t index = 0; index < document::kStringPadSides.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::selectorStoredValue(document::kStringPadSides[index]));
        }
        return items;
    }
    if (schemaKey == document::kNumberRadixParameterSchemaKey) {
        // The stored value IS the radix, so the offered list is a convenience rather than a
        // mapping: a document carrying base 36 keeps it, and this dropdown simply has no row
        // for it.
        static constexpr std::array kNames{"Binary", "Octal", "Decimal", "Hexadecimal"};
        for (std::size_t index = 0; index < document::kOfferedRadices.size(); ++index) {
            add(QString::fromUtf8(kNames[index]), document::kOfferedRadices[index]);
        }
        return items;
    }
    if (schemaKey == document::kCompareOperationParameterSchemaKey) {
        static constexpr std::array kNames{"Equal",         "Not Equal", "Less",
                                           "Less Or Equal", "Greater",   "Greater Or Equal"};
        for (std::size_t index = 0; index < document::kCompareOperations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::compareOperationStoredValue(document::kCompareOperations[index]));
        }
        return items;
    }
    return items;
}

} // namespace bloom::ui
