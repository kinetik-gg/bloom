// Native acceptance for the runtime GPU output-colour stage: a real prepared OCIO command is
// dispatched, then the exact process RGBA32F payload and the encoded output are read back in one
// combined submission and compared against the unchanged CPU OCIO oracle. The identity arm proves a
// single process-only payload. Cancel and budget ceilings prove the retirement/budget model.
//
// A missing device or missing shader tools is an explicit SKIP (exit 77) unless --require-device.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_output_color.hpp>

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
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::GpuOutputColorArm;
using bloom::runtime::GpuOutputColorDiagnosticCode;
using bloom::runtime::GpuOutputColorPollResult;
using bloom::runtime::GpuOutputColorStage;
using bloom::runtime::GpuOutputColorStatus;

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

[[nodiscard]] GpuOutputColorStage* stageFor(Expectations& expectations, GpuDevice& device) {
    auto created = GpuOutputColorStage::create(device);
    expectations.expect(created.hasValue(), "the output-colour stage hosts");
    return created.hasValue() ? created.stage.release() : nullptr;
}

void testEffectStage(Expectations& expectations, GpuDevice& device,
                     GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Cst;
    spec.fromId = "ACES2065-1";
    spec.toId = "ACEScg";
    constexpr GpuOcioCommandGeometry geometry{4, 3};
    const auto prepared = preparer.prepare(*fixtures.aces, spec, geometry, fixtures.options);
    expectations.expect(prepared.hasValue(), "effect: the CST command prepares");
    if (!prepared) {
        return;
    }
    const std::unique_ptr<GpuOutputColorStage> stage(stageFor(expectations, device));
    auto uploader = GpuImageUpload::create(device);
    if (stage == nullptr || !uploader) {
        expectations.expect(false, "effect: stage and uploader exist");
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    expectations.expect(input != nullptr, "effect: the process image uploads");
    if (input == nullptr) {
        return;
    }

    const auto accepted = stage->begin(prepared.command, input, kBudget);
    expectations.expect(accepted.code == GpuOutputColorDiagnosticCode::None,
                        "effect: begin accepts the command");
    auto poll = stage->poll();
    while (poll == GpuOutputColorPollResult::Pending) {
        poll = stage->poll();
    }
    expectations.expect(poll == GpuOutputColorPollResult::Ready, "effect: the job completes");
    auto frame = stage->take();
    expectations.expect(frame.has_value(), "effect: a frame is published");
    if (!frame.has_value()) {
        return;
    }
    expectations.expect(frame->arm == GpuOutputColorArm::ProcessEffect,
                        "effect: the ProcessEffect arm is selected");
    expectations.expect(frame->counters.readbackSubmissions == 1 &&
                            frame->counters.transferredPayloads == 2,
                        "effect: one submission carries the process + encoded payloads");
    expectations.expect(frame->counters.processPayloadBytes ==
                            static_cast<std::uint64_t>(geometry.width) * geometry.height * 16U,
                        "effect: the process payload byte count is exact");
    expectations.expect(frame->counters.encodedPayloadBytes ==
                            static_cast<std::uint64_t>(geometry.width) * geometry.height * 16U,
                        "effect: the encoded payload byte count is exact");
    expectations.expect(frame->counters.transferredBytes == frame->counters.processPayloadBytes +
                                                                frame->counters.encodedPayloadBytes,
                        "effect: bytes are the sum, never a folded single transfer");
    expectations.expect(frame->counters.commandAccepted == 1 &&
                            frame->counters.programCreations == 1 &&
                            frame->counters.dispatches == 1,
                        "effect: cold job creates one program and dispatches once");

    // The process payload is bit-identical to the uploaded source pixels.
    bool processMismatch = frame->process.size() != pixels.size();
    if (!processMismatch) {
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            if (frame->process[index].red() != pixels[index].red() ||
                frame->process[index].green() != pixels[index].green() ||
                frame->process[index].blue() != pixels[index].blue() ||
                frame->process[index].alpha() != pixels[index].alpha()) {
                processMismatch = true;
                break;
            }
        }
    }
    expectations.expect(!processMismatch,
                        "effect: the process payload is the unchanged process bits");

    const auto cpu =
        bloom::color::CpuColorSpaceProcessor::prepare(*fixtures.aces, "ACES2065-1", "ACEScg");
    expectations.expect(cpu.succeeded(), "effect: the CPU oracle prepares");
    if (cpu) {
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
            const auto& gpu = frame->effectRgba32f[index];
            if (!close(gpu.red(), straight[0] * p.alpha()) ||
                !close(gpu.green(), straight[1] * p.alpha()) ||
                !close(gpu.blue(), straight[2] * p.alpha()) || gpu.alpha() != p.alpha()) {
                ++mismatches;
            }
        }
        expectations.expect(mismatches == 0,
                            "effect: every encoded pixel is within 2e-6 of the CPU oracle");
    }

    // Warm: the same command reuses the retained native program with zero creation.
    const auto warmAccepted = stage->begin(prepared.command, input, kBudget);
    expectations.expect(warmAccepted.code == GpuOutputColorDiagnosticCode::None,
                        "effect: the warm job is accepted");
    auto warmPoll = stage->poll();
    while (warmPoll == GpuOutputColorPollResult::Pending) {
        warmPoll = stage->poll();
    }
    auto warmFrame = stage->take();
    expectations.expect(warmPoll == GpuOutputColorPollResult::Ready && warmFrame.has_value(),
                        "effect: the warm job completes");
    if (warmFrame.has_value()) {
        expectations.expect(warmFrame->counters.programCreations == 0 &&
                                warmFrame->counters.programReuses == 1,
                            "effect: a warm frame reuses the retained program (zero creation)");
        expectations.expect(warmFrame->counters.transferredPayloads == 2,
                            "effect: the warm job still transfers both payloads");
    }

    // A colour job followed by an identity job on the SAME stage must report zero new colour
    // program creation/dispatch and exactly one actual process payload.
    const auto identityAccepted = stage->begin(nullptr, input, kBudget);
    expectations.expect(identityAccepted.code == GpuOutputColorDiagnosticCode::None,
                        "effect: the identity job is accepted after a colour job");
    auto identityPoll = stage->poll();
    while (identityPoll == GpuOutputColorPollResult::Pending) {
        identityPoll = stage->poll();
    }
    auto identityFrame = stage->take();
    expectations.expect(identityPoll == GpuOutputColorPollResult::Ready &&
                            identityFrame.has_value(),
                        "effect: the identity job completes");
    if (identityFrame.has_value()) {
        expectations.expect(identityFrame->arm == GpuOutputColorArm::None,
                            "effect: the identity arm is selected");
        expectations.expect(identityFrame->counters.commandAccepted == 0 &&
                                identityFrame->counters.programCreations == 0 &&
                                identityFrame->counters.programReuses == 0 &&
                                identityFrame->counters.dispatches == 0,
                            "effect: identity after colour reports zero new colour work");
        expectations.expect(identityFrame->counters.readbackSubmissions == 1 &&
                                identityFrame->counters.transferredPayloads == 1 &&
                                identityFrame->counters.encodedPayloadBytes == 0,
                            "effect: identity after colour transfers exactly one process payload");
    }
}

