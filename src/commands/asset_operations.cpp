#include <algorithm>
#include <array>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/image.hpp>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace bloom::commands {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string fileUri(const std::filesystem::path& path) {
    const auto text = utf8(path);
    std::string uri = text.starts_with("/") ? "file://" : "file:///";
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || character == '/' || character == ':' ||
            character == '-' || character == '_' || character == '.' || character == '~')
            uri += character;
        else {
            uri += '%';
            uri += hex[byte >> 4U];
            uri += hex[byte & 15U];
        }
    }
    return uri;
}
// document::AssetRecord::validate() (src/document/asset.cpp's validLocator()) forbids a literal
// ':' or '\\' anywhere in a "project-relative" locator path -- a portability guard against a
// Windows drive-letter/separator colliding with a path this schema promises is a plain relative
// join. A GVFS network-share mount is named literally with a colon on this platform (e.g.
// `smb-share:server=10.10.10.20,share=production`), which a purely lexical relative path can't
// avoid when the source lives outside the project directory (NETSHARE-1, deliverable 4). Percent-
// encoding just those two characters keeps the stored path schema-valid without changing the
// schema itself: on resolve, media::resolveImagePath()'s project-relative candidate then simply
// doesn't exist on disk (no directory is literally named with a "%3A") and falls through to the
// always-correct absolute relinkHint below -- the same fallback every other moved/foreign source
// already relies on. Ordinary imports (no ':' or '\\' anywhere in the relative path) are
// byte-for-byte unaffected.
std::string sanitizeRelativePathComponents(const std::string& raw) {
    std::string encoded;
    encoded.reserve(raw.size());
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (const char character : raw) {
        if (character == ':' || character == '\\') {
            encoded += '%';
            encoded += hex[static_cast<unsigned char>(character) >> 4U];
            encoded += hex[static_cast<unsigned char>(character) & 15U];
        } else
            encoded += character;
    }
    return encoded;
}
document::AssetLocator locator(const std::filesystem::path& path,
                               const std::filesystem::path& base) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto relative =
        absolute.lexically_relative(std::filesystem::absolute(base).lexically_normal());
    if (relative.empty())
        throw std::runtime_error("Image and project must share a filesystem root");
    return {"file", "project-relative", sanitizeRelativePathComponents(utf8(relative)),
            fileUri(absolute)};
}
std::optional<core::Sha256Digest> digestFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    core::Sha256Hasher hasher;
    std::array<std::byte, static_cast<std::size_t>(64) * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && !hasher.update(std::span<const std::byte>{buffer}.first(
                             static_cast<std::size_t>(count))))
            return std::nullopt;
    }
    return hasher.finalize();
}
} // namespace
OperationResult RelinkFontAsset::apply(document::Draft& draft) const {
    auto* target = draft.project().findAsset(id_);
    if (target == nullptr || target->kind != document::AssetKind::Font)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Target is not a Font asset");
    auto replacement = replacement_;
    replacement.id = id_;
    replacement.name = target->name;
    replacement.folder = target->folder;
    replacement.tags = target->tags;
    replacement.order = target->order;
    if (!replacement.validate().ok())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid relinked font face");
    *target = std::move(replacement);
    return OperationResult::applied({{"asset", id_}});
}

OperationResult EnsureFontAsset::apply(document::Draft& draft) const {
    const auto existing =
        std::ranges::find_if(draft.project().assets(), [&](const auto& candidate) {
            return candidate.kind == document::AssetKind::Font &&
                   candidate.contentDigest == asset_.contentDigest &&
                   candidate.fontFamily == asset_.fontFamily &&
                   candidate.fontStyle == asset_.fontStyle;
        });
    if (existing != draft.project().assets().end())
        return OperationResult::applied({{"asset", existing->id}});
    const auto id = draft.ids().allocateAsset();
    if (!id)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Asset ID space is exhausted");
    auto asset = asset_;
    asset.id = *id;
    if (asset.name.empty())
        asset.name = document::defaultAssetName(asset);
    for (const auto& existingAsset : draft.project().assets())
        if (!existingAsset.folder)
            asset.order = std::max(asset.order,
                                   (existingAsset.order == std::numeric_limits<std::uint64_t>::max()
                                        ? existingAsset.order
                                        : existingAsset.order + 1));
    if (!asset.validate().ok())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid font face reference");
    if (!draft.project().addAsset(std::move(asset)))
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Invalid font asset");
    return OperationResult::applied({{"asset", *id}});
}

