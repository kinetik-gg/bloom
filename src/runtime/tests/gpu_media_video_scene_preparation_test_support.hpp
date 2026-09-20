#pragma once

// Real-video fixtures for the CPU GPU-scene media preparation tests. The only fixture files read
// here are the three canonical generated media-worker fixtures (numbered-h264.mp4,
// numbered-prores.mov, alpha-prores.mov) owned by
// apps/bloom-media-worker/ffmpeg_provider_fixtures.ipp and supplied by the caller. Files written
// by sibling tests into the same directory are not fixtures and are never consumed. Each asset is
// probed with the SAME media::video::VideoDecodeSession (and therefore the same qualified worker)
// the production evaluator uses, so the tests exercise genuine demux, decode and colour conversion
// rather than a fake media handle. The probe-to-AssetRecord mapping mirrors
// commands::ImportAssets.

#include "gpu_scene_preparation_test_support.hpp"

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/media/video/session.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace document = bloom::document;
namespace provider = bloom::media::provider;

[[nodiscard]] document::AssetRecord videoAssetFromProbe(const std::filesystem::path& path,
                                                        const provider::ProbeResult& probe,
                                                        const std::uint64_t rawId,
                                                        const std::string& name) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(rawId);
    asset.kind = document::AssetKind::Video;
    asset.contentDigest = probe.sourceDigest;
    for (const auto& stream : probe.streams) {
        const auto timebase =
            RationalTime::create(stream.timebase.numerator, stream.timebase.denominator);
        const auto period = RationalTime::create(stream.rate.denominator, stream.rate.numerator);
        const auto duration =
            RationalTime::create(stream.duration.numerator, stream.duration.denominator);
        if (!timebase || !period || !duration) {
            throw std::logic_error("video fixture timing is invalid");
        }
        if (stream.kind == provider::MediaKind::Video && asset.width == 0) {
            asset.width = stream.width;
            asset.height = stream.height;
            asset.duration = *duration;
            if (stream.rate.numerator > std::numeric_limits<std::uint32_t>::max() ||
                stream.rate.denominator > std::numeric_limits<std::uint32_t>::max()) {
                throw std::logic_error("video fixture rate exceeds the supported range");
            }
            const auto mapping = bloom::core::FrameTimeMapping::create(
                *duration, static_cast<std::uint32_t>(stream.rate.numerator),
                static_cast<std::uint32_t>(stream.rate.denominator));
            if (!mapping ||
                mapping.value()->maximumFrameIndex() >= provider::Limits::indexEntries ||
                stream.frameCount > provider::Limits::indexEntries) {
                throw std::logic_error("video fixture duration is unavailable");
            }
            asset.frames =
                stream.frameCount ? stream.frameCount : mapping.value()->maximumFrameIndex() + 1;
        }
        if (stream.kind == provider::MediaKind::Audio && asset.channels == 0) {
            asset.rate = stream.sampleRate;
            asset.channels = static_cast<std::uint32_t>(stream.channelLayout.size());
        }
        asset.videoStreams.push_back(document::AssetVideoStream{
            stream.id, static_cast<std::uint32_t>(stream.kind), stream.codec, stream.profile,
            stream.pixelFormat, stream.timecode, *timebase, *period, *duration, stream.width,
            stream.height, stream.sampleRate, stream.colour.primaries, stream.colour.transfer,
            stream.colour.matrix, stream.colour.range, stream.channelLayout});
    }
    if (asset.width == 0) {
        throw std::logic_error("video fixture has no video stream");
    }
    asset.locator.kind = "file";
    asset.locator.portability = "project-relative";
    asset.locator.path = path.filename().string();
    asset.locator.relinkHint = "file:" + path.string();
    asset.interpretation.colorSpace = document::AssetColorSpace::Auto;
    asset.name = name;
    return asset;
}

[[nodiscard]] document::AssetRecord videoAsset(const std::filesystem::path& path,
                                               const std::uint64_t rawId) {
    bloom::media::video::VideoDecodeSession session(path);
    auto probed = session.probe();
    const auto* probe = std::get_if<provider::ProbeResult>(&probed);
    if (probe == nullptr) {
        const auto* error = std::get_if<provider::Unavailable>(&probed);
        throw std::logic_error("video fixture probe failed: " +
                               (error != nullptr ? error->detail : std::string{"unknown error"}));
    }
    return videoAssetFromProbe(path, *probe, rawId, path.stem().string());
}

// A single VideoSource -> translation-only Layer Output -> Normal Merge -> Composition Output
// graph.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
videoPlan(const CompositionFormat compositionFormat, const document::AssetRecord& asset,
          const LayerValues layer, const std::uint64_t idBase, const std::int64_t startFrame = 0,
          const std::int64_t loopMode = 0) {
    const LayerIds ids{
        document::ParameterId::fromRaw(idBase + 0), document::ParameterId::fromRaw(idBase + 1),
        document::ParameterId::fromRaw(idBase + 2), document::ParameterId::fromRaw(idBase + 3),
        document::ParameterId::fromRaw(idBase + 4), document::ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(bloom::runtime::CompiledVideoSource{
        document::NodeId::fromRaw(idBase + 10), asset, startFrame, loopMode, 0, std::string{}});
    operations.emplace_back(layerOutput(document::NodeId::fromRaw(idBase + 11),
                                        document::LayerId::fromRaw(idBase + 12),
                                        OperationIndex::fromRaw(0), ids, layer));
    operations.emplace_back(
        CompiledMerge{document::NodeId::fromRaw(idBase + 13),
                      std::vector<CompiledMergeInput>{CompiledMergeInput{
                          document::LayerSlotId::fromRaw(idBase + 14),
                          document::LayerId::fromRaw(idBase + 12), OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{document::NodeId::fromRaw(idBase + 15),
                                                      OperationIndex::fromRaw(2)});
    return publish(CompiledCompositionPlanDefinition{
        document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

} // namespace
