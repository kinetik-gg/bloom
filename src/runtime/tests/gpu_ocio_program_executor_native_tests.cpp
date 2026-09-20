// Native acceptance for the runtime OCIO command/program lifecycle. Real OCIO CST and display
// programs are extracted and compiled off-device through the runtime preparation service, then
// dispatched on the device owner thread by the runtime executor and compared against the actual
// unchanged CPU OCIO oracle. Warm frames prove zero compile and zero resource upload; a changed
// uniform/config/geometry proves invalidation; cancellation and an injected upload fault prove
// resource retirement. The source image stays resident and the executor performs no full-frame
// readback (the only readbacks here are the test-only oracle readbacks).
//
// A missing device is an explicit SKIP (exit 77) unless --require-device is passed.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_ocio_program_executor.hpp>
#include <bloom/runtime/gpu_ocio_wrapper.hpp>

#include "ocio_gpu_program_fault.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::Sha256Digest;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioExecutorBudgets;
using bloom::runtime::GpuOcioExecutorDiagnosticCode;
using bloom::runtime::GpuOcioExecutorJobState;
using bloom::runtime::GpuOcioExecutorPollResult;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramExecutor;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;

constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
constexpr int kSkipExit = 77;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::shared_ptr<const GpuImage> uploadImage(GpuImageUpload& uploader,
                                                          const std::uint32_t width,
                                                          const std::uint32_t height,
                                                          const std::vector<Rgba32f>& pixels) {
    const auto windowResult = bloom::render::ImageWindow::create(0, 0, width, height);
    if (!windowResult) {
        return nullptr;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(
        *windowResult.value(), *windowResult.value(), bloom::core::PixelAspectRatio::square());
    if (!descriptor || descriptor.value()->layout().pixelCount != pixels.size()) {
        return nullptr;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
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
    if (uploader.begin({source}, kBudget).code != GpuImageUploadDiagnosticCode::None) {
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

// Premultiplied fixture: HDR, a negative channel, translucent alpha, and an exact zero-alpha pixel.
[[nodiscard]] std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
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

[[nodiscard]] std::uint8_t quantize(const double value) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
}

[[nodiscard]] bool close(const float a, const float b) {
    if (a == b) {
        return true;
    }
    const double absolute = std::fabs(static_cast<double>(a) - static_cast<double>(b));
    const double magnitude =
        std::max(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)));
    return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
}

[[nodiscard]] GpuOcioCompileOptions compileOptions() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    return options;
}

struct Fixtures final {
    std::optional<bloom::color::ResolvedBloomNeutralConfig> neutral;
    std::optional<bloom::color::ResolvedBloomNeutralConfig> aces;
    GpuOcioCompileOptions options;
};

