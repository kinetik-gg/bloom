#pragma once

// Real video fixture for the bounded GPU coverage gate. It probes a canonical generated
// media-worker fixture with the same production VideoDecodeSession the evaluator uses and builds a
// genuine CompiledVideoSource plan. The worker path comes from the build's BLOOM_VIDEO_WORKER
// define (set by apps/bloom-media-worker), so no fake media handle is used. When the generated
// fixtures are not supplied the fixture reports an explicit typed test-environment-missing result
// rather than passing.

#include "gpu_coverage_fixture_support.hpp"

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/media/video/session.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace bloom::gpu_coverage_video {

namespace document = bloom::document;
namespace provider = bloom::media::provider;

using bloom::gpu_coverage_fixtures::format;
using bloom::gpu_coverage_fixtures::LayerIds;
using bloom::gpu_coverage_fixtures::layerOutput;
using bloom::gpu_coverage_fixtures::LayerValues;
using bloom::gpu_coverage_fixtures::publish;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::OperationIndex;

// Set from the test's --media-fixtures argument; empty means the environment is not supplied.
inline std::filesystem::path& mediaFixturesDirectory() {
    static std::filesystem::path directory;
    return directory;
}

[[nodiscard]] inline std::string fixtureName() { return "numbered-h264.mp4"; }

struct VideoFixture final {
    std::filesystem::path path;
    document::AssetRecord asset;
};

[[nodiscard]] inline document::AssetRecord videoAssetFromProbe(const std::filesystem::path& path,
                                                               const provider::ProbeResult& probe,
                                                               const std::uint64_t rawId,
                                                               const std::string& name) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(rawId);
    asset.kind = document::AssetKind::Video;
    asset.contentDigest = probe.sourceDigest;
    for (const auto& stream : probe.streams) {
        const auto timebase =
            bloom::core::RationalTime::create(stream.timebase.numerator, stream.timebase.denominator);
        const auto period =
            bloom::core::RationalTime::create(stream.rate.denominator, stream.rate.numerator);
        const auto duration =
            bloom::core::RationalTime::create(stream.duration.numerator, stream.duration.denominator);
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

[[nodiscard]] inline VideoFixture probeVideoFixture() {
    VideoFixture fixture;
    fixture.path = mediaFixturesDirectory() / fixtureName();
    if (mediaFixturesDirectory().empty() || !std::filesystem::is_regular_file(fixture.path)) {
        throw std::runtime_error("test-environment-missing: media fixtures not supplied");
    }
    bloom::media::video::VideoDecodeSession session(fixture.path);
    auto probed = session.probe();
    const auto* probe = std::get_if<provider::ProbeResult>(&probed);
    if (probe == nullptr) {
        const auto* error = std::get_if<provider::Unavailable>(&probed);
        throw std::runtime_error("codec-unavailable: " +
                                 (error != nullptr ? error->detail : std::string{"unknown error"}));
    }
    fixture.asset = videoAssetFromProbe(fixture.path, *probe, 901, fixture.path.stem().string());
    return fixture;
}

// VideoSource -> translation-only Layer Output -> Normal Merge -> Composition Output at a given
// composition time. Video always takes the raster translation path.
[[nodiscard]] inline std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
videoPlan(const document::AssetRecord& asset, const LayerValues layer, const std::int64_t startFrame,
          const std::uint64_t idBase) {
    using bloom::document::LayerId;
    using bloom::document::LayerSlotId;
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    const LayerIds ids{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                       ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                       ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(bloom::runtime::CompiledVideoSource{
        NodeId::fromRaw(idBase + 10), asset, startFrame, 0, 0, std::string{}});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 11), LayerId::fromRaw(idBase + 12),
                                        OperationIndex::fromRaw(0), ids, layer));
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{
            LayerSlotId::fromRaw(idBase + 14), LayerId::fromRaw(idBase + 12),
            OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), OperationIndex::fromRaw(2)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), bloom::document::ProjectId::fromRaw(1),
        bloom::document::CompositionId::fromRaw(2), format(8, 8), std::move(operations),
        OperationIndex::fromRaw(3)});
}

} // namespace bloom::gpu_coverage_video
