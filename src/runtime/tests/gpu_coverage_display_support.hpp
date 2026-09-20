#pragma once

// Genuine native display proofs for the bounded GPU coverage gate. There is no display-only
// exemption and no "claimed from a test name" evidence: a real OCIO display program is compiled with
// the shared GpuSceneOcioContext, dispatched through the production GpuOcioProgramExecutor on the
// owner thread, read back, and compared to the unchanged CPU display oracle (RGB within one RGBA8
// code, alpha exact). `view_adjust` uses a non-neutral post-display exposure/gamma; the custom view
// proof uses a non-default display/view pair extracted from the pinned ACES built-in. No CPU frame is
// ever relabelled GPU.

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_ocio_program_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/view_adjust.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::gpu_coverage_display {

struct DisplayProofOutcome final {
    bool ran = false;
    bool passed = false;
    std::string evidence;
};

using bloom::core::Color4d;
using bloom::render::GpuDevice;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioExecutorDiagnosticCode;
using bloom::runtime::GpuOcioExecutorPollResult;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramExecutor;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::ViewAdjust;

inline constexpr std::uint64_t kDisplayBudget = std::uint64_t{1} << 30;

// The one resolved ACES config; move-only, so it lives as a function-local static and is only read.
[[nodiscard]] inline const bloom::color::ResolvedBloomNeutralConfig* acesConfig() {
    static const std::optional<bloom::color::ResolvedBloomNeutralConfig> resolved = [] {
        const auto revision = bloom::color::ocioBuiltInContentRevision(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
        if (!revision.has_value()) {
            return std::optional<bloom::color::ResolvedBloomNeutralConfig>{};
        }
        auto result = bloom::color::resolveOcioBuiltIn(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
            *revision, "ACEScg");
        return std::move(result).takeResolved();
    }();
    return resolved.has_value() ? &*resolved : nullptr;
}

// Premultiplied HDR/negative/translucent/zero-alpha pixels so the display transform and adjustment
// are exercised over their real domain.
[[nodiscard]] inline std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
                                                        const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float alpha = ((x + y) % 3 == 0) ? 0.0F : 0.25F + 0.5F * fx;
            const auto value = Rgba32f::fromPremultiplied((1.6F + fx) * alpha, (-0.2F + fy) * alpha,
                                                          (0.125F + 2.0F * fx) * alpha, alpha);
            if (value) {
                pixels[static_cast<std::size_t>(y) * width + x] = *value.value();
            }
        }
    }
    return pixels;
}