void testCst(Expectations& expectations, GpuDevice& device, GpuOcioProgramPreparer& preparer,
             const Fixtures& fixtures) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = "ACES2065-1";
    spec.toId = "ACEScg";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto prepared = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    expectations.expect(prepared.hasValue(), "the ACES CST command prepares");
    if (!prepared) {
        return;
    }
    expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::FinalRgba32f,
                        "the CST command is FinalRgba32f");
    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(executor.hasValue() && uploader.hasValue(),
                        "the executor and uploader host");
    if (!executor || !uploader) {
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    expectations.expect(input != nullptr, "the CST input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioExecutorDiagnosticCode::None,
                        "begin accepts the CST command");
    auto poll = executor.executor->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    expectations.expect(poll == GpuOcioExecutorPollResult::Ready, "the CST dispatch completes");
    auto output = executor.executor->takeEffectOutput();
    expectations.expect(output != nullptr, "the CST resident output is published");
    expectations.expect(input->isValid() && input->isBoundTo(device),
                        "the source remains resident after the dispatch");
    if (output == nullptr) {
        return;
    }
    expectations.expect(output->dataWindow() == input->dataWindow() &&
                            output->displayWindow() == input->displayWindow() &&
                            output->pixelAspect() == input->pixelAspect(),
                        "the OCIO effect output propagates the input window/PAR metadata exactly");
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    expectations.expect(readback.hasValue(), "the CST output reads back for the oracle");
    if (!readback) {
        return;
    }
    const auto cpu =
        bloom::color::CpuColorSpaceProcessor::prepare(*fixtures.aces, "ACES2065-1", "ACEScg");
    expectations.expect(cpu.succeeded(), "the CPU ACES oracle prepares");
    if (!cpu) {
        return;
    }
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<float, 4> straight{0.0F, 0.0F, 0.0F, 1.0F};
        if (p.alpha() != 0.0F) {
            straight[0] = p.red() / p.alpha();
            straight[1] = p.green() / p.alpha();
            straight[2] = p.blue() / p.alpha();
        }
        std::span<std::array<float, 4>> span(&straight, 1);
        if (!cpu.processor()->apply(span)) {
            ++mismatches;
            continue;
        }
        const auto& gpu = readback.pixels[index];
        if (!close(gpu.red(), straight[0] * p.alpha()) ||
            !close(gpu.green(), straight[1] * p.alpha()) ||
            !close(gpu.blue(), straight[2] * p.alpha()) || gpu.alpha() != p.alpha()) {
            ++mismatches;
        }
    }
    expectations.expect(mismatches == 0, "every CST pixel is within 2e-6 of the CPU oracle");

    // Exact CPU image-effect semantics for an alpha-zero source pixel: the effect arm copies the
    // original premultiplied pixel through unchanged (bit-exact), never transforming or
    // un-premultiplying it. The fixture's zero-alpha pixels are canonical (RGB +0), which is the
    // only representable form through Rgba32f::fromPremultiplied; the hidden-RGB case is covered by
    // the wrapper source-semantics assertion in the CPU command test.
    std::size_t zeroAlphaMismatches = 0;
    std::size_t zeroAlphaCount = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        if (p.alpha() != 0.0F) {
            continue;
        }
        ++zeroAlphaCount;
        const auto& gpu = readback.pixels[index];
        if (gpu.red() != p.red() || gpu.green() != p.green() || gpu.blue() != p.blue() ||
            gpu.alpha() != p.alpha()) {
            ++zeroAlphaMismatches;
        }
    }
    expectations.expect(zeroAlphaCount > 0, "the CST fixture exercises alpha-zero pixels");
    expectations.expect(zeroAlphaMismatches == 0,
                        "an alpha-zero effect pixel is copied through bit-exactly");

    const auto countersBefore = executor.executor->counters();
    const auto preparerBefore = preparer.counters();
    const auto warm = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(warm.code == GpuOcioExecutorDiagnosticCode::None,
                        "the warm CST job is accepted");
    auto warmPoll = executor.executor->poll();
    while (warmPoll == GpuOcioExecutorPollResult::Pending) {
        warmPoll = executor.executor->poll();
    }
    expectations.expect(warmPoll == GpuOcioExecutorPollResult::Ready &&
                            executor.executor->takeEffectOutput() != nullptr,
                        "the warm CST job completes");
    const auto countersAfter = executor.executor->counters();
    const auto preparerAfter = preparer.counters();
    expectations.expect(countersAfter.programCreations == countersBefore.programCreations,
                        "a warm frame performs zero native program creation");
    expectations.expect(countersAfter.programReuses == countersBefore.programReuses + 1,
                        "a warm frame reuses the retained native program");
    expectations.expect(preparerAfter.compiles == preparerBefore.compiles &&
                            preparerAfter.extractions == preparerBefore.extractions,
                        "a warm frame performs zero compile and zero extraction");
    const auto rePrepare = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    expectations.expect(rePrepare.hasValue() && rePrepare.command == prepared.command &&
                            preparer.counters().cacheHits == preparerBefore.cacheHits + 1,
                        "an identical preparation request is a cache hit");
    expectations.expect(executor.executor->counters().readbacks == 0,
                        "the executor never reads back a full frame");

    // The program cache charges the ACTUAL native VMA retained bytes: a ceiling one byte below the
    // measured retained allocation refuses the program without entering the cache.
    const std::uint64_t actualBytes = executor.executor->counters().cacheBytes;
    expectations.expect(actualBytes > 0, "the retained native program reports actual bytes");
    GpuOcioExecutorBudgets tightBudgets;
    tightBudgets.maxRetainedProgramBytes = actualBytes > 0 ? actualBytes - 1 : 0;
    auto tightExecutor = GpuOcioProgramExecutor::create(device, tightBudgets);
    if (tightExecutor) {
        const auto refused = tightExecutor.executor->begin(prepared.command, input, {}, kBudget);
        expectations.expect(refused.code == GpuOcioExecutorDiagnosticCode::OverBudget,
                            "a budget below the actual retained bytes refuses the program");
        expectations.expect(tightExecutor.executor->counters().programRefusals >= 1,
                            "the refusal is counted");
    }
}

