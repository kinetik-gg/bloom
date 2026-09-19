#pragma once

// Media fixtures and scene builders for the CPU GPU-scene media preparation tests. Every fixture is
// a REAL OpenEXR file written to a throwaway directory and resolved through the production image
// pipeline (media::probeImage -> detail::selectImageSource -> detail::evaluateImageSource), so the
// tests exercise genuine decode/colour-conversion behaviour rather than a fake media handle.

#include "gpu_scene_preparation_test_support.hpp"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace document = bloom::document;

struct ExrPixel final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

void writeExrRgba(const std::filesystem::path& path, const int width, const int height,
                  const std::vector<ExrPixel>& pixels) {
    if (static_cast<int>(pixels.size()) != width * height) {
        throw std::logic_error("EXR fixture pixel count does not match its dimensions");
    }
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

[[nodiscard]] document::AssetRecord imageAsset(const std::filesystem::path& path,
                                               const std::string& name, const std::uint64_t rawId) {
    const auto probe = bloom::media::probeImage(path);
    if (!probe.value.has_value()) {
        throw std::logic_error("EXR fixture could not be probed: " + probe.diagnostic);
    }
    document::AssetRecord asset;
    asset.id = bloom::document::AssetId::fromRaw(rawId);
    asset.kind = document::AssetKind::Image;
    asset.locator.kind = "file";
    asset.locator.portability = "project-relative";
    asset.locator.path = path.filename().string();
    asset.locator.relinkHint = "file:" + path.string();
    asset.contentDigest = probe.value->contentDigest;
    asset.interpretation.colorSpace = document::AssetColorSpace::Auto;
    asset.width = probe.value->width;
    asset.height = probe.value->height;
    asset.name = name;
    return asset;
}

[[nodiscard]] document::AssetSequenceMember sequenceMember(const std::filesystem::path& path,
                                                           const std::int64_t frame,
                                                           const std::uint64_t rawId) {
    const auto probe = bloom::media::probeImage(path);
    if (!probe.value.has_value()) {
        throw std::logic_error("EXR sequence member could not be probed: " + probe.diagnostic);
    }
    document::AssetSequenceMember member;
    member.frame = frame;
    member.locator.kind = "file";
    member.locator.portability = "project-relative";
    member.locator.path = path.filename().string();
    member.locator.relinkHint = "file:" + path.string();
    member.contentDigest = probe.value->contentDigest;
    (void)rawId;
    return member;
}

// A single ImageSource -> translation-only Layer Output -> Normal Merge -> Composition Output
// graph.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
mediaPlan(const CompositionFormat compositionFormat, const document::AssetRecord& asset,
          const LayerValues layer, const std::uint64_t idBase, const std::int64_t startFrame = 0,
          const std::int64_t loopMode = 0) {
    const LayerIds ids{bloom::document::ParameterId::fromRaw(idBase + 0),
                       bloom::document::ParameterId::fromRaw(idBase + 1),
                       bloom::document::ParameterId::fromRaw(idBase + 2),
                       bloom::document::ParameterId::fromRaw(idBase + 3),
                       bloom::document::ParameterId::fromRaw(idBase + 4),
                       bloom::document::ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(
        bloom::runtime::CompiledImageSource{bloom::document::NodeId::fromRaw(idBase + 10), asset,
                                            startFrame, loopMode, 0, std::string{}, false});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 11),
                                        bloom::document::LayerId::fromRaw(idBase + 12),
                                        OperationIndex::fromRaw(0), ids, layer));
    operations.emplace_back(CompiledMerge{
        bloom::document::NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{
            bloom::document::LayerSlotId::fromRaw(idBase + 14),
            bloom::document::LayerId::fromRaw(idBase + 12), OperationIndex::fromRaw(1)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 15),
                                                      OperationIndex::fromRaw(2)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(3)});
}

// A 3x2 signed/HDR RGBA EXR: an odd source smaller than a typical composition, with a negative
// channel, values above 1 and a non-opaque alpha, written premultiplied.
[[nodiscard]] std::vector<ExrPixel> signedHdrPixels() {
    return {ExrPixel{1.5F, -0.25F, 0.5F, 1.0F},  ExrPixel{0.25F, 0.5F, 4.0F, 1.0F},
            ExrPixel{2.0F, 0.125F, 0.75F, 0.5F}, ExrPixel{0.5F, 2.5F, -0.5F, 1.0F},
            ExrPixel{0.0F, 0.0F, 0.0F, 0.0F},    ExrPixel{3.0F, 1.0F, 0.25F, 0.25F}};
}

} // namespace