void testDisplayStage(Expectations& expectations, GpuDevice& device,
                      GpuOcioProgramPreparer& preparer, const Fixtures& fixtures) {
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = std::string(fixtures.neutral->displayName());
    spec.view = std::string(fixtures.neutral->viewName());
    constexpr GpuOcioCommandGeometry geometry{5, 2};
    const auto prepared = preparer.prepare(*fixtures.neutral, spec, geometry, fixtures.options);
    expectations.expect(prepared.hasValue(), "display: the command prepares");
    if (!prepared) {
        return;
    }
    expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::DisplayRgba8,
                        "display: the command is DisplayRgba8");
    const std::unique_ptr<GpuOutputColorStage> stage(stageFor(expectations, device));
    auto uploader = GpuImageUpload::create(device);
    if (stage == nullptr || !uploader) {
        expectations.expect(false, "display: stage and uploader exist");
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "display: the process image uploads");
        return;
    }
    expectations.expect(stage->begin(prepared.command, input, kBudget).code ==
                            GpuOutputColorDiagnosticCode::None,
                        "display: begin accepts the command");
    auto poll = stage->poll();
    while (poll == GpuOutputColorPollResult::Pending) {
        poll = stage->poll();
    }
    auto frame = stage->take();
    expectations.expect(poll == GpuOutputColorPollResult::Ready && frame.has_value(),
                        "display: the job completes");
    if (!frame.has_value()) {
        return;
    }
    expectations.expect(frame->arm == GpuOutputColorArm::DisplayRgba8,
                        "display: the DisplayRgba8 arm is selected");
    expectations.expect(frame->counters.transferredPayloads == 2 &&
                            frame->counters.encodedPayloadBytes ==
                                static_cast<std::uint64_t>(geometry.width) * geometry.height * 4U,
                        "display: the combined readback carries an exact RGBA8 payload");
    const auto handle = bloom::color::buildBloomNeutralCpuDisplayProcessor(*fixtures.neutral);
    const auto* const processor = handle.handle();
    expectations.expect(processor != nullptr, "display: the CPU oracle prepares");
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
        const auto& gpu = frame->displayRgba8[index];
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
                        "display: every display pixel is within one RGBA8 code of the CPU oracle");
    expectations.expect(alphaMismatches == 0, "display: alpha is byte-exact");
}

