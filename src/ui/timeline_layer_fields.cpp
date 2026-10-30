#include "timeline_layer_fields.hpp"

#include "asset_drop.hpp"
#include "composition_editor_support.hpp"
#include "properties_registry_row.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <map>
#include <tuple>

namespace bloom::ui::layer_fields {
namespace {

// Reads a document field as soon as LayerOutputBoundary declares it; until then `fallback`.
template <typename Layer> bool shyField(const Layer& layer, const bool fallback) {
    if constexpr (requires { layer.shy; })
        return layer.shy;
    else
        return fallback;
}
template <typename Layer> std::optional<bool> followsField(const Layer& layer) {
    if constexpr (requires { layer.nameFollowsSource; })
        return layer.nameFollowsSource;
    else
        return std::nullopt;
}

// Interim Shy store, used only while the document has no Shy field and command (handoff T2):
// session state, not saved and not undoable. setShy() moves to the document command when it lands.
using ShyKey = std::tuple<const CompositionSession*, std::uint64_t, std::uint64_t>;
std::map<ShyKey, bool>& interimShy() {
    static std::map<ShyKey, bool> store;
    return store;
}

const document::LayerOutputBoundary* layerRecord(const CompositionSession& session,
                                                 const document::LayerId layer) {
    const auto* composition = session.composition();
    return composition ? composition->graph().findLayer(layer) : nullptr;
}

const document::AssetRecord* sourceAsset(const CompositionSession& session,
                                         const document::NodeRecord& source) {
    for (const auto& binding : source.parameters) {
        if (binding.role != "asset")
            continue;
        const auto value = session.constantStringValue(binding.parameterId);
        return value ? session.snapshot().project().findAsset(
                           document::AssetId::fromRaw(value->toULongLong()))
                     : nullptr;
    }
    return nullptr;
}

[[nodiscard]] bool mediaSource(const document::NodeRecord& source) {
    return source.typeId == "bloom.image-source" || source.typeId == "bloom.video-source" ||
           source.typeId == document::kAudioSourceNodeType;
}

} // namespace

bool shy(const CompositionSession& session, const document::LayerId layer) {
    const auto* record = layerRecord(session, layer);
    if (record == nullptr)
        return false;
    const auto found =
        interimShy().find({&session, session.compositionId().value(), layer.value()});
    return shyField(*record, found != interimShy().end() && found->second);
}

bool setShy(CompositionSession& session, const std::vector<document::LayerId>& layers,
            const bool value) {
    bool changed = false;
    for (const auto layer : layers) {
        if (layerRecord(session, layer) == nullptr || shy(session, layer) == value)
            continue;
        interimShy()[{&session, session.compositionId().value(), layer.value()}] = value;
        changed = true;
    }
    return changed;
}

QString sourceName(const CompositionSession& session, const document::LayerId layer) {
    const auto* composition = session.composition();
    const auto* source = directSourceNode(session, layer);
    if (composition == nullptr || source == nullptr)
        return {};
    if (source->typeId == document::kCompositionSourceNodeType) {
        const auto* nested = session.snapshot().project().findComposition(
            compositionSourceId(*composition, *source));
        return nested ? QString::fromStdString(nested->name()) : QString{};
    }
    if (!mediaSource(*source))
        return {};
    const auto* asset = sourceAsset(session, *source);
    return asset ? imageAssetDisplayName(*asset) : QString{};
}

bool nameFollowsSource(const CompositionSession& session, const document::LayerId layer) {
    const auto* record = layerRecord(session, layer);
    const auto* source = directSourceNode(session, layer);
    if (record == nullptr || source == nullptr)
        return false;
    if (const auto field = followsField(*record))
        return *field && !sourceName(session, layer).isEmpty();
    // Until the document carries the flag: a layer still holding the name its add-layer command
    // authored follows its source. That is the imported file name (or path, or a sequence's
    // pattern) for media, and the composition's current name for a nested composition.
    if (source->typeId == document::kCompositionSourceNodeType)
        return QString::fromStdString(record->name) == sourceName(session, layer);
    if (!mediaSource(*source))
        return false;
    const auto* asset = sourceAsset(session, *source);
    if (asset == nullptr)
        return false;
    const auto imported =
        asset->kind == document::AssetKind::Sequence
            ? asset->manifest.pattern
            : asset->locator.path.substr(asset->locator.path.find_last_of('/') + 1);
    return record->name == imported || record->name == asset->locator.path;
}

QString displayName(const CompositionSession& session, const document::LayerId layer) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {};
    if (nameFollowsSource(session, layer))
        if (auto name = sourceName(session, layer); !name.isEmpty())
            return name;
    return layerName(*composition, layer);
}

} // namespace bloom::ui::layer_fields
