#pragma once

// Real still-image fixture for the bounded GPU coverage gate. It writes a genuine synthetic
// signed/HDR EXR to the gate work directory and resolves it through the production image probe and
// the production image-source lowering, exactly as the media scene-preparation suite does. The
// oracle and the preparer read the same bytes, time, and interpretation; there is no fake
// admission and no InvalidMediaAccepted path.

#include "gpu_coverage_fixture_support.hpp"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bloom::gpu_coverage_media {

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

struct ExrPixel final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

[[nodiscard]] inline const std::filesystem::path& gateDirectory() {
    static const std::filesystem::path directory{"/tmp/opencode/gpu-contract/media"};
    return directory;
}

inline void writeExrRgba(const std::filesystem::path& path, const int width, const int height,
                         const std::vector<ExrPixel>& pixels) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(width - 1, height - 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));

    std::vector<float> red(pixels.size()), green(pixels.size()), blue(pixels.size()),
        alpha(pixels.size());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        red[index] = pixels[index].red;
        green[index] = pixels[index].green;
        blue[index] = pixels[index].blue;
        alpha[index] = pixels[index].alpha;
    }
    Imf::FrameBuffer frameBuffer;
    const auto stride = static_cast<std::size_t>(width) * sizeof(float);
    frameBuffer.insert(
        "R", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(red.data()), sizeof(float), stride));
    frameBuffer.insert(
        "G", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(green.data()), sizeof(float), stride));
    frameBuffer.insert(
        "B", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(blue.data()), sizeof(float), stride));
    frameBuffer.insert(
        "A", Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(alpha.data()), sizeof(float), stride));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
}

struct ImageFixture final {
    std::filesystem::path path;
    bloom::document::AssetRecord asset;
};

// A 3x2 signed/HDR RGBA EXR: smaller than the composition, with a negative channel, values above 1,
// and a non-opaque alpha, written premultiplied.
[[nodiscard]] inline std::vector<ExrPixel> signedHdrPixels() {
    return {ExrPixel{1.5F, -0.25F, 0.5F, 1.0F},  ExrPixel{0.25F, 0.5F, 4.0F, 1.0F},
            ExrPixel{2.0F, 0.125F, 0.75F, 0.5F}, ExrPixel{0.5F, 2.5F, -0.5F, 1.0F},
            ExrPixel{0.0F, 0.0F, 0.0F, 0.0F},    ExrPixel{3.0F, 1.0F, 0.25F, 0.25F}};
}

[[nodiscard]] inline const ImageFixture& syntheticImage() {
    static const ImageFixture fixture = [] {
        std::filesystem::create_directories(gateDirectory());
        ImageFixture value;
        value.path = gateDirectory() / "signed_hdr.exr";
        writeExrRgba(value.path, 3, 2, signedHdrPixels());
        const auto probe = bloom::media::probeImage(value.path);
        if (!probe.value.has_value()) {
            throw std::logic_error("synthetic EXR could not be probed: " + probe.diagnostic);
        }
        value.asset.id = bloom::document::AssetId::fromRaw(900);
        value.asset.kind = bloom::document::AssetKind::Image;
        value.asset.locator.kind = "file";
        value.asset.locator.portability = "project-relative";
        value.asset.locator.path = value.path.filename().string();
        value.asset.locator.relinkHint = "file:" + value.path.string();
        value.asset.contentDigest = probe.value->contentDigest;
        value.asset.interpretation.colorSpace = bloom::document::AssetColorSpace::Auto;
        value.asset.width = probe.value->width;
        value.asset.height = probe.value->height;
        value.asset.name = "signed_hdr";
        return value;
    }();
    return fixture;
}

// ImageSource -> translation-only Layer Output -> Normal Merge -> Composition Output. Media always
// takes the raster translation path, so the required native family is Translation.
[[nodiscard]] inline std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
imageSourcePlan(const bloom::document::AssetRecord& asset, const LayerValues layer,
                const std::uint64_t idBase) {
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    const LayerIds ids{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                       ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                       ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(bloom::runtime::CompiledImageSource{NodeId::fromRaw(idBase + 10), asset,
                                                                0, 0, 0, std::string{}, false});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 11),
                                        bloom::document::LayerId::fromRaw(idBase + 12),
                                        OperationIndex::fromRaw(0), ids, layer));
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{
            bloom::document::LayerSlotId::fromRaw(idBase + 14),
            bloom::document::LayerId::fromRaw(idBase + 12), OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), OperationIndex::fromRaw(2)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), bloom::document::ProjectId::fromRaw(1),
        bloom::document::CompositionId::fromRaw(2), format(8, 8), std::move(operations),
        OperationIndex::fromRaw(3)});
}

} // namespace bloom::gpu_coverage_media