void testIdentityAndCeilings(Expectations& expectations, GpuDevice& device) {
    const std::unique_ptr<GpuOutputColorStage> stage(stageFor(expectations, device));
    auto uploader = GpuImageUpload::create(device);
    if (stage == nullptr || !uploader) {
        expectations.expect(false, "identity: stage and uploader exist");
        return;
    }
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 4;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "identity: the process image uploads");
        return;
    }

    // Identity: a null command runs no OCIO program and transfers ONE payload.
    expectations.expect(stage->begin(nullptr, input, kBudget).code ==
                            GpuOutputColorDiagnosticCode::None,
                        "identity: begin accepts a null command");
    auto poll = stage->poll();
    while (poll == GpuOutputColorPollResult::Pending) {
        poll = stage->poll();
    }
    auto frame = stage->take();
    expectations.expect(poll == GpuOutputColorPollResult::Ready && frame.has_value(),
                        "identity: the job completes");
    if (frame.has_value()) {
        expectations.expect(frame->arm == GpuOutputColorArm::None,
                            "identity: the identity arm is selected");
        expectations.expect(frame->counters.readbackSubmissions == 1 &&
                                frame->counters.transferredPayloads == 1 &&
                                frame->counters.encodedPayloadBytes == 0,
                            "identity: exactly one process payload, no extra transfer");
        expectations.expect(frame->counters.commandAccepted == 0 &&
                                frame->counters.programCreations == 0,
                            "identity: no OCIO program ran");
        expectations.expect(frame->process.size() == pixels.size(),
                            "identity: the process payload is present");
    }

    // A budget below the actual concurrent peak is a typed OverBudget and leaves the stage usable.
    const auto refused = stage->begin(nullptr, input, 1);
    expectations.expect(refused.code == GpuOutputColorDiagnosticCode::OverBudget,
                        "ceiling: an under-peak budget is a typed OverBudget");
    expectations.expect(stage->state() == GpuOutputColorStatus::Idle,
                        "ceiling: a refused begin leaves the stage reusable");
    auto recovered = stage->begin(nullptr, input, kBudget);
    expectations.expect(recovered.code == GpuOutputColorDiagnosticCode::None,
                        "ceiling: the stage is reusable after a budget refusal");
    auto recoveredPoll = stage->poll();
    while (recoveredPoll == GpuOutputColorPollResult::Pending) {
        recoveredPoll = stage->poll();
    }
    expectations.expect(recoveredPoll == GpuOutputColorPollResult::Ready,
                        "ceiling: the recovered job completes");
    static_cast<void>(stage->take());
}