void testDisplay(Expectations& expectations, GpuDevice& device, GpuOcioProgramPreparer& preparer,
                 const Fixtures& fixtures) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = std::string(fixtures.neutral->displayName());
    spec.view = std::string(fixtures.neutral->viewName());
    constexpr GpuOcioCommandGeometry geometry{5, 2};
    const auto prepared = preparer.prepare(*fixtures.neutral, spec, geometry, fixtures.options);
    expectations.expect(prepared.hasValue(), "the display command prepares");
    if (!prepared) {
        return;
    }
    expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::DisplayRgba8,
                        "the display command is DisplayRgba8");
    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!executor || !uploader) {
        expectations.expect(false, "the display executor and uploader host");
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    expectations.expect(input != nullptr, "the display input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioExecutorDiagnosticCode::None,
                        "begin accepts the display command");
    auto poll = executor.executor->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    expectations.expect(poll == GpuOcioExecutorPollResult::Ready, "the display dispatch completes");
    auto output = executor.executor->takeDisplayOutput();
    expectations.expect(output.has_value() && output->isValid(),
                        "the resident RGBA8 display output is published");
    if (!output.has_value() || !output->isValid()) {
        return;
    }
    const auto readback = bloom::render::readbackResidentDisplayImage(*output, kBudget);
    expectations.expect(readback.hasValue(), "the display output reads back for the oracle");
    if (!readback) {
        return;
    }
    const auto handle = bloom::color::buildBloomNeutralCpuDisplayProcessor(*fixtures.neutral);
    const auto* const processor = handle.handle();
    expectations.expect(processor != nullptr, "the CPU display oracle prepares");
    if (processor == nullptr) {
        return;
    }
    std::size_t rgbMismatches = 0;
    std::size_t alphaMismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<float, 3> straight{0.0F, 0.0F, 0.0F};
        if (p.alpha() != 0.0F) {
            straight = {p.red() / p.alpha(), p.green() / p.alpha(), p.blue() / p.alpha()};
        }
        const auto display = processor->referenceToDisplayLinear(
            bloom::core::Color4d{static_cast<double>(straight[0]), static_cast<double>(straight[1]),
                                 static_cast<double>(straight[2]), 1.0});
        if (!display.has_value()) {
            ++rgbMismatches;
            continue;
        }
        const auto& gpu = readback.pixels[index];
        if (std::abs(static_cast<int>(gpu.red) - quantize(display->red)) > 1 ||
            std::abs(static_cast<int>(gpu.green) - quantize(display->green)) > 1 ||
            std::abs(static_cast<int>(gpu.blue) - quantize(display->blue)) > 1) {
            ++rgbMismatches;
        }
        if (gpu.alpha != quantize(static_cast<double>(p.alpha()))) {
            ++alphaMismatches;
        }
    }
    expectations.expect(rgbMismatches == 0,
                        "every display pixel is within one RGBA8 code of the CPU oracle");
    expectations.expect(alphaMismatches == 0, "display alpha is byte-exact against the CPU oracle");

    // A too-small byte budget is refused before dispatch and the executor stays usable.
    const auto overBudget = executor.executor->begin(prepared.command, input, {}, 0);
    expectations.expect(overBudget.code == GpuOcioExecutorDiagnosticCode::OverBudget,
                        "a too-small byte budget is refused");
    expectations.expect(executor.executor->state() == GpuOcioExecutorJobState::Idle,
                        "a refused begin leaves the executor idle");
    const auto recovered = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(recovered.code == GpuOcioExecutorDiagnosticCode::None,
                        "the executor stays usable after a budget refusal");
    executor.executor->cancel();
    auto cancelPoll = executor.executor->poll();
    while (cancelPoll == GpuOcioExecutorPollResult::Pending) {
        cancelPoll = executor.executor->poll();
    }
    expectations.expect(cancelPoll == GpuOcioExecutorPollResult::Failure &&
                            executor.executor->diagnostic().code ==
                                GpuOcioExecutorDiagnosticCode::Cancelled,
                        "a cancelled job publishes no output");
    expectations.expect(!executor.executor->hasUnretiredSubmission(),
                        "a cancelled job's resources retire after fence proof");
}