ImportAssets::ImportAssets(const std::vector<std::filesystem::path>& paths,
                           const std::filesystem::path& projectDirectory,
                           const std::function<bool()>& cancel,
                           const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    try {
        if (paths.size() > 100000) {
            diagnostic_ = "Too many files selected";
            return;
        }
        std::set<std::filesystem::path> admitted;
        for (const auto& path : paths) {
            if (admitted.contains(std::filesystem::absolute(path).lexically_normal()))
                continue;
            if (cancel && cancel()) {
                diagnostic_ = "Image import cancelled";
                assets_.clear();
                return;
            }
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (extension == ".wav" || extension == ".mp3") {
                const auto probe = media::audio::probeAudio(path);
                if (!probe.value()) {
                    diagnostic_ = "Audio import failed";
                    assets_.clear();
                    return;
                }
                document::AssetRecord asset;
                asset.kind = document::AssetKind::Audio;
                asset.locator = locator(path, projectDirectory);
                const auto digest = digestFile(path);
                if (!digest.has_value()) {
                    diagnostic_ = "Audio digest failed";
                    assets_.clear();
                    return;
                }
                asset.contentDigest = *digest;
                asset.rate = probe.value()->rate;
                asset.channels = probe.value()->channels;
                asset.frames = probe.value()->frames;
                asset.duration = probe.value()->duration;
                assets_.push_back(std::move(asset));
                admitted.insert(std::filesystem::absolute(path).lexically_normal());
                if (progress)
                    progress(assets_.size(), paths.size());
                continue;
            }
            const auto probe = media::probeImage(path, cancel);
            if (!probe.value.has_value()) {
                diagnostic_ = probe.cancelled ? "Image import cancelled" : probe.diagnostic;
                assets_.clear();
                return;
            }
            auto sequence = media::scanSequence(path, cancel, progress);
            if (!sequence.value.has_value()) {
                diagnostic_ = sequence.cancelled ? "Image import cancelled" : sequence.diagnostic;
                assets_.clear();
                return;
            }
            document::AssetRecord asset;
            asset.locator = locator(path, projectDirectory);
            asset.contentDigest = probe.value->contentDigest;
            asset.width = probe.value->width;
            asset.height = probe.value->height;
            asset.interpretation.colorSpace =
                probe.value->colorSpace == media::ImageColorSpace::Srgb
                    ? document::AssetColorSpace::Srgb
                : probe.value->colorSpace == media::ImageColorSpace::Linear
                    ? document::AssetColorSpace::Linear
                : probe.value->colorSpace == media::ImageColorSpace::Raw
                    ? document::AssetColorSpace::Raw
                    : document::AssetColorSpace::Auto;
            asset.interpretation.alphaAssociation =
                probe.value->alphaAssociation == media::ImageAlphaAssociation::Premultiplied
                    ? document::AssetAlphaAssociation::Premultiplied
                    : document::AssetAlphaAssociation::Straight;
            if (sequence.value->members.size() > 1) {
                asset.kind = document::AssetKind::Sequence;
                const auto& scan = *sequence.value;
                asset.manifest.pattern = scan.pattern;
                asset.manifest.padding = scan.padding;
                asset.manifest.first = scan.first;
                asset.manifest.last = scan.last;
                asset.manifest.gaps = scan.gaps;
                core::Sha256Hasher hasher;
                for (const auto& member : scan.members) {
                    asset.manifest.members.push_back({member.frame,
                                                      locator(member.path, projectDirectory),
                                                      member.contentDigest});
                    const auto frame = std::to_string(member.frame) + ":";
                    const auto digest = member.contentDigest.toLowercaseHex();
                    if (!hasher.update(std::as_bytes(std::span(frame))) ||
                        !hasher.update(std::as_bytes(std::span(digest)))) {
                        diagnostic_ = "Sequence digest failed";
                        assets_.clear();
                        return;
                    }
                    admitted.insert(std::filesystem::absolute(member.path).lexically_normal());
                }
                asset.contentDigest = hasher.finalize();
                asset.locator = asset.manifest.members.front().locator;
            } else
                admitted.insert(std::filesystem::absolute(path).lexically_normal());
            assets_.push_back(std::move(asset));
            if (progress)
                progress(assets_.size(), paths.size());
        }
    } catch (const std::exception& error) {
        diagnostic_ = error.what();
        assets_.clear();
    }
}
OperationResult ImportAssets::apply(document::Draft& draft) const {
    if (!diagnostic_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, diagnostic_);
    std::vector<OperationOutput> outputs;
    for (auto asset : assets_) {
        const auto existing =
            std::ranges::find_if(draft.project().assets(), [&](const auto& candidate) {
                return candidate.locator.path == asset.locator.path &&
                       candidate.contentDigest == asset.contentDigest;
            });
        if (existing != draft.project().assets().end())
            continue;
        const auto id = draft.ids().allocateAsset();
        if (!id)
            return OperationResult::rejected(OperationIssueCode::Unsupported,
                                             "Asset ID space is exhausted");
        asset.id = *id;
        asset.name = document::defaultAssetName(asset);
        for (const auto& existingAsset : draft.project().assets())
            if (!existingAsset.folder)
                asset.order = std::max(
                    asset.order, (existingAsset.order == std::numeric_limits<std::uint64_t>::max()
                                      ? existingAsset.order
                                      : existingAsset.order + 1));
        if (!draft.project().addAsset(std::move(asset)))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Invalid imported asset");
        outputs.push_back({"asset", *id});
    }
    return outputs.empty() ? OperationResult::noChange()
                           : OperationResult::applied(std::move(outputs));
}
RelinkAsset::RelinkAsset(document::AssetId id, const std::filesystem::path& path,
                         const std::filesystem::path& base, const std::function<bool()>& cancel)
    : id_(id), prepared_({path}, base, cancel) {}
