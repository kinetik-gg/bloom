// Native acceptance for the general GPU display arm (runtime::GpuOcioDisplayArm) and its genuine
// program/descriptor qualification (qualifyGpuOcioDisplay).
//
// It extracts and compiles real OCIO display programs off-device through the shared runtime
// preparation service for the DEFAULT Bloom Neutral display/view and two NON-DEFAULT ACES
// display/view pairs, dispatches each on the device owner thread through the arm, and compares the
// resident RGBA8 output to the unchanged CPU OCIO display oracle (<= one code RGB, alpha exact).
// Each pair is exercised across the full six-adjustment matrix (neutral plus exposure/gamma
// extremes); the producer DisplayRgba8 wrapper bakes the exact post-display adjustment, so a
// changed adjustment is a different command identity and a mismatched request adjustment is
// refused as an IdentityMismatch. The arm performs no full-frame readback: the only readbacks here
// are the test-only oracle readbacks. A geometry above the retired 4K ceiling is admitted on real
// device capacity with no pixel interval.
//
// A missing device is an explicit SKIP (exit 77) unless --require-device is passed.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_ocio_display_arm.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::render::GpuDevice;
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
using bloom::runtime::GpuOcioDisplayArm;
using bloom::runtime::GpuOcioDisplayArmDiagnosticCode;
using bloom::runtime::GpuOcioDisplayDiagnosticCode;
using bloom::runtime::GpuOcioDisplayOutcome;
using bloom::runtime::GpuOcioDisplayQualificationBudgets;
using bloom::runtime::GpuOcioDisplayQualificationReport;
using bloom::runtime::GpuOcioDisplayRequest;
using bloom::runtime::GpuOcioExecutorPollResult;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::PreparedGpuOcioCommand;
using bloom::runtime::qualifyGpuOcioDisplay;
using bloom::runtime::ViewAdjust;

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

[[nodiscard]] GpuOcioCompileOptions compileOptions() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    return options;
}

// The exact CPU display reference for one straight source pixel under a ViewAdjust, matching the
// unchanged sibling oracle (gpu_ocio_view_adjust_native_tests.cpp):
//   encoded = referenceToDisplayLinear(straight)    (the OCIO display function, UNCLAMPED)
//   adjusted = viewAdjust.fromEncoded(encoded)      (post-display exposure/gamma, CPU semantics)
//   code = ViewAdjust::quantize(adjusted)
// This mirrors runtime::detail::adjustedQualifiedBuffer. `referenceToDisplayLinear` is the
// unclamped display value; `referenceToDisplay` clamps to [0,1] BEFORE the exposure/gamma step and
// therefore is NOT the CPU oracle. For a neutral adjust this reduces to the plain encoded code.
[[nodiscard]] bool cpuDisplayCode(const bloom::color::PreparedCpuDisplayProcessorHandle& handle,
                                  const std::array<float, 3>& straight, const ViewAdjust& adjust,
                                  std::array<std::uint8_t, 3>& out) {
    const auto encoded = handle.referenceToDisplayLinear(
        bloom::core::Color4d{static_cast<double>(straight[0]), static_cast<double>(straight[1]),
                             static_cast<double>(straight[2]), 1.0});
    if (!encoded.has_value()) {
        return false;
    }
    out = {ViewAdjust::quantize(adjust.fromEncoded(encoded->red)),
           ViewAdjust::quantize(adjust.fromEncoded(encoded->green)),
           ViewAdjust::quantize(adjust.fromEncoded(encoded->blue))};
    return true;
}

// The full adjustment matrix from the unchanged CPU oracle: neutral plus exposure/gamma extremes.
struct AdjustCase final {
    const char* name;
    ViewAdjust adjust;
};

constexpr std::array<AdjustCase, 6> kAdjustCases{{
    {"neutral", ViewAdjust{}},
    {"exposure-negative", ViewAdjust{.exposure = -1.0, .gamma = 1.0}},
    {"exposure-hdr", ViewAdjust{.exposure = 2.0, .gamma = 1.0}},
    {"gamma-lift", ViewAdjust{.exposure = 0.0, .gamma = 2.2}},
    {"gamma-crush", ViewAdjust{.exposure = 0.0, .gamma = 0.5}},
    {"exposure-and-gamma", ViewAdjust{.exposure = 1.0, .gamma = 0.8}},
}};

