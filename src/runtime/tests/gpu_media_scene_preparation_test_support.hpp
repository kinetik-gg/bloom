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

// ACES AP0 primaries with the ACES white point, exactly the values the media EXR reader recognizes
// as the ACES2065-1 interpretation.
[[nodiscard]] inline Imf::Chromaticities acesAp0Chromaticities() {
    return Imf::Chromaticities(Imath::V2f(0.7347F, 0.2653F), Imath::V2f(0.0F, 1.0F),
                               Imath::V2f(0.0001F, -0.0770F), Imath::V2f(0.32168F, 0.33767F));
}

void writeExrRgbaWithChromaticities(const std::filesystem::path& path, const int width,
                                    const int height, const std::vector<ExrPixel>& pixels,
                                    const Imf::Chromaticities& chromaticities) {
    if (static_cast<int>(pixels.size()) != width * height) {
        throw std::logic_error("EXR fixture pixel count does not match its dimensions");
    }
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(width - 1, height - 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(chromaticities));
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

void writeExrRgba(const std::filesystem::path& path, const int width, const int height,
                  const std::vector<ExrPixel>& pixels) {
    writeExrRgbaWithChromaticities(path, width, height, pixels, Imf::Chromaticities());
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

// Writes a large RGBA EXR from one interleaved 16-bytes-per-pixel buffer (about 233 MB for
// 4608x3164), avoiding the four separate float staging vectors the small fixture writer uses. The
// reported user media is a 4608x3164 compressed EXR; this reproduces the same decoded extent and
// the same converted RGBA32F source size without depending on the share.
inline void writeLargeExrRgba(const std::filesystem::path& path, const int width,
                              const int height) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(width - 1, height - 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    std::vector<ExrPixel> pixels(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            ExrPixel& pixel = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)];
            pixel = ExrPixel{static_cast<float>(x) / static_cast<float>(width),
                             static_cast<float>(y) / static_cast<float>(height), 0.25F, 1.0F};
        }
    }
    const auto pixelStride = static_cast<std::size_t>(sizeof(ExrPixel));
    const auto rowStride = static_cast<std::size_t>(width) * sizeof(ExrPixel);
    auto* base = reinterpret_cast<char*>(pixels.data());
    Imf::FrameBuffer frameBuffer;
    frameBuffer.insert("R",
                       Imf::Slice(Imf::FLOAT, base + 0 * sizeof(float), pixelStride, rowStride));
    frameBuffer.insert("G",
                       Imf::Slice(Imf::FLOAT, base + 1 * sizeof(float), pixelStride, rowStride));
    frameBuffer.insert("B",
                       Imf::Slice(Imf::FLOAT, base + 2 * sizeof(float), pixelStride, rowStride));
    frameBuffer.insert("A",
                       Imf::Slice(Imf::FLOAT, base + 3 * sizeof(float), pixelStride, rowStride));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
}

// A large EXR source + FHD solid + text -> three layers -> Normal merge -> Composition Output. This
// is the reported graph shape; the source is deliberately larger than the composition so the
// converted upload keeps full source detail (no resize to composition dimensions).
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
largeSourcePlan(const CompositionFormat compositionFormat, const document::AssetRecord& asset,
                const std::uint64_t idBase) {
    const auto parameter = [](const std::uint64_t raw) {
        return bloom::document::ParameterId::fromRaw(raw);
    };
    const LayerIds imageIds{parameter(idBase + 0), parameter(idBase + 1), parameter(idBase + 2),
                            parameter(idBase + 3), parameter(idBase + 4), parameter(idBase + 5)};
    const LayerIds solidIds{parameter(idBase + 10), parameter(idBase + 11), parameter(idBase + 12),
                            parameter(idBase + 13), parameter(idBase + 14), parameter(idBase + 15)};
    const LayerIds textIds{parameter(idBase + 20), parameter(idBase + 21), parameter(idBase + 22),
                           parameter(idBase + 23), parameter(idBase + 24), parameter(idBase + 25)};
    std::vector<CompiledOperation> operations;
    operations.emplace_back(bloom::runtime::CompiledImageSource{
        bloom::document::NodeId::fromRaw(idBase + 30), asset, 0, 0, 0, std::string{}, false});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 31),
                                        bloom::document::LayerId::fromRaw(idBase + 32),
                                        OperationIndex::fromRaw(0), imageIds, LayerValues{}));
    operations.emplace_back(
        CompiledSolid{bloom::document::NodeId::fromRaw(idBase + 40),
                      {parameter(idBase + 41), Color4d{0.5, 0.25, 0.125, 1.0}},
                      {parameter(idBase + 42), static_cast<double>(compositionFormat.width())},
                      {parameter(idBase + 43), static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 44),
                                        bloom::document::LayerId::fromRaw(idBase + 45),
                                        OperationIndex::fromRaw(2), solidIds, LayerValues{}));
    operations.emplace_back(bloom::runtime::CompiledText{
        bloom::document::NodeId::fromRaw(idBase + 50),
        parameter(idBase + 51),
        "BLOOM",
        {parameter(idBase + 52), 96.0},
        {parameter(idBase + 53), Color4d{0.8, 0.4, 0.2, 1.0}},
        bloom::runtime::CompiledTextLayout{parameter(idBase + 54),
                                           0,
                                           {parameter(idBase + 55), 1.0},
                                           {parameter(idBase + 56), 0.0}}});
    operations.emplace_back(layerOutput(bloom::document::NodeId::fromRaw(idBase + 57),
                                        bloom::document::LayerId::fromRaw(idBase + 58),
                                        OperationIndex::fromRaw(4), textIds, LayerValues{}));
    operations.emplace_back(
        CompiledMerge{bloom::document::NodeId::fromRaw(idBase + 60),
                      std::vector<CompiledMergeInput>{
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 61),
                                             bloom::document::LayerId::fromRaw(idBase + 32),
                                             OperationIndex::fromRaw(1)},
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 62),
                                             bloom::document::LayerId::fromRaw(idBase + 45),
                                             OperationIndex::fromRaw(3)},
                          CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 63),
                                             bloom::document::LayerId::fromRaw(idBase + 58),
                                             OperationIndex::fromRaw(5)}}});
    operations.emplace_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(idBase + 64),
                                                      OperationIndex::fromRaw(6)});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
        std::move(operations), OperationIndex::fromRaw(7)});
}

} // namespace