void testViewAdjustInvalidation(Expectations& expectations, GpuOcioProgramPreparer& preparer,
                                const Fixtures& fixtures) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::ExposureContrast;
    spec.fromId = "ACEScg";
    spec.exposure = 0.5;
    spec.contrast = 1.0;
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto low = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    spec.exposure = 1.0;
    const auto high = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    GpuOcioTransformSpec neutralSpec;
    neutralSpec.kind = GpuOcioTransformKind::ExposureContrast;
    neutralSpec.fromId = std::string(fixtures.neutral->processColorSpaceId());
    neutralSpec.exposure = 1.0;
    neutralSpec.contrast = 1.0;
    const auto neutral =
        preparer.prepare(*fixtures.neutral, neutralSpec, geometry, fixtures.options);
    expectations.expect(low.hasValue() && high.hasValue() && neutral.hasValue(),
                        "the view-adjust commands prepare");
    if (low && high && neutral) {
        expectations.expect(low.command->identity() != high.command->identity(),
                            "a changed uniform value invalidates the command identity");
        expectations.expect(low.command->identity() != neutral.command->identity(),
                            "a changed config invalidates the command identity");
    }
    const auto changed =
        preparer.prepare(*fixtures.aces, spec, GpuOcioCommandGeometry{8, 3}, fixtures.options);
    expectations.expect(changed.hasValue() && low.hasValue() &&
                            changed.command->identity() != low.command->identity(),
                        "a changed geometry invalidates the command identity");
}

void testUploadFaultRetirement(Expectations& expectations, GpuDevice& device,
                               GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = "Rec.2100-PQ - Display";
    spec.view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto prepared = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    expectations.expect(prepared.hasValue(), "the LUT display command prepares");
    if (!prepared) {
        return;
    }
    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    if (!executor || !uploader) {
        expectations.expect(false, "the fault executor and uploader host");
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the fault input uploads");
        return;
    }
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::CancelAfterSubmit);
    const auto faulted = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(faulted.code == GpuOcioExecutorDiagnosticCode::Cancelled,
                        "a cancelled post-submit LUT upload is a typed failure");
    expectations.expect(bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the unproven upload retains its resources in the quarantine");
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::None);
    for (int attempt = 0;
         attempt < 200 && bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest();
         ++attempt) {
        bloom::render::ocio_program_detail::retireUploadQuarantineForTest();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expectations.expect(!bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the quarantined upload retires after fence proof");
    const auto recovered = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(recovered.code == GpuOcioExecutorDiagnosticCode::None,
                        "the executor stays usable after the fault retires");
    executor.executor->cancel();
    auto poll = executor.executor->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
    // Production-wrapper contract: the capacity-safe 2D flattening and the semantic version that
    // invalidates an older artifact. This needs no device or compiler and always runs.
    {
        bloom::render::OcioGpuProgramDesc probe;
        probe.shaderText = "vec4 bloom_ocio_probe(vec4 v) { return v; }\n";
        probe.functionName = "bloom_ocio_probe";
        probe.semanticsId = "bloom.test.ocio-wrapper-probe.v1";
        probe.stage = bloom::render::OcioGpuProgramStage::ProcessEffect;
        const auto wrapper = bloom::runtime::buildGpuOcioWrapperGlsl(probe);
        expectations.expect(wrapper.succeeded(), "the production OCIO wrapper builds");
        expectations.expect(wrapper.wrapperVersion == bloom::runtime::kGpuOcioWrapperVersion,
                            "the production OCIO wrapper reports its semantic version");
        expectations.expect(wrapper.source.find("gl_GlobalInvocationID.y * bloom_ocio_stride_x") !=
                                std::string::npos,
                            "the production OCIO wrapper flattens a capacity-safe 2D dispatch");
    }
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return kSkipExit;
#else
    Fixtures fixtures;
    fixtures.options = compileOptions();
    auto neutralResolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    fixtures.neutral = std::move(neutralResolution).takeResolved();
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        std::cout << "SKIP: the ACES built-in is unavailable\n";
        return kSkipExit;
    }
    auto acesResolution =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *revision, "ACEScg");
    fixtures.aces = std::move(acesResolution).takeResolved();
    if (!fixtures.neutral.has_value() || !fixtures.aces.has_value()) {
        std::cerr << "FAILED: the built-in configs do not resolve\n";
        return 1;
    }

    GpuDeviceCreationOptions options;
    auto device = GpuDevice::create(options);
    if (!device) {
        if (requireDevice) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return kSkipExit;
    }
    expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
    GpuOcioProgramPreparer preparer;
    testCst(expectations, *device.device, preparer, fixtures);
    testDisplay(expectations, *device.device, preparer, fixtures);
    testViewAdjustInvalidation(expectations, preparer, fixtures);
    testUploadFaultRetirement(expectations, *device.device, preparer, fixtures);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " runtime OCIO executor expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: runtime OCIO executor native acceptance\n";
    return 0;
#endif
}