[[nodiscard]] inline std::shared_ptr<const GpuImage>
uploadImage(GpuImageUpload& uploader, const std::uint32_t width, const std::uint32_t height,
            const std::vector<Rgba32f>& pixels) {
    const auto window = bloom::render::ImageWindow::create(0, 0, width, height);
    if (!window) {
        return nullptr;
    }
    const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
        *window.value(), *window.value(), bloom::core::PixelAspectRatio::square());
    if (!descriptor || descriptor.value()->layout().pixelCount != pixels.size()) {
        return nullptr;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kDisplayBudget);
    if (!builder) {
        return nullptr;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto row = builder.value()->row(y);
        if (!row) {
            return nullptr;
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            (*row.value())[x] = pixels[static_cast<std::size_t>(y) * width + x];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return nullptr;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
    if (uploader.begin({source}, kDisplayBudget).code != GpuImageUploadDiagnosticCode::None) {
        return nullptr;
    }
    auto poll = GpuImageUploadPollResult::Pending;
    while (poll == GpuImageUploadPollResult::Pending) {
        poll = uploader.poll();
    }
    if (poll != GpuImageUploadPollResult::Ready) {
        return nullptr;
    }
    return std::make_shared<GpuImage>(uploader.takeImage());
}

struct Pair final {
    std::string display;
    std::string view;
};

// Runs one genuine native display proof. `customView` selects the non-default display/view proof
// (neutral adjustment); otherwise the non-neutral ViewAdjust proof runs on the default pair.
[[nodiscard]] inline DisplayProofOutcome
runDisplayProof(GpuDevice& device, const GpuSceneOcioContext& ocioContext, const bool customView) {
    DisplayProofOutcome outcome;
    const auto* config = acesConfig();
    if (config == nullptr) {
        outcome.evidence = "the pinned ACES built-in is unavailable";
        return outcome;
    }
    if (ocioContext.preparer == nullptr) {
        outcome.evidence = "no shared OCIO preparer for the display proof";
        return outcome;
    }
    std::vector<Pair> pairs;
    for (const auto& entry : config->displays()) {
        auto built =
            bloom::color::buildCpuDisplayProcessorForView(*config, entry.display, entry.view);
        if (built.handle() != nullptr) {
            pairs.push_back(Pair{std::string{entry.display}, std::string{entry.view}});
        }
    }
    if (pairs.empty()) {
        outcome.evidence = "no ACES display/view pair has a CPU processor";
        return outcome;
    }
    const auto pair = pairs[std::min<std::size_t>(pairs.size() - 1, customView ? 1U : 0U)];
    const ViewAdjust adjust =
        customView ? ViewAdjust{} : ViewAdjust{.exposure = 1.0, .gamma = 0.8};
    auto cpuResult = bloom::color::buildCpuDisplayProcessorForView(*config, pair.display, pair.view);
    const auto* const processor = cpuResult.handle();
    if (processor == nullptr) {
        outcome.evidence = "the CPU display oracle did not prepare";
        return outcome;
    }

    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!executor || !uploader) {
        outcome.evidence = "the display executor or uploader did not host";
        return outcome;
    }
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 3;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        outcome.evidence = "the display input did not upload";
        return outcome;
    }

    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = pair.display;
    spec.view = pair.view;
    spec.viewAdjust = adjust;
    const auto prepared = ocioContext.preparer->prepare(*config, spec,
                                                        GpuOcioCommandGeometry{width, height},
                                                        ocioContext.compileOptions);
    if (!prepared) {
        outcome.evidence = "the display program did not prepare: " +
                           std::string{bloom::runtime::gpuOcioPreparationErrorName(prepared.error)};
        return outcome;
    }
    if (prepared.command->encoding() != GpuOcioOutputEncoding::DisplayRgba8) {
        outcome.evidence = "the prepared display command is not DisplayRgba8";
        return outcome;
    }
    if (executor.executor->begin(prepared.command, input, {}, kDisplayBudget).code !=
        GpuOcioExecutorDiagnosticCode::None) {
        outcome.evidence = "the display dispatch was refused";
        return outcome;
    }
    auto poll = executor.executor->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    if (poll != GpuOcioExecutorPollResult::Ready) {
        outcome.evidence = "the native display dispatch did not complete";
        return outcome;
    }
    auto output = executor.executor->takeDisplayOutput();
    if (!output.has_value() || !output->isValid()) {
        outcome.evidence = "the native display output did not publish";
        return outcome;
    }
    const auto counters = executor.executor->counters();
    if (counters.displayDispatches == 0) {
        outcome.evidence = "the display proof performed zero native display dispatches";
        return outcome;
    }
    const auto readback = bloom::render::readbackResidentDisplayImage(*output, kDisplayBudget);
    if (!readback) {
        outcome.evidence = "the native display output did not read back";
        return outcome;
    }

    std::size_t rgbMismatches = 0;
    std::size_t alphaMismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& pixel = pixels[index];
        std::array<double, 3> straight{0.0, 0.0, 0.0};
        if (pixel.alpha() != 0.0F) {
            straight = {static_cast<double>(pixel.red() / pixel.alpha()),
                        static_cast<double>(pixel.green() / pixel.alpha()),
                        static_cast<double>(pixel.blue() / pixel.alpha())};
        }
        const auto display = processor->referenceToDisplayLinear(
            Color4d{straight[0], straight[1], straight[2], 1.0});
        if (!display.has_value()) {
            ++rgbMismatches;
            continue;
        }
        const auto expectedR = ViewAdjust::quantize(adjust.fromEncoded(display->red));
        const auto expectedG = ViewAdjust::quantize(adjust.fromEncoded(display->green));
        const auto expectedB = ViewAdjust::quantize(adjust.fromEncoded(display->blue));
        const auto expectedA = ViewAdjust::quantize(static_cast<double>(pixel.alpha()));
        const auto& gpu = readback.pixels[index];
        if (std::abs(static_cast<int>(gpu.red) - expectedR) > 1 ||
            std::abs(static_cast<int>(gpu.green) - expectedG) > 1 ||
            std::abs(static_cast<int>(gpu.blue) - expectedB) > 1) {
            ++rgbMismatches;
        }
        if (gpu.alpha != expectedA) {
            ++alphaMismatches;
        }
    }
    if (rgbMismatches != 0 || alphaMismatches != 0) {
        outcome.evidence = "native display parity failed: " + std::to_string(rgbMismatches) +
                           " RGB >1 code, " + std::to_string(alphaMismatches) + " non-exact alpha";
        return outcome;
    }
    outcome.ran = true;
    outcome.passed = true;
    outcome.evidence = "native display dispatch " + std::to_string(counters.displayDispatches) +
                       ", display/view " + pair.display + " / " + pair.view +
                       (customView ? ", custom view transform" : ", view adjustment");
    return outcome;
}

} // namespace bloom::gpu_coverage_display