OperationResult RelinkAsset::apply(document::Draft& draft) const {
    auto* target = draft.project().findAsset(id_);
    if (!target)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget, "Asset does not exist");
    if (!prepared_.diagnostic().empty() || prepared_.prepared().size() != 1)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, prepared_.diagnostic());
    auto replacement = prepared_.prepared().front();
    replacement.id = id_;
    replacement.name = target->name;
    replacement.folder = target->folder;
    replacement.tags = target->tags;
    replacement.order = target->order;
    replacement.interpretation = target->interpretation;
    *target = std::move(replacement);
    return OperationResult::applied({{"asset", id_}});
}
OperationResult RemoveAsset::apply(document::Draft& draft) const {
    if (!draft.project().removeAsset(id_))
        return OperationResult::noChange();
    return OperationResult::applied();
}
OperationResult SetCompositionBackgroundColor::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition does not exist");
    if (!color_.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid background colour");
    if (composition->backgroundColor() == color_)
        return OperationResult::noChange();
    composition->setBackgroundColor(color_);
    return OperationResult::applied();
}

OperationResult AddAudioLayer::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    const auto* asset = draft.project().findAsset(asset_);
    if (composition == nullptr || asset == nullptr || asset->kind != document::AssetKind::Audio)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Audio asset or composition does not exist");

    const auto sourceNodeId = draft.ids().allocateNode();
    const auto layerOutputNodeId = draft.ids().allocateNode();
    const auto sourceToLayerEdgeId = draft.ids().allocateEdge();
    const auto layerToStackEdgeId = draft.ids().allocateEdge();
    const auto layerId = draft.ids().allocateLayer();
    const auto slotId = draft.ids().allocateLayerSlot();
    std::array<document::ParameterId, 9> parameterIds{};
    bool parametersAllocated = true;
    for (auto& parameterId : parameterIds) {
        const auto allocated = draft.ids().allocateParameter();
        if (!allocated) {
            parametersAllocated = false;
            break;
        }
        parameterId = *allocated;
    }
    if (!sourceNodeId || !layerOutputNodeId || !sourceToLayerEdgeId || !layerToStackEdgeId ||
        !layerId || !slotId || !parametersAllocated)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Audio layer ID space is exhausted");

    auto& parameters = composition->parameters();
    const auto insert = [&](const document::ParameterId id, const std::string& key,
                            const document::ParameterValue& value) {
        return parameters.insert({id, key, document::ConstantValueSource{value}});
    };
    if (!insert(parameterIds[0], "bloom.audio.asset", std::to_string(asset_.value())) ||
        !insert(parameterIds[1], "bloom.audio.start-frame", std::int64_t{0}) ||
        !insert(parameterIds[2], std::string(document::kAudioLevelParameterSchemaKey), 1.0) ||
        !insert(parameterIds[3], std::string(document::kPositionParameterSchemaKey),
                document::Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                                static_cast<double>(composition->format().height()) / 2.0}) ||
        !insert(parameterIds[4], std::string(document::kAnchorParameterSchemaKey),
                document::kDefaultAnchor) ||
        !insert(parameterIds[5], std::string(document::kScaleParameterSchemaKey),
                document::kDefaultScale) ||
        !insert(parameterIds[6], std::string(document::kRotationParameterSchemaKey),
                document::kDefaultRotationDegrees) ||
        !insert(parameterIds[7], std::string(document::kOpacityParameterSchemaKey), 1.0) ||
        !insert(parameterIds[8], std::string(document::kBlendModeParameterSchemaKey),
                document::kDefaultBlendModeValue))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Audio layer parameters could not be inserted");

    auto& graph = composition->graph();
    if (!graph.addNode({*sourceNodeId,
                        std::string(document::kAudioSourceNodeType),
                        {{"asset", parameterIds[0]},
                         {"startFrame", parameterIds[1]},
                         {"level", parameterIds[2]}},
                        document::kAudioSourceNodeSchemaVersion}) ||
        !graph.addNode({*layerOutputNodeId,
                        std::string(document::kLayerOutputNodeType),
                        {{std::string(document::kPositionParameterRole), parameterIds[3]},
                         {std::string(document::kAnchorParameterRole), parameterIds[4]},
                         {std::string(document::kScaleParameterRole), parameterIds[5]},
                         {std::string(document::kRotationParameterRole), parameterIds[6]},
                         {std::string(document::kOpacityParameterRole), parameterIds[7]},
                         {std::string(document::kBlendModeParameterRole), parameterIds[8]}},
                        document::kLayerOutputNodeSchemaVersion}) ||
        !graph.addLayerOutput({*layerOutputNodeId,
                               *layerId,
                               asset->locator.path,
                               std::string(document::kLayerOutputAudioOutputPort),
                               std::nullopt,
                               true,
                               false,
                               false,
                               {},
                               {}}) ||
        !graph.layerStack().append({*slotId, *layerId}) ||
        !graph.addEdge(
            {*sourceToLayerEdgeId,
             {*sourceNodeId, std::string(document::kAudioSourceOutputPort)},
             document::NodeInputRef{*layerOutputNodeId,
                                    std::string(document::kLayerOutputAudioInputPort)}}) ||
        !graph.addEdge(
            {*layerToStackEdgeId,
             {*layerOutputNodeId, std::string(document::kLayerOutputAudioOutputPort)},
             document::LayerStackInputRef{graph.layerStack().nodeId(), *slotId,
                                          std::string(document::kLayerStackAudioInputRole)}}))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Audio layer topology could not be inserted");

    const auto output = graph.compositionOutput();
    if (output.has_value() && std::ranges::none_of(graph.edges(), [&](const auto& edge) {
            const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            return input != nullptr && input->nodeId == output->nodeId &&
                   input->port == document::kCompositionOutputAudioInputPort;
        })) {
        const auto outputEdgeId = draft.ids().allocateEdge();
        if (!outputEdgeId ||
            !graph.addEdge(
                {*outputEdgeId,
                 {graph.layerStack().nodeId(), std::string(document::kLayerStackAudioOutputPort)},
                 document::NodeInputRef{output->nodeId,
                                        std::string(document::kCompositionOutputAudioInputPort)}}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Audio output could not be connected");
    }
    const auto card = document::kConservativeNodeCardSize;
    const auto defaults = document::defaultNodeLayout(graph.nodes());
    const auto mergeId = graph.layerStack().nodeId();
    const auto mergePosition = composition->nodeLayout().contains(mergeId)
                                   ? composition->nodeLayout().at(mergeId).position
                                   : defaults.at(mergeId).position;
    const auto gap = document::kNodeLayoutSpacing;
    const auto layerPreferred =
        document::Vec2d{mergePosition.x - card.width - gap, mergePosition.y};
    auto occupied = composition->nodeLayout();
    for (const auto& existing : graph.nodes())
        if (existing.id != *sourceNodeId && existing.id != *layerOutputNodeId &&
            !occupied.contains(existing.id))
            occupied.emplace(existing.id, defaults.at(existing.id));
    const auto layerPosition =
        document::findNearestFreeNodePosition(occupied, {}, card, layerPreferred, gap);
    occupied[*layerOutputNodeId] = {layerPosition, 128.0, false, false};
    const auto sourcePreferred =
        document::Vec2d{layerPosition.x - card.width - gap, layerPosition.y};
    const auto sourcePosition =
        document::findNearestFreeNodePosition(occupied, {}, card, sourcePreferred, gap);
    composition->nodeLayout()[*sourceNodeId] = {sourcePosition, 128.0, false, false};
    composition->nodeLayout()[*layerOutputNodeId] = {layerPosition, 128.0, false, false};
    return OperationResult::applied({{"layer", *layerId},
                                     {"slot", *slotId},
                                     {"audioNode", *sourceNodeId},
                                     {"layerOutputNode", *layerOutputNodeId}});
}
} // namespace bloom::commands
