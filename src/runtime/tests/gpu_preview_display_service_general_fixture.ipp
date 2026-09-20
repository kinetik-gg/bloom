// Production media+text fixture for the general-display SERVICE acceptance test.
//
// A real full-size EXR source + solid + text composition, built from canonical immutable
// operations. It is kept in its own cohesive .ipp so the acceptance test translation unit stays
// readable. Included exactly once, only by the general-display service test.

#pragma once

#include "gpu_preview_display_service_general_support.hpp"

#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>

namespace {

#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

// A unique, per-run scoped throwaway directory. It never scans or removes a shared/known user path,
// so concurrent runs of this test cannot collide or delete each other's fixtures.
[[nodiscard]] std::filesystem::path makeUniqueProductionMediaDirectory() {
    static std::atomic<std::uint64_t> counter{0};
    const auto ticks =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device device;
    const auto salt = static_cast<std::uint64_t>(device()) ^ ticks ^
                      (counter.fetch_add(1, std::memory_order_relaxed) << 32U);
    return std::filesystem::temp_directory_path() /
           ("bloom_general_production_media_" + std::to_string(salt));
}

// A real, full-size RGBA EXR written to a throwaway directory. The production media pipeline probes
// and decodes it (no fake media handle). Kept local so this TU does not include the conflicting
// CPU media-preparation support header.
void writeProductionLargeExr(const std::filesystem::path& path, const int width, const int height) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(width - 1, height - 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
    header.insert("alphaAssociation", Imf::StringAttribute("premultiplied"));
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> red(count), green(count), blue(count), alpha(count, 1.0F);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            red[index] = static_cast<float>(x) / static_cast<float>(width);
            green[index] = static_cast<float>(y) / static_cast<float>(height);
            blue[index] = 0.25F;
        }
    }
    const auto stride = static_cast<std::size_t>(width) * sizeof(float);
    Imf::FrameBuffer frameBuffer;
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

[[nodiscard]] bloom::document::AssetRecord productionImageAsset(const std::filesystem::path& path,
                                                                const std::uint64_t rawId) {
    const auto probe = bloom::media::probeImage(path);
    if (!probe.value.has_value()) {
        throw std::logic_error("the production EXR fixture could not be probed");
    }
    bloom::document::AssetRecord asset;
    asset.id = bloom::document::AssetId::fromRaw(rawId);
    asset.kind = bloom::document::AssetKind::Image;
    asset.locator.kind = "file";
    asset.locator.portability = "project-relative";
    asset.locator.path = path.filename().string();
    asset.locator.relinkHint = "file:" + path.string();
    asset.contentDigest = probe.value->contentDigest;
    asset.interpretation.colorSpace = bloom::document::AssetColorSpace::Auto;
    asset.width = probe.value->width;
    asset.height = probe.value->height;
    asset.name = "production-large-source";
    return asset;
}

// The reported production graph shape: a full-size EXR source + solid + text, merged to the
// composition output. Built directly from canonical immutable operations.
[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
makeProductionMediaTextPlan(const bloom::document::CompositionFormat compositionFormat,
                            const bloom::document::AssetRecord& asset, const std::uint64_t idBase) {
    namespace document = bloom::document;
    namespace runtime = bloom::runtime;
    const auto parameter = [](const std::uint64_t raw) {
        return document::ParameterId::fromRaw(raw);
    };
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledImageSource{document::NodeId::fromRaw(idBase + 30),
                                                         asset, 0, 0, 0, std::string{}, false});
    operations.emplace_back(runtime::CompiledLayerOutput{
        document::NodeId::fromRaw(idBase + 31), document::LayerId::fromRaw(idBase + 32),
        runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{parameter(idBase + 0), document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{parameter(idBase + 1), document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{parameter(idBase + 2), document::kDefaultScale},
        runtime::CompiledScalarParameter{parameter(idBase + 3), document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{parameter(idBase + 4), 1.0}, parameter(idBase + 5),
        bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledSolid{
        document::NodeId::fromRaw(idBase + 40),
        {parameter(idBase + 41), bloom::core::Color4d{0.5, 0.25, 0.125, 1.0}},
        {parameter(idBase + 42), static_cast<double>(compositionFormat.width())},
        {parameter(idBase + 43), static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        document::NodeId::fromRaw(idBase + 44), document::LayerId::fromRaw(idBase + 45),
        runtime::OperationIndex::fromRaw(2),
        runtime::CompiledVec2Parameter{parameter(idBase + 10), document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{parameter(idBase + 11), document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{parameter(idBase + 12), document::kDefaultScale},
        runtime::CompiledScalarParameter{parameter(idBase + 13), document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{parameter(idBase + 14), 1.0}, parameter(idBase + 15),
        bloom::core::kDefaultBlendMode});
    operations.emplace_back(
        runtime::CompiledText{document::NodeId::fromRaw(idBase + 50),
                              parameter(idBase + 51),
                              "BLOOM",
                              {parameter(idBase + 52), 96.0},
                              {parameter(idBase + 53), bloom::core::Color4d{0.8, 0.4, 0.2, 1.0}},
                              runtime::CompiledTextLayout{parameter(idBase + 54),
                                                          0,
                                                          {parameter(idBase + 55), 1.0},
                                                          {parameter(idBase + 56), 0.0}}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        document::NodeId::fromRaw(idBase + 57), document::LayerId::fromRaw(idBase + 58),
        runtime::OperationIndex::fromRaw(4),
        runtime::CompiledVec2Parameter{parameter(idBase + 20), document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{parameter(idBase + 21), document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{parameter(idBase + 22), document::kDefaultScale},
        runtime::CompiledScalarParameter{parameter(idBase + 23), document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{parameter(idBase + 24), 1.0}, parameter(idBase + 25),
        bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        document::NodeId::fromRaw(idBase + 60),
        std::vector<runtime::CompiledMergeInput>{
            runtime::CompiledMergeInput{document::LayerSlotId::fromRaw(idBase + 61),
                                        document::LayerId::fromRaw(idBase + 32),
                                        runtime::OperationIndex::fromRaw(1)},
            runtime::CompiledMergeInput{document::LayerSlotId::fromRaw(idBase + 62),
                                        document::LayerId::fromRaw(idBase + 45),
                                        runtime::OperationIndex::fromRaw(3)},
            runtime::CompiledMergeInput{document::LayerSlotId::fromRaw(idBase + 63),
                                        document::LayerId::fromRaw(idBase + 58),
                                        runtime::OperationIndex::fromRaw(5)}}});
    operations.emplace_back(runtime::CompiledCompositionOutput{
        document::NodeId::fromRaw(idBase + 64), runtime::OperationIndex::fromRaw(6)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
            std::move(operations), runtime::OperationIndex::fromRaw(7)});
}

#endif // BLOOM_GPU_TOOLS_AVAILABLE

} // namespace
