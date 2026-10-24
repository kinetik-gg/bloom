#pragma once

// Real still-image fixture for the bounded GPU coverage gate. It writes a genuine synthetic
// signed/HDR EXR to the gate work directory and resolves it through the production image probe and
// the production image-source lowering, exactly as the media scene-preparation suite does. The
// oracle and the preparer read the same bytes, time, and interpretation; there is no fake
// admission and no InvalidMediaAccepted path.

#include "cryptomatte_fixture_support.hpp"
#include "gpu_coverage_fixture_support.hpp"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
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

// A 3x2 signed/HDR RGBA EXR: smaller than the composition, with a negative channel, values above 1,
// and a non-opaque alpha, written premultiplied.
[[nodiscard]] inline std::vector<ExrPixel> signedHdrPixels() {
    return {ExrPixel{1.5F, -0.25F, 0.5F, 1.0F},  ExrPixel{0.25F, 0.5F, 4.0F, 1.0F},
            ExrPixel{2.0F, 0.125F, 0.75F, 0.5F}, ExrPixel{0.5F, 2.5F, -0.5F, 1.0F},
            ExrPixel{0.0F, 0.0F, 0.0F, 0.0F},    ExrPixel{3.0F, 1.0F, 0.25F, 0.25F}};
}

struct ImageFixture final {
    std::filesystem::path path;
    bloom::document::AssetRecord asset;
};

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

// A genuine 1D .cube LUT for a non-identity FileTransform. It is read back through the production
// color::readLutFile so the asset digest matches the exact bytes the CPU oracle and the GPU builder
// both consume.
struct LutFixture final {
    std::filesystem::path path;
    bloom::document::AssetRecord asset;
};

[[nodiscard]] inline const LutFixture& syntheticLut() {
    static const LutFixture fixture = [] {
        std::filesystem::create_directories(gateDirectory());
        LutFixture value;
        value.path = gateDirectory() / "coverage_curve.cube";
        {
            std::ofstream file(value.path);
            file << "TITLE \"coverage\"\nLUT_1D_SIZE 4\n";
            file << "0.0 0.0 0.0\n0.2 0.3 0.4\n0.6 0.5 0.7\n1.0 1.0 1.0\n";
        }
        const auto resource = bloom::color::readLutFile(value.path);
        if (resource.error != bloom::color::LutError::None) {
            throw std::logic_error("synthetic LUT could not be read");
        }
        value.asset.id = bloom::document::AssetId::fromRaw(901);
        value.asset.kind = bloom::document::AssetKind::Lut;
        value.asset.locator.kind = "file";
        value.asset.locator.portability = "project-relative";
        value.asset.locator.path = value.path.filename().string();
        value.asset.locator.relinkHint = "file:" + value.path.string();
        value.asset.contentDigest = resource.digest;
        value.asset.name = "coverage_curve";
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

// ADR 0026: a layered 3x2 EXR -- premultiplied root RGBA, a diffuse colour layer and a depth data
// channel with a negative value and a NaN -- for the Shuffle, Channel Copy and Remove fixtures.
[[nodiscard]] inline const ImageFixture& layeredImage() {
    static const ImageFixture fixture = [] {
        std::filesystem::create_directories(gateDirectory());
        ImageFixture value;
        value.path = gateDirectory() / "layered_channels.exr";
        const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(2, 1));
        Imf::Header header(window, window);
        header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
        const auto pixels = signedHdrPixels();
        std::vector<std::pair<std::string, std::vector<float>>> planes;
        const auto plane = [&](std::string name, auto&& sample) {
            std::vector<float> values;
            for (std::size_t index = 0; index < pixels.size(); ++index)
                values.push_back(sample(index));
            planes.emplace_back(std::move(name), std::move(values));
        };
        plane("R", [&](std::size_t i) { return pixels[i].red; });
        plane("G", [&](std::size_t i) { return pixels[i].green; });
        plane("B", [&](std::size_t i) { return pixels[i].blue; });
        plane("A", [&](std::size_t i) { return pixels[i].alpha; });
        plane("diffuse.R", [](std::size_t i) { return 0.125F * static_cast<float>(i); });
        plane("diffuse.G", [](std::size_t i) { return 1.5F - 0.25F * static_cast<float>(i); });
        plane("diffuse.B", [](std::size_t) { return 0.75F; });
        plane("depth.Z", [](std::size_t i) {
            return i == 4 ? std::numeric_limits<float>::quiet_NaN()
                          : -0.5F + 0.375F * static_cast<float>(i);
        });
        for (const auto& [name, values] : planes)
            header.channels().insert(name, Imf::Channel(Imf::FLOAT));
        Imf::FrameBuffer frameBuffer;
        for (auto& [name, values] : planes)
            frameBuffer.insert(name, Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(values.data()),
                                                sizeof(float), 3 * sizeof(float)));
        {
            Imf::OutputFile file(value.path.string().c_str(), header);
            file.setFrameBuffer(frameBuffer);
            file.writePixels(2);
        }
        const auto probe = bloom::media::probeImage(value.path);
        if (!probe.value.has_value())
            throw std::logic_error("layered EXR could not be probed: " + probe.diagnostic);
        value.asset.id = bloom::document::AssetId::fromRaw(901);
        value.asset.kind = bloom::document::AssetKind::Image;
        value.asset.locator.kind = "file";
        value.asset.locator.portability = "project-relative";
        value.asset.locator.path = value.path.filename().string();
        value.asset.locator.relinkHint = "file:" + value.path.string();
        value.asset.contentDigest = probe.value->contentDigest;
        value.asset.interpretation.colorSpace = bloom::document::AssetColorSpace::Auto;
        value.asset.width = probe.value->width;
        value.asset.height = probe.value->height;
        value.asset.name = "layered_channels";
        return value;
    }();
    return fixture;
}

