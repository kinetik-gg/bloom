#include <algorithm>
#include <array>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/core/file_uri.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/image.hpp>

#include <bloom/media/video/session.hpp>
#include <bloom/platform/source_identity.hpp>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace bloom::commands {
namespace {
media::provider::Result<document::AssetRecord>
assetMetadata(const media::provider::ProbeResult& probe) {
    using namespace media::provider;
    if (!valid(probe))
        return Unavailable{Error::InvalidValue, "Invalid video metadata"};
    document::AssetRecord asset;
    asset.kind = document::AssetKind::Video;
    asset.contentDigest = probe.sourceDigest;
    for (const auto& stream : probe.streams) {
        const auto timebase =
            core::RationalTime::create(stream.timebase.numerator, stream.timebase.denominator);
        const auto period =
            core::RationalTime::create(stream.rate.denominator, stream.rate.numerator);
        const auto duration =
            core::RationalTime::create(stream.duration.numerator, stream.duration.denominator);
        if (!timebase || !period || !duration)
            return Unavailable{Error::InvalidValue, "Invalid video timing"};
        if (stream.kind == MediaKind::Video && asset.width == 0) {
            asset.width = stream.width;
            asset.height = stream.height;
            asset.duration = *duration;
            if (stream.rate.numerator > std::numeric_limits<std::uint32_t>::max() ||
                stream.rate.denominator > std::numeric_limits<std::uint32_t>::max())
                return Unavailable{Error::Unavailable, "Video rate exceeds the supported range"};
            const auto mapping = core::FrameTimeMapping::create(
                *duration, static_cast<std::uint32_t>(stream.rate.numerator),
                static_cast<std::uint32_t>(stream.rate.denominator));
            if (!mapping || mapping.value()->maximumFrameIndex() >= Limits::indexEntries ||
                stream.frameCount > Limits::indexEntries)
                return Unavailable{Error::Unavailable,
                                   "Video duration is unavailable or exceeds the frame limit"};
            asset.frames =
                stream.frameCount ? stream.frameCount : mapping.value()->maximumFrameIndex() + 1;
        }
        if (stream.kind == MediaKind::Audio && asset.channels == 0) {
            asset.rate = stream.sampleRate;
            asset.channels = static_cast<std::uint32_t>(stream.channelLayout.size());
        }
        asset.videoStreams.push_back(
            {stream.id, static_cast<std::uint32_t>(stream.kind), stream.codec, stream.profile,
             stream.pixelFormat, stream.timecode, *timebase, *period, *duration, stream.width,
             stream.height, stream.sampleRate, stream.colour.primaries, stream.colour.transfer,
             stream.colour.matrix, stream.colour.range, stream.fieldOrder,
             document::timecodeTypeFor(stream.timecode), stream.channelLayout});
    }
    if (asset.width == 0)
        return Unavailable{Error::Unavailable, "The file has no video stream"};
    return asset;
}

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
document::AssetLocator locator(const std::filesystem::path& path,
                               const std::filesystem::path& base) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto relative =
        absolute.lexically_relative(std::filesystem::absolute(base).lexically_normal());
    const auto uri = core::fileUriFromAbsolutePath(absolute);
    if (!uri || uri->size() > 16384)
        throw std::runtime_error("The asset path cannot be represented as an absolute file URI");
    const auto relativeText = utf8(relative);
    if (relative.empty() || relativeText.size() > 4096 ||
        relativeText.find_first_of(":\\") != std::string::npos)
        return {"file", "external", *uri, *uri};
    return {"file", "project-relative", relativeText, *uri};
}
media::audio::AudioResult<core::Sha256Digest> digestFile(const std::filesystem::path& path,
                                                         const std::function<bool()>& cancel) {
    const auto cancelled = [&] {
        if (!cancel)
            return false;
        try {
            return cancel();
        } catch (...) {
            return true;
        }
    };
    // Hashed once here; the recorded identity lets later verification skip re-reading the file.
    const auto hashed = platform::hashSourceFile(path, cancelled);
    if (hashed.error == platform::SourceHashError::Cancelled)
        return media::audio::AudioResult<core::Sha256Digest>::failure(
            media::audio::AudioError::codeOnly(media::audio::AudioErrorCode::Cancelled));
    if (hashed.error != platform::SourceHashError::None)
        return media::audio::AudioResult<core::Sha256Digest>::failure(
            media::audio::AudioError::codeOnly(media::audio::AudioErrorCode::FileReadFailed));
    return media::audio::AudioResult<core::Sha256Digest>::success(hashed.digest);
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
    // One failing file never aborts the batch: it is recorded with its own diagnostic and the
    // remaining files import. Only cancellation or an unusable request refuses the whole batch.
    const auto failFile = [this](const std::filesystem::path& path, std::string diagnostic,
                                 std::optional<media::provider::Unavailable> media = {}) {
        failures_.push_back({path, std::move(diagnostic), std::move(media)});
    };
    const auto cancelImport = [this] {
        diagnostic_ = "Image import cancelled";
        assets_.clear();
    };
    if (paths.size() > 100000) {
        diagnostic_ = "Too many files selected";
        return;
    }
    std::set<std::filesystem::path> admitted;
    for (const auto& path : paths) {
        try {
            if (admitted.contains(std::filesystem::absolute(path).lexically_normal()))
                continue;
            if (cancel && cancel()) {
                cancelImport();
                return;
            }
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (color::isLutExtension(path)) {
                const auto lut = color::readLutFile(path, cancel);
                if (lut.error != color::LutError::None) {
                    failFile(path,
                             "LUT import refused: " + std::string(color::lutErrorName(lut.error)));
                    continue;
                }
                document::AssetRecord asset;
                asset.kind = document::AssetKind::Lut;
                asset.locator = locator(path, projectDirectory);
                asset.contentDigest = lut.digest;
                assets_.push_back(std::move(asset));
                admitted.insert(std::filesystem::absolute(path).lexically_normal());
                if (progress)
                    progress(assets_.size() + failures_.size(), paths.size());
                continue;
            }
            if (media::video::isVideoExtension(path)) {
                media::video::VideoDecodeSession session(path);
                const auto probed = session.probe(cancel);
                const auto* probe = std::get_if<media::provider::ProbeResult>(&probed);
                if (!probe) {
                    const auto& unavailable = std::get<media::provider::Unavailable>(probed);
                    if (unavailable.reason == media::provider::Error::Cancelled) {
                        cancelImport();
                        return;
                    }
                    failFile(path, unavailable.detail, unavailable);
                    continue;
                }
                auto metadata = assetMetadata(*probe);
                auto* asset = std::get_if<document::AssetRecord>(&metadata);
                if (!asset) {
                    const auto& unavailable = std::get<media::provider::Unavailable>(metadata);
                    if (unavailable.reason == media::provider::Error::Cancelled) {
                        cancelImport();
                        return;
                    }
                    failFile(path, unavailable.detail, unavailable);
                    continue;
                }
                asset->locator = locator(path, projectDirectory);
                assets_.push_back(std::move(*asset));
                admitted.insert(std::filesystem::absolute(path).lexically_normal());
                if (progress)
                    progress(assets_.size() + failures_.size(), paths.size());
                continue;
            }
            if (extension == ".wav" || extension == ".mp3") {
                const auto probe = media::audio::probeAudio(path, {}, cancel);
                if (!probe.value()) {
                    failFile(path, "Audio import failed: " +
                                       media::audio::describeAudioError(*probe.error()));
                    continue;
                }
                document::AssetRecord asset;
                asset.kind = document::AssetKind::Audio;
                asset.locator = locator(path, projectDirectory);
                const auto digest = digestFile(path, cancel);
                if (!digest.value()) {
                    failFile(path, "Audio import failed: " +
                                       media::audio::describeAudioError(*digest.error()));
                    continue;
                }
                asset.contentDigest = *digest.value();
                asset.rate = probe.value()->rate;
                asset.channels = probe.value()->channels;
                asset.frames = probe.value()->frames;
                asset.duration = probe.value()->duration;
                assets_.push_back(std::move(asset));
                admitted.insert(std::filesystem::absolute(path).lexically_normal());
                if (progress)
                    progress(assets_.size() + failures_.size(), paths.size());
                continue;
            }
            const auto probe = media::probeImage(path, cancel);
            if (!probe.value.has_value()) {
                if (probe.cancelled) {
                    cancelImport();
                    return;
                }
                failFile(path, probe.diagnostic);
                continue;
            }
            auto sequence = media::scanSequence(path, cancel, progress);
            if (!sequence.value.has_value()) {
                if (sequence.cancelled) {
                    cancelImport();
                    return;
                }
                failFile(path, sequence.diagnostic);
                continue;
            }
            document::AssetRecord asset;
            asset.locator = locator(path, projectDirectory);
            asset.contentDigest = probe.value->contentDigest;
            asset.width = probe.value->width;
            asset.height = probe.value->height;
            // New imports keep the legacy interpretation at Auto.  The probe's format,
            // bit-depth, and EXR chromaticities are metadata for the config-managed resolver;
            // copying its compatibility enum here would turn a fresh automatic import into an
            // authored legacy override and would bypass the EXR chromaticity rules.
            asset.interpretation.colorSpace = document::AssetColorSpace::Auto;
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
                bool digested = true;
                for (const auto& member : scan.members) {
                    asset.manifest.members.push_back({member.frame,
                                                      locator(member.path, projectDirectory),
                                                      member.contentDigest});
                    const auto frame = std::to_string(member.frame) + ":";
                    const auto digest = member.contentDigest.toLowercaseHex();
                    if (!hasher.update(std::as_bytes(std::span(frame))) ||
                        !hasher.update(std::as_bytes(std::span(digest)))) {
                        digested = false;
                        break;
                    }
                }
                if (!digested) {
                    failFile(path, "Sequence digest failed");
                    continue;
                }
                for (const auto& member : scan.members)
                    admitted.insert(std::filesystem::absolute(member.path).lexically_normal());
                asset.contentDigest = hasher.finalize();
                asset.locator = asset.manifest.members.front().locator;
            } else
                admitted.insert(std::filesystem::absolute(path).lexically_normal());
            assets_.push_back(std::move(asset));
            if (progress)
                progress(assets_.size() + failures_.size(), paths.size());
        } catch (const std::exception& error) {
            failFile(path, error.what());
        }
    }
    // A cancelled batch never commits a partial result.
    if (!assets_.empty() && cancel && cancel()) {
        cancelImport();
        return;
    }
    if (assets_.empty() && !failures_.empty()) {
        // Nothing importable: the batch is refused with the (first) per-file reason.
        mediaDiagnostic_ = failures_.front().media;
        diagnostic_ = failures_.size() == 1 ? failures_.front().diagnostic
                                            : std::to_string(failures_.size()) +
                                                  " files could not be imported; first: " +
                                                  failures_.front().path.filename().string() +
                                                  ": " + failures_.front().diagnostic;
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
    auto result = outputs.empty() ? OperationResult::noChange()
                                  : OperationResult::applied(std::move(outputs));
    // The successful files commit together; each skipped file stays reported individually.
    for (const auto& failure : failures_)
        result.issues.push_back({OperationIssueCode::InvalidValue,
                                 failure.path.filename().string() + ": " + failure.diagnostic});
    return result;
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

OperationResult SetCompositionWorkingColorSpace::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition does not exist");
    if (colorSpaceId_.has_value() && !document::validateWorkingColorSpaceId(*colorSpaceId_).ok())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid working colour space id");
    if (composition->workingColorSpaceId() == colorSpaceId_)
        return OperationResult::noChange();
    composition->setWorkingColorSpaceId(colorSpaceId_);
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
    // ADR 0024: the audio is the layer's content extent and the range ends with it (capped by the
    // composition), so after its last sample the layer is silent.
    if (auto* boundary = graph.findLayer(*layerId); boundary != nullptr) {
        boundary->timing.contentExtent = document::SourceLayerExtent{*sourceNodeId};
        const auto rate = composition->format().frameRate();
        if (asset->duration > core::RationalTime{} && asset->duration < composition->duration()) {
            const auto frames = (asset->duration.numerator() * rate.numerator() +
                                 asset->duration.denominator() * rate.denominator() - 1) /
                                (asset->duration.denominator() * rate.denominator());
            if (const auto end = core::RationalTime::create(
                    frames * static_cast<std::int64_t>(rate.denominator()), rate.numerator());
                end && *end > core::RationalTime{} && *end < composition->duration())
                boundary->outPoint = *end;
        }
    }

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