[[nodiscard]] std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
                                                 const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float alpha = ((x + y) % 3U == 0U) ? 0.0F : 0.25F + 0.5F * fx;
            const auto value = Rgba32f::fromPremultiplied((1.6F + fx) * alpha, (-0.2F + fy) * alpha,
                                                          (0.125F + 2.0F * fx) * alpha, alpha);
            if (value) {
                pixels[static_cast<std::size_t>(y) * width + x] = *value.value();
            }
        }
    }
    return pixels;
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

// Runs one strict display dispatch through the arm and verifies the resident RGBA8 output against
// the independent CPU OCIO display oracle. Returns the RGB mismatch count, or nullopt on a native
// failure. `expectColdDispatch` and `expectWarmDispatch` assert the actual native dispatch delta.
void testDisplayPair(Expectations& expectations, GpuDevice& device,
                     GpuOcioProgramPreparer& preparer,
                     const bloom::color::ResolvedBloomNeutralConfig& config,
                     const std::string& display, const std::string& view,
                     const std::string_view label, const bool adjustMatrix) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = display;
    spec.view = view;
    constexpr GpuOcioCommandGeometry geometry{5, 2};
    const auto prepared = preparer.prepare(config, spec, geometry, compileOptions());
    expectations.expect(prepared.hasValue(), std::string(label) + ": the display command prepares");
    if (!prepared) {
        return;
    }
    expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::DisplayRgba8,
                        std::string(label) + ": the display command is DisplayRgba8");

    const auto cpuHandle = bloom::color::buildCpuDisplayProcessorForView(config, display, view);
    expectations.expect(cpuHandle.handle() != nullptr,
                        std::string(label) + ": the CPU display oracle prepares");
    if (cpuHandle.handle() == nullptr) {
        return;
    }

    auto armResult = GpuOcioDisplayArm::create(device);
    expectations.expect(armResult.hasValue(), std::string(label) + ": the display arm hosts");
    if (!armResult) {
        return;
    }
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(uploader.hasValue(), std::string(label) + ": the uploader hosts");
    if (!uploader) {
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    expectations.expect(input != nullptr, std::string(label) + ": the input uploads");
    if (input == nullptr) {
        return;
    }

    const auto coldBefore = armResult.arm->counters();
    GpuOcioDisplayRequest request;
    request.command = prepared.command;
    request.input = input;
    request.width = geometry.width;
    request.height = geometry.height;
    request.byteBudget = kBudget;
    const auto begun = armResult.arm->begin(request);
    expectations.expect(begun.code == GpuOcioDisplayArmDiagnosticCode::None,
                        std::string(label) + ": begin accepts the display command");
    if (begun.code != GpuOcioDisplayArmDiagnosticCode::None) {
        return;
    }
    auto poll = armResult.arm->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = armResult.arm->poll();
    }
    expectations.expect(poll == GpuOcioExecutorPollResult::Ready,
                        std::string(label) + ": the display dispatch completes");
    if (poll != GpuOcioExecutorPollResult::Ready) {
        return;
    }
    auto output = armResult.arm->takeDisplayImage();
    expectations.expect(output.has_value() && output->isValid(),
                        std::string(label) + ": the resident RGBA8 output is published");
    if (!output.has_value() || !output->isValid()) {
        return;
    }
    const auto coldAfter = armResult.arm->counters();
    expectations.expect(coldAfter.displayDispatches == coldBefore.displayDispatches + 1,
                        std::string(label) + ": the cold frame dispatches exactly one display op");
    expectations.expect(coldAfter.programCreations == coldBefore.programCreations + 1,
                        std::string(label) + ": the cold frame creates the program once");
    expectations.expect(coldAfter.readbacks == 0,
                        std::string(label) + ": the executor never reads back a full frame");

    const auto readback = bloom::render::readbackResidentDisplayImage(*output, kBudget);
    expectations.expect(readback.hasValue(), std::string(label) + ": the output reads back");
    if (!readback) {
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
        std::array<std::uint8_t, 3> expected{};
        if (!cpuDisplayCode(*cpuHandle.handle(), straight, request.viewAdjust, expected)) {
            ++rgbMismatches;
            continue;
        }
        const auto& gpu = readback.pixels[index];
        if (std::abs(static_cast<int>(gpu.red) - expected[0]) > 1 ||
            std::abs(static_cast<int>(gpu.green) - expected[1]) > 1 ||
            std::abs(static_cast<int>(gpu.blue) - expected[2]) > 1) {
            ++rgbMismatches;
        }
        if (gpu.alpha != ViewAdjust::quantize(static_cast<double>(p.alpha()))) {
            ++alphaMismatches;
        }
    }
    expectations.expect(rgbMismatches == 0,
                        std::string(label) + ": every display pixel is within one RGBA8 code");
    expectations.expect(alphaMismatches == 0, std::string(label) + ": display alpha is byte-exact");

    // Warm frame: same command/input, zero dispatch, zero creation.
    const auto warm = armResult.arm->begin(request);
    expectations.expect(warm.code == GpuOcioDisplayArmDiagnosticCode::None,
                        std::string(label) + ": the warm job is accepted");
    auto warmPoll = armResult.arm->poll();
    while (warmPoll == GpuOcioExecutorPollResult::Pending) {
        warmPoll = armResult.arm->poll();
    }
    expectations.expect(warmPoll == GpuOcioExecutorPollResult::Ready &&
                            armResult.arm->takeDisplayImage().has_value(),
                        std::string(label) + ": the warm job completes");
    const auto warmAfter = armResult.arm->counters();
    expectations.expect(warmAfter.displayDispatches == coldAfter.displayDispatches + 1,
                        std::string(label) + ": the warm frame still dispatches (no scene cache)");
    expectations.expect(warmAfter.programCreations == coldAfter.programCreations,
                        std::string(label) + ": the warm frame creates zero programs");
    expectations.expect(warmAfter.programReuses == coldAfter.programReuses + 1,
                        std::string(label) + ": the warm frame reuses the retained program");
    expectations.expect(warmAfter.readbacks == 0,
                        std::string(label) + ": the warm frame reads back nothing");

    // The full non-neutral adjustment matrix: each adjustment is a distinct prepared command that
    // carries its exact baked post-display exposure/gamma. The arm dispatches it and the resident
    // RGBA8 output must match the unchanged CPU oracle (unclamped display -> fromEncoded ->
    // quantize) within one code with alpha exact. A changed adjustment is a different command
    // identity; a request adjustment that differs from the prepared command's is refused.
    std::vector<bloom::core::Sha256Digest> identities;
    if (adjustMatrix) {
        GpuOcioDisplayRequest mismatchedProbe;
        bool haveMismatchProbe = false;
        for (const auto& adjustCase : kAdjustCases) {
            GpuOcioTransformSpec adjustSpec;
            adjustSpec.kind = GpuOcioTransformKind::Display;
            adjustSpec.display = display;
            adjustSpec.view = view;
            adjustSpec.viewAdjust = adjustCase.adjust;
            const auto adjustedCommand =
                preparer.prepare(config, adjustSpec, geometry, compileOptions());
            expectations.expect(adjustedCommand.hasValue(),
                                std::string(label) + ": the adjusted display command prepares (" +
                                    adjustCase.name + ")");
            if (!adjustedCommand) {
                continue;
            }
            expectations.expect(adjustedCommand.command->viewAdjust() == adjustCase.adjust,
                                std::string(label) +
                                    ": the command carries the exact adjustment (" +
                                    adjustCase.name + ")");
            identities.push_back(adjustedCommand.command->identity());

            GpuOcioDisplayRequest adjustedRequest;
            adjustedRequest.command = adjustedCommand.command;
            adjustedRequest.input = input;
            adjustedRequest.width = geometry.width;
            adjustedRequest.height = geometry.height;
            adjustedRequest.viewAdjust = adjustCase.adjust;
            adjustedRequest.byteBudget = kBudget;
            if (!haveMismatchProbe) {
                mismatchedProbe = adjustedRequest;
                haveMismatchProbe = true;
            }
            const auto adjustedBegun = armResult.arm->begin(adjustedRequest);
            expectations.expect(adjustedBegun.code == GpuOcioDisplayArmDiagnosticCode::None,
                                std::string(label) + ": the adjusted display is admitted (" +
                                    adjustCase.name + ")");
            if (adjustedBegun.code != GpuOcioDisplayArmDiagnosticCode::None) {
                continue;
            }
            auto adjustedPoll = armResult.arm->poll();
            while (adjustedPoll == GpuOcioExecutorPollResult::Pending) {
                adjustedPoll = armResult.arm->poll();
            }
            expectations.expect(adjustedPoll == GpuOcioExecutorPollResult::Ready,
                                std::string(label) + ": the adjusted display completes (" +
                                    adjustCase.name + ")");
            auto adjustedOutput = armResult.arm->takeDisplayImage();
            expectations.expect(adjustedOutput.has_value(),
                                std::string(label) + ": the adjusted display publishes (" +
                                    adjustCase.name + ")");
            if (!adjustedOutput.has_value()) {
                continue;
            }
            const auto adjustedReadback =
                bloom::render::readbackResidentDisplayImage(*adjustedOutput, kBudget);
            expectations.expect(adjustedReadback.hasValue(),
                                std::string(label) + ": the adjusted output reads back (" +
                                    adjustCase.name + ")");
            if (!adjustedReadback) {
                continue;
            }
            std::size_t adjustedMismatches = 0;
            for (std::size_t index = 0; index < pixels.size(); ++index) {
                const auto& p = pixels[index];
                std::array<float, 3> straight{0.0F, 0.0F, 0.0F};
                if (p.alpha() != 0.0F) {
                    straight = {p.red() / p.alpha(), p.green() / p.alpha(), p.blue() / p.alpha()};
                }
                std::array<std::uint8_t, 3> expected{};
                if (!cpuDisplayCode(*cpuHandle.handle(), straight, adjustCase.adjust, expected)) {
                    ++adjustedMismatches;
                    continue;
                }
                const auto& gpu = adjustedReadback.pixels[index];
                if (std::abs(static_cast<int>(gpu.red) - expected[0]) > 1 ||
                    std::abs(static_cast<int>(gpu.green) - expected[1]) > 1 ||
                    std::abs(static_cast<int>(gpu.blue) - expected[2]) > 1 ||
                    gpu.alpha != ViewAdjust::quantize(static_cast<double>(p.alpha()))) {
                    ++adjustedMismatches;
                }
            }
            expectations.expect(adjustedMismatches == 0,
                                std::string(label) +
                                    ": every adjusted pixel matches the exact CPU "
                                    "semantics (" +
                                    adjustCase.name + ")");
        }
        expectations.expect(identities.size() == kAdjustCases.size(),
                            std::string(label) + ": every adjustment prepared a command");
        for (std::size_t i = 1; i < identities.size(); ++i) {
            expectations.expect(identities[i] != identities[0],
                                std::string(label) + ": a changed adjustment changes the command "
                                                     "identity");
        }
        // A request adjustment that differs from the prepared command's is an identity mismatch.
        if (haveMismatchProbe) {
            mismatchedProbe.viewAdjust = ViewAdjust{1.0, 1.0};
            const auto refused = armResult.arm->begin(mismatchedProbe);
            expectations.expect(refused.code == GpuOcioDisplayArmDiagnosticCode::IdentityMismatch,
                                std::string(label) + ": a mismatched request adjust is refused");
        }
    }

    // Genuine runtime qualification of the actual program/descriptor.
    const auto report = qualifyGpuOcioDisplay(prepared.command, *cpuHandle.handle(), device,
                                              GpuOcioDisplayQualificationBudgets{kBudget, kBudget});
    expectations.expect(report.outcome() == GpuOcioDisplayOutcome::PreviewOnly && report.eligible(),
                        std::string(label) + ": the program qualifies on real parity");
    expectations.expect(report.eligibleFor(device, *prepared.command),
                        std::string(label) + ": the report names this device and command");
}