enum class ChannelFixture : std::uint8_t {
    // Source -> slots diffuse.R/G/B and A, Colour mapping.
    Shuffle,
    // Source -> Remove "diffuse, @depth" -> slots R/G/B/A, Colour mapping.
    Remove,
    // Source -> Remove keep "A" -> Copy depth.Z from the source -> slots depth.Z x3 and "1", Raw.
    Copy,
};

[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
channelShufflePlan(const bloom::document::AssetRecord& asset, const LayerValues layer,
                   const std::uint64_t idBase, const ChannelFixture kind) {
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    using bloom::runtime::CompiledChannelStep;
    using bloom::runtime::CompiledChannelStepKind;
    const LayerIds ids{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                       ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                       ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    bloom::runtime::CompiledChannelShuffle shuffle;
    shuffle.sourceNodeId = NodeId::fromRaw(idBase + 20);
    shuffle.channels.sources.push_back(
        bloom::runtime::CompiledChannelSource{NodeId::fromRaw(idBase + 10), asset, 0, 0, 0, {}, {}});
    CompiledChannelStep source;
    source.kind = CompiledChannelStepKind::Source;
    source.nodeId = NodeId::fromRaw(idBase + 10);
    shuffle.channels.steps.push_back(source);
    switch (kind) {
    case ChannelFixture::Shuffle:
        shuffle.mapping = {"diffuse.R", "diffuse.G", "diffuse.B", "A"};
        break;
    case ChannelFixture::Remove: {
        CompiledChannelStep remove;
        remove.kind = CompiledChannelStepKind::Remove;
        remove.nodeId = NodeId::fromRaw(idBase + 21);
        remove.select = "diffuse, @depth";
        shuffle.channels.steps.push_back(remove);
        shuffle.mapping = {"R", "G", "B", "A"};
        break;
    }
    case ChannelFixture::Copy: {
        CompiledChannelStep keep;
        keep.kind = CompiledChannelStepKind::Remove;
        keep.nodeId = NodeId::fromRaw(idBase + 21);
        keep.select = "A";
        keep.keep = true;
        shuffle.channels.steps.push_back(keep);
        CompiledChannelStep copy;
        copy.kind = CompiledChannelStepKind::Copy;
        copy.nodeId = NodeId::fromRaw(idBase + 22);
        copy.input = 1;
        copy.from = 0;
        copy.select = "depth.Z";
        shuffle.channels.steps.push_back(copy);
        shuffle.mapping = {"depth.Z", "depth.Z", "depth.Z", "1"};
        shuffle.mode = 1;
        break;
    }
    }
    std::vector<CompiledOperation> operations;
    operations.emplace_back(std::move(shuffle));
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

// #364: a real multi-rank Cryptomatte EXR (two CryptoObject rank layers and a premultiplied
// beauty), with or without the `cryptomatte/<key>/manifest` metadata.
[[nodiscard]] inline const ImageFixture& cryptomatteImage(const bool manifest) {
    const auto make = [](const bool withManifest) {
        std::filesystem::create_directories(gateDirectory());
        ImageFixture value;
        value.path =
            gateDirectory() / (withManifest ? "cryptomatte_manifest.exr" : "cryptomatte_plain.exr");
        bloom::cryptomatte_fixture::writeCryptomatteExr(value.path, withManifest);
        const auto probe = bloom::media::probeImage(value.path);
        if (!probe.value.has_value())
            throw std::logic_error("Cryptomatte EXR could not be probed: " + probe.diagnostic);
        value.asset.id = bloom::document::AssetId::fromRaw(withManifest ? 902 : 903);
        value.asset.kind = bloom::document::AssetKind::Image;
        value.asset.locator.kind = "file";
        value.asset.locator.portability = "project-relative";
        value.asset.locator.path = value.path.filename().string();
        value.asset.locator.relinkHint = "file:" + value.path.string();
        value.asset.contentDigest = probe.value->contentDigest;
        value.asset.interpretation.colorSpace = bloom::document::AssetColorSpace::Auto;
        value.asset.width = probe.value->width;
        value.asset.height = probe.value->height;
        value.asset.name = withManifest ? "cryptomatte_manifest" : "cryptomatte_plain";
        return value;
    };
    static const ImageFixture withManifest = make(true);
    static const ImageFixture plain = make(false);
    return manifest ? withManifest : plain;
}

// Source -> Cryptomatte (CryptoObject, `mattes`) -> Layer Output. With `imageOutput` the layer
// reads the node's Image output: the beauty (an Image source of the same file) x the matte through
// the MultiplyAlpha combine the compiler emits; otherwise it reads the Mask output.
[[nodiscard]] inline std::shared_ptr<const CompiledCompositionPlan>
cryptomattePlan(const bloom::document::AssetRecord& asset, const LayerValues layer,
                const std::uint64_t idBase, std::vector<std::string> mattes,
                const bool imageOutput, const bool preview = false) {
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    const LayerIds ids{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                       ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                       ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    bloom::runtime::CompiledCryptomatte cryptomatte;
    cryptomatte.sourceNodeId = NodeId::fromRaw(idBase + 20);
    cryptomatte.channels.sources.push_back(bloom::runtime::CompiledChannelSource{
        NodeId::fromRaw(idBase + 10), asset, 0, 0, 0, {}, {}});
    bloom::runtime::CompiledChannelStep source;
    source.kind = bloom::runtime::CompiledChannelStepKind::Source;
    source.nodeId = NodeId::fromRaw(idBase + 10);
    cryptomatte.channels.steps.push_back(source);
    cryptomatte.layer = "CryptoObject";
    cryptomatte.mattes = std::move(mattes);
    cryptomatte.preview = preview;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(std::move(cryptomatte));
    auto layerInput = OperationIndex::fromRaw(0);
    if (imageOutput) {
        operations.emplace_back(bloom::runtime::CompiledImageSource{
            NodeId::fromRaw(idBase + 10), asset, 0, 0, 0, std::string{}, false});
        operations.emplace_back(bloom::runtime::CompiledImageEffect{
            NodeId::fromRaw(idBase + 20), OperationIndex::fromRaw(1),
            bloom::runtime::ImageCombineKernel{
                bloom::runtime::ImageCombineMode::MultiplyAlpha,
                bloom::runtime::CompiledScalarParameter{ParameterId{}, 1.0},
                bloom::render::ImageCoverageChannel::Alpha, std::nullopt, false},
            false, false, OperationIndex::fromRaw(0)});
        layerInput = OperationIndex::fromRaw(2);
    }
    const auto layerIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 11),
                                        bloom::document::LayerId::fromRaw(idBase + 12), layerInput,
                                        ids, layer));
    const auto mergeIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 13),
                      std::vector<CompiledMergeInput>{CompiledMergeInput{
                          bloom::document::LayerSlotId::fromRaw(idBase + 14),
                          bloom::document::LayerId::fromRaw(idBase + 12), layerIndex}}});
    const auto outputIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), mergeIndex});
    return publish(CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), bloom::document::ProjectId::fromRaw(1),
        bloom::document::CompositionId::fromRaw(2), format(8, 8), std::move(operations),
        outputIndex});
}

} // namespace bloom::gpu_coverage_media
