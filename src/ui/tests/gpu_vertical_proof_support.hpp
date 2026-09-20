#pragma once

// Shared, test-only support for the GPU end-to-end vertical proof.
//
// This header is compiled ONLY into the new acceptance test binaries under this prep directory. It
// is deliberately self-contained: it re-declares the exact fixture helpers the accepted runtime
// tests use (a real OpenEXR writer, a real probed AssetRecord, real CompiledCompositionPlan
// builders) instead of including a runtime test support header from the read-only main tree. No
// production TU includes it, and it owns no product behaviour.
//
// The fixtures are genuine: every EXR is written to a throwaway directory and probed through the
// production image pipeline, every plan is a real CompiledCompositionPlan, and the scene builders
// consume them through the production CpuGpuSceneBuilder.

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/media/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <QGuiApplication>

namespace bloom::ui::verticalproof {

// ---------------------------------------------------------------------------------------------
// Phase markers. The optional external Linux runner (proof/external_capture.sh) reads these from
// stdout to know when a real window is on screen and what metadata to compare. They are plain
// single-line records and never gate the test's own success.
// ---------------------------------------------------------------------------------------------
inline void phase(const std::string_view name, const std::string& metadata = {}) {
    std::cout << "PHASE " << name;
    if (!metadata.empty()) {
        std::cout << ' ' << metadata;
    }
    std::cout << '\n';
    std::cout.flush();
}

// ---------------------------------------------------------------------------------------------
// Bounded expectations. A failure is recorded and printed, never thrown.
// ---------------------------------------------------------------------------------------------
class Expectations final {
  public:
    void expect(const bool ok, const std::string& message,
                const std::source_location loc = std::source_location::current()) {
        if (ok) {
            return;
        }
        ++failures_;
        std::cerr << loc.file_name() << ':' << loc.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Loader / device options, matching the accepted native proof convention.
// ---------------------------------------------------------------------------------------------
struct TestOptions final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] inline TestOptions parseOptions(const int argc, char** argv) {
    TestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

template <typename Predicate>
[[nodiscard]] inline bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        // Keep the real QWindow responsive to the compositor while the native owner thread works,
        // so an external grim capture is not occluded by a "not responding" dialog.
        QGuiApplication::processEvents();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    QGuiApplication::processEvents();
    return predicate();
}

template <typename Value>
[[nodiscard]] inline std::optional<runtime::TaskResult<Value>>
awaitResult(const runtime::TaskHandle<Value>& handle,
            const std::chrono::milliseconds timeout = std::chrono::milliseconds(30'000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = handle.tryTakeResult()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// A real premultiplied RGBA float OpenEXR fixture. Mirrors the accepted runtime test support so the
// media decode/colour path is exercised rather than a fake handle.
// ---------------------------------------------------------------------------------------------
struct ExrPixel final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

inline void writeExrRgba(const std::filesystem::path& path, const int width, const int height,
                         const std::vector<ExrPixel>& pixels) {
    if (static_cast<int>(pixels.size()) != width * height) {
        std::abort();
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

// A real probed image asset for the EXR fixture. The locator is project-relative with a relink hint
// carrying the real absolute path, exactly as an imported still would be.
[[nodiscard]] inline document::AssetRecord
imageAsset(const std::filesystem::path& path, const std::string& name, const std::uint64_t rawId) {
    const auto probe = media::probeImage(path);
    if (!probe.value.has_value()) {
        std::abort();
    }
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(rawId);
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

// ---------------------------------------------------------------------------------------------
// Real plan builders.
//
// `makeSolidSourceOverPlan` is a hand-lowered 1920x1080 graph with two Solid layers whose layer
// boundaries use a fractional translation and Normal (source-over) compositing. It exists so the
// SourceOver / fractional-translation / non-square-PAR path can be driven without a document.
//
// `makeMediaPlan` is a single real ImageSource -> translation-only Layer Output -> Normal Merge ->
// Composition Output graph over a probed EXR asset.
// ---------------------------------------------------------------------------------------------
[[nodiscard]] inline std::shared_ptr<const runtime::CompiledCompositionPlan>
makeMediaPlan(const document::CompositionFormat compositionFormat,
              const document::AssetRecord& asset, const document::ProjectId projectId,
              const document::CompositionId compositionId, const document::Revision revision,
              const std::uint64_t rawBase) {
    const auto layerPosition = document::ParameterId::fromRaw(rawBase + 10);
    const auto layerAnchor = document::ParameterId::fromRaw(rawBase + 11);
    const auto layerScale = document::ParameterId::fromRaw(rawBase + 12);
    const auto layerRotation = document::ParameterId::fromRaw(rawBase + 13);
    const auto layerOpacity = document::ParameterId::fromRaw(rawBase + 14);
    const auto layerBlend = document::ParameterId::fromRaw(rawBase + 15);

    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledImageSource{document::NodeId::fromRaw(rawBase + 20),
                                                         asset, 0, 0, 0, std::string{}, true});
    operations.emplace_back(runtime::CompiledLayerOutput{
        document::NodeId::fromRaw(rawBase + 21), document::LayerId::fromRaw(rawBase + 22),
        runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{layerPosition, document::Vec2d{4.5, -3.25}},
        runtime::CompiledVec2Parameter{layerAnchor, document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{layerScale, document::kDefaultScale},
        runtime::CompiledScalarParameter{layerRotation, document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{layerOpacity, 1.0}, layerBlend, core::BlendMode::Normal});
    operations.emplace_back(runtime::CompiledMerge{
        document::NodeId::fromRaw(rawBase + 23),
        {{document::LayerSlotId::fromRaw(rawBase + 24), document::LayerId::fromRaw(rawBase + 22),
          runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(runtime::CompiledCompositionOutput{
        document::NodeId::fromRaw(rawBase + 25), runtime::OperationIndex::fromRaw(2)});

    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{revision, projectId, compositionId,
                                                   compositionFormat, std::move(operations),
                                                   runtime::OperationIndex::fromRaw(3)});
}

// A 64x48 still with a transparent hole and an opaque coloured quadrant, so a source-over composite
// is observable and a full-frame readback would be distinguishable from the resident path.
[[nodiscard]] inline std::vector<ExrPixel> stillPixels() {
    std::vector<ExrPixel> pixels(static_cast<std::size_t>(64U) * 48U);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            ExrPixel& pixel =
                pixels[static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)];
            if (x < 32 && y < 24) {
                pixel = ExrPixel{0.05F, 0.9F, 0.2F, 1.0F};
            } else if (x >= 32 && y >= 24) {
                pixel = ExrPixel{0.2F, 0.1F, 0.8F, 0.5F};
            } else {
                pixel = ExrPixel{0.0F, 0.0F, 0.0F, 0.0F};
            }
        }
    }
    return pixels;
}

} // namespace bloom::ui::verticalproof