// Proves the general display arm admits a geometry ABOVE the retired fixed 4K ceiling
// (3840*2160 == 8294400 pixels) purely on real device capacity and the exact prepared program,
// with no artificial pixel interval. Uses one real frame at the requested size and nothing extra.
void testLargeGeometry(Expectations& expectations, GpuDevice& device,
                       GpuOcioProgramPreparer& preparer,
                       const bloom::color::ResolvedBloomNeutralConfig& config) {
    constexpr std::uint32_t width = 4096;
    constexpr std::uint32_t height = 2304;
    static_assert(static_cast<std::uint64_t>(width) * height > 3840ULL * 2160ULL,
                  "the large-geometry case must exceed the retired 4K ceiling");
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = std::string(config.displayName());
    spec.view = std::string(config.viewName());
    const auto prepared =
        preparer.prepare(config, spec, GpuOcioCommandGeometry{width, height}, compileOptions());
    expectations.expect(prepared.hasValue(), "the >4K display command prepares");
    if (!prepared) {
        return;
    }
    const auto cpuHandle =
        bloom::color::buildCpuDisplayProcessorForView(config, spec.display, spec.view);
    if (cpuHandle.handle() == nullptr) {
        expectations.expect(false, "the >4K CPU display oracle prepares");
        return;
    }
    auto armResult = GpuOcioDisplayArm::create(device);
    expectations.expect(armResult.hasValue(), "the >4K display arm hosts");
    if (!armResult) {
        return;
    }
    auto uploader = GpuImageUpload::create(device);
    if (!uploader) {
        expectations.expect(false, "the >4K uploader hosts");
        return;
    }
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the >4K input uploads within device capacity");
    if (input == nullptr) {
        return;
    }
    GpuOcioDisplayRequest request;
    request.command = prepared.command;
    request.input = input;
    request.width = width;
    request.height = height;
    request.byteBudget = kBudget;
    const auto begun = armResult.arm->begin(request);
    expectations.expect(begun.code == GpuOcioDisplayArmDiagnosticCode::None,
                        "the >4K geometry is admitted by real capacity, not a pixel interval");
    if (begun.code != GpuOcioDisplayArmDiagnosticCode::None) {
        return;
    }
    auto poll = armResult.arm->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = armResult.arm->poll();
    }
    expectations.expect(poll == GpuOcioExecutorPollResult::Ready,
                        "the >4K display dispatch completes");
    if (poll != GpuOcioExecutorPollResult::Ready) {
        return;
    }
    auto output = armResult.arm->takeDisplayImage();
    expectations.expect(output.has_value() && output->width() == width &&
                            output->height() == height,
                        "the >4K resident RGBA8 display is published at the exact geometry");
    expectations.expect(armResult.arm->counters().readbacks == 0,
                        "the >4K route reads back no full frame");
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return kSkipExit;
#else
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri);
    if (!revision.has_value()) {
        std::cout << "SKIP: the Bloom Neutral built-in is unavailable\n";
        return kSkipExit;
    }
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        *revision, std::string{});
    auto neutral = std::move(resolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral built-in does not resolve\n";
        return 1;
    }
    auto device = GpuDevice::create(bloom::render::GpuDeviceCreationOptions{});
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
    // Default display/view of the Bloom Neutral config: the ordinary production default now takes
    // the SAME general display arm (with no pixel-interval/4K gate). The >4K case below proves the
    // retired ceiling specifically.
    testDisplayPair(expectations, *device.device, preparer, *neutral,
                    std::string(neutral->displayName()), std::string(neutral->viewName()),
                    "default", false);
    // The baseline root: two real ACES 1.3 CG display/view pairs, each across the full
    // six-adjustment matrix, against the unchanged CPU oracle.
    const auto acesRevision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    std::size_t ranAcesViews = 0;
    if (acesRevision.has_value()) {
        auto acesResolution = bloom::color::resolveOcioBuiltIn(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
            *acesRevision, std::string{});
        auto aces = std::move(acesResolution).takeResolved();
        if (aces.has_value()) {
            for (const auto& candidate : aces->displays()) {
                auto cpuHandle = bloom::color::buildCpuDisplayProcessorForView(
                    *aces, candidate.display, candidate.view);
                if (cpuHandle.handle() == nullptr) {
                    continue;
                }
                testDisplayPair(expectations, *device.device, preparer, *aces, candidate.display,
                                candidate.view, "aces-view", true);
                ++ranAcesViews;
                if (ranAcesViews >= 2) {
                    break;
                }
            }
        }
    }
    expectations.expect(ranAcesViews >= 2,
                        "two ACES display/view pairs exercised the six-adjustment matrix");
    testLargeGeometry(expectations, *device.device, preparer, *neutral);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " GPU display arm expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: general GPU display arm native acceptance\n";
    return 0;
#endif
}