void testCancel(Expectations& expectations, GpuDevice& device) {
    const std::unique_ptr<GpuOutputColorStage> stage(stageFor(expectations, device));
    auto uploader = GpuImageUpload::create(device);
    if (stage == nullptr || !uploader) {
        expectations.expect(false, "cancel: stage and uploader exist");
        return;
    }
    constexpr std::uint32_t width = 32;
    constexpr std::uint32_t height = 32;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "cancel: the process image uploads");
        return;
    }
    expectations.expect(stage->begin(nullptr, input, kBudget).code ==
                            GpuOutputColorDiagnosticCode::None,
                        "cancel: begin accepts the job");
    stage->cancel();
    auto poll = stage->poll();
    while (poll == GpuOutputColorPollResult::Pending) {
        poll = stage->poll();
    }
    expectations.expect(poll == GpuOutputColorPollResult::Failure &&
                            stage->diagnostic().code == GpuOutputColorDiagnosticCode::Cancelled,
                        "cancel: a cancelled job publishes no frame");
    expectations.expect(!stage->take().has_value(), "cancel: take returns nothing");
    expectations.expect(!stage->hasUnretiredSubmission(),
                        "cancel: the cancelled submission retires after fence proof");

    // A foreign-thread begin after the retired failure must report WrongThread without mutating the
    // owner's state (the stage must still be reusable by the real owner).
    bool foreignBeginWrongThread = false;
    std::thread foreign([&] {
        const auto result = stage->begin(nullptr, input, kBudget);
        foreignBeginWrongThread = result.code == GpuOutputColorDiagnosticCode::WrongThread;
    });
    foreign.join();
    expectations.expect(foreignBeginWrongThread,
                        "cancel: a foreign-thread begin reports WrongThread");
    expectations.expect(stage->state() == GpuOutputColorStatus::Failure &&
                            !stage->hasUnretiredSubmission(),
                        "cancel: the foreign begin did not mutate owner state");

    // After genuine retirement the same owning stage accepts and completes a new request.
    const auto recovered = stage->begin(nullptr, input, kBudget);
    expectations.expect(recovered.code == GpuOutputColorDiagnosticCode::None,
                        "cancel: the stage is reusable after the cancelled job retires");
    auto recoveredPoll = stage->poll();
    while (recoveredPoll == GpuOutputColorPollResult::Pending) {
        recoveredPoll = stage->poll();
    }
    expectations.expect(recoveredPoll == GpuOutputColorPollResult::Ready &&
                            stage->take().has_value(),
                        "cancel: the follow-up request completes");
}

void testWrongThread(Expectations& expectations, GpuDevice& device) {
    const std::unique_ptr<GpuOutputColorStage> stage(stageFor(expectations, device));
    auto uploader = GpuImageUpload::create(device);
    if (stage == nullptr || !uploader) {
        expectations.expect(false, "wrong-thread: stage and uploader exist");
        return;
    }
    const auto pixels = fixturePixels(4, 4);
    const auto input = uploadImage(*uploader.upload, 4, 4, pixels);
    if (input == nullptr) {
        expectations.expect(false, "wrong-thread: the process image uploads");
        return;
    }
    expectations.expect(stage->begin(nullptr, input, kBudget).code ==
                            GpuOutputColorDiagnosticCode::None,
                        "wrong-thread: begin accepts the job");
    std::thread foreign([&] {
        const auto result = stage->poll();
        expectations.expect(result == GpuOutputColorPollResult::WrongThread,
                            "wrong-thread: a foreign poll reports WrongThread");
    });
    foreign.join();
    auto poll = stage->poll();
    while (poll == GpuOutputColorPollResult::Pending) {
        poll = stage->poll();
    }
    expectations.expect(poll == GpuOutputColorPollResult::Ready,
                        "wrong-thread: the owner still completes the job");

    // A foreign-thread take must not steal the owner's Ready frame, and a foreign cancel must not
    // touch it.
    bool foreignTakeReturnedFrame = true;
    std::thread thief([&] {
        foreignTakeReturnedFrame = stage->take().has_value();
        stage->cancel();
    });
    thief.join();
    expectations.expect(!foreignTakeReturnedFrame, "wrong-thread: a foreign take returns nothing");
    expectations.expect(stage->state() == GpuOutputColorStatus::Ready,
                        "wrong-thread: foreign take/cancel left the Ready frame intact");
    expectations.expect(stage->take().has_value(), "wrong-thread: the owner still takes the frame");
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
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
    testEffectStage(expectations, *device.device, preparer, fixtures);
    testDisplayStage(expectations, *device.device, preparer, fixtures);
    testIdentityAndCeilings(expectations, *device.device);
    testCancel(expectations, *device.device);
    testWrongThread(expectations, *device.device);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " output-colour expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: runtime GPU output-colour stage\n";
    return 0;
#endif
}
