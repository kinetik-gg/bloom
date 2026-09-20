// Focused native acceptance for the production combined output-colour readback primitive:
//  * a real begin -> poll -> take with a process RGBA32F payload and an encoded RGBA32F effect
//    payload, proving ONE submission carries TWO payloads and both are bit-for-bit the resident
//    images;
//  * a process-only identity readback (ONE payload, no encoded bytes);
//  * an encoded RGBA8 display payload from a real GpuResidentDisplay image;
//  * a pre-submit budget refusal that leaves the object reusable;
//  * cancellation that publishes no payload and proves retirement.
//
// Compiled against the frozen private headers exactly like the process-readback native proof.

#include "gpu_composite_native_support.hpp"

#include <bloom/render/gpu_output_color_readback.hpp>
#include <bloom/render/gpu_process_readback.hpp>
#include <bloom/render/gpu_resident_display.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace bloom::render::composite_proof;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuOutputColorReadback;
using bloom::render::GpuOutputColorReadbackCode;
using bloom::render::GpuOutputColorReadbackState;
using bloom::render::GpuProcessReadback;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayJobState;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::Rgba32f;
using bloom::render::Rgba8;

[[nodiscard]] std::shared_ptr<const GpuImage> solidImage(GpuSolid& solidOp, const Color4d color,
                                                         const std::uint32_t width,
                                                         const std::uint32_t height) {
    const auto w = window(0, 0, width, height);
    auto image = solid(solidOp, color, w, w, PixelAspectRatio::square());
    if (!image) {
        return nullptr;
    }
    return std::make_shared<const GpuImage>(std::move(*image));
}

[[nodiscard]] std::vector<Rgba32f> oraclePixels(const GpuImage& image) {
    const auto readback = bloom::render::readbackResidentImage(image, kBudget);
    return readback.hasValue() ? readback.pixels : std::vector<Rgba32f>{};
}

void testCombinedProcessAndEffect(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    expectations.expect(solidOp.hasValue(), "combined: solid op created");
    if (!solidOp) {
        return;
    }
    const auto process = solidImage(*solidOp.solid, {0.4, 0.6, 0.2, 1.0}, 8, 4);
    const auto effect = solidImage(*solidOp.solid, {0.1, 0.3, 0.9, 0.5}, 8, 4);
    expectations.expect(process != nullptr && effect != nullptr, "combined: images built");
    if (process == nullptr || effect == nullptr) {
        return;
    }
    const auto processOracle = oraclePixels(*process);
    const auto effectOracle = oraclePixels(*effect);

    GpuOutputColorReadback readback;
    expectations.expect(readback.begin(process, effect, std::nullopt, kBudget),
                        "combined: begin accepted");
    expectations.expect(readback.state() == GpuOutputColorReadbackState::Pending,
                        "combined: begin leaves Pending");
    GpuOutputColorReadbackState poll = readback.state();
    while (poll == GpuOutputColorReadbackState::Pending) {
        poll = readback.poll();
    }
    expectations.expect(poll == GpuOutputColorReadbackState::Ready, "combined: poll reaches Ready");
    auto payloads = readback.take();
    expectations.expect(payloads.counters.submissions == 1,
                        "combined: exactly one device-to-host submission");
    expectations.expect(payloads.counters.payloads == 2,
                        "combined: the one submission carries two payloads");
    expectations.expect(payloads.counters.processBytes == 8U * 4U * sizeof(Rgba32f),
                        "combined: the process payload byte count is exact");
    expectations.expect(payloads.counters.encodedBytes == 8U * 4U * sizeof(Rgba32f),
                        "combined: the encoded payload byte count is exact");
    expectations.expect(payloads.counters.bytes ==
                            payloads.counters.processBytes + payloads.counters.encodedBytes,
                        "combined: total bytes are the two payloads, not a folded single transfer");
    static_cast<void>(
        pixelsMatch(payloads.process, processOracle, expectations, "combined process"));
    static_cast<void>(
        pixelsMatch(payloads.encodedRgba32f, effectOracle, expectations, "combined effect"));
    expectations.expect(payloads.encodedRgba8.empty(), "combined: no RGBA8 arm was staged");
    expectations.expect(readback.state() == GpuOutputColorReadbackState::Idle,
                        "combined: take resets to Idle");
}

void testProcessOnlyIdentity(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    if (!solidOp) {
        expectations.expect(false, "identity: solid op created");
        return;
    }
    const auto process = solidImage(*solidOp.solid, {0.7, 0.2, 0.1, 1.0}, 5, 3);
    if (process == nullptr) {
        expectations.expect(false, "identity: image built");
        return;
    }
    const auto oracle = oraclePixels(*process);
    GpuOutputColorReadback readback;
    expectations.expect(readback.begin(process, nullptr, std::nullopt, kBudget),
                        "identity: begin accepted");
    GpuOutputColorReadbackState poll = readback.state();
    while (poll == GpuOutputColorReadbackState::Pending) {
        poll = readback.poll();
    }
    expectations.expect(poll == GpuOutputColorReadbackState::Ready, "identity: poll reaches Ready");
    auto payloads = readback.take();
    expectations.expect(payloads.counters.submissions == 1, "identity: exactly one submission");
    expectations.expect(payloads.counters.payloads == 1,
                        "identity: one payload only (no extra encoded work)");
    expectations.expect(payloads.counters.encodedBytes == 0, "identity: no encoded bytes");
    static_cast<void>(pixelsMatch(payloads.process, oracle, expectations, "identity process"));
}

void testEncodedDisplay(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    if (!solidOp) {
        expectations.expect(false, "display: solid op created");
        return;
    }
    auto display = GpuResidentDisplay::create(device);
    expectations.expect(display.hasValue(), "display: resident display created");
    if (!display) {
        return;
    }
    const auto process = solidImage(*solidOp.solid, {0.2, 0.8, 0.4, 1.0}, 6, 2);
    if (process == nullptr) {
        expectations.expect(false, "display: process image built");
        return;
    }
    expectations.expect(display.display->begin(process, kBudget).code ==
                            bloom::render::GpuResidentDisplayDiagnosticCode::None,
                        "display: display dispatch accepted");
    auto displayPoll = display.display->poll();
    while (displayPoll == GpuResidentDisplayPollResult::Pending) {
        displayPoll = display.display->poll();
    }
    expectations.expect(displayPoll == GpuResidentDisplayPollResult::Ready,
                        "display: display dispatch completes");
    auto displayImage = display.display->takeImage();
    expectations.expect(displayImage.isValid(), "display: resident RGBA8 image published");
    if (!displayImage.isValid()) {
        return;
    }

    GpuOutputColorReadback readback;
    expectations.expect(readback.begin(process, nullptr, std::move(displayImage), kBudget),
                        "display: combined begin accepted");
    GpuOutputColorReadbackState poll = readback.state();
    while (poll == GpuOutputColorReadbackState::Pending) {
        poll = readback.poll();
    }
    expectations.expect(poll == GpuOutputColorReadbackState::Ready, "display: poll reaches Ready");
    auto payloads = readback.take();
    expectations.expect(payloads.counters.submissions == 1 && payloads.counters.payloads == 2,
                        "display: one submission carries process + RGBA8");
    expectations.expect(payloads.counters.encodedBytes == 6U * 2U * sizeof(Rgba8),
                        "display: the RGBA8 payload byte count is exact");
    expectations.expect(payloads.process.size() == 6U * 2U,
                        "display: the process payload is present");
    expectations.expect(payloads.encodedRgba8.size() == 6U * 2U,
                        "display: the RGBA8 payload is present");
    expectations.expect(payloads.encodedRgba32f.empty(),
                        "display: no RGBA32F encoded arm was staged");
}

void testPreSubmitRefusals(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    if (!solidOp) {
        expectations.expect(false, "refusal: solid op created");
        return;
    }
    const auto process = solidImage(*solidOp.solid, {0.5, 0.5, 0.5, 1.0}, 8, 8);
    const auto effect = solidImage(*solidOp.solid, {0.1, 0.1, 0.1, 1.0}, 8, 8);
    if (process == nullptr || effect == nullptr) {
        expectations.expect(false, "refusal: images built");
        return;
    }
    GpuOutputColorReadback both;
    expectations.expect(!both.begin(process, effect, std::nullopt, 0) &&
                            both.diagnostic().code == GpuOutputColorReadbackCode::OverBudget,
                        "refusal: an under-peak budget is a typed OverBudget");
    GpuOutputColorReadback valid;
    expectations.expect(valid.begin(process, effect, std::nullopt, kBudget),
                        "refusal: the object stays usable after a refusal");
    GpuOutputColorReadbackState poll = valid.state();
    while (poll == GpuOutputColorReadbackState::Pending) {
        poll = valid.poll();
    }
    expectations.expect(poll == GpuOutputColorReadbackState::Ready,
                        "refusal: the follow-up completes");
    static_cast<void>(valid.take());
}

void testCancellation(Expectations& expectations, GpuDevice& device) {
    auto solidOp = GpuSolid::create(device);
    if (!solidOp) {
        expectations.expect(false, "cancel: solid op created");
        return;
    }
    const auto process = solidImage(*solidOp.solid, {0.3, 0.3, 0.9, 1.0}, 16, 16);
    if (process == nullptr) {
        expectations.expect(false, "cancel: image built");
        return;
    }
    GpuOutputColorReadback readback;
    expectations.expect(readback.begin(process, nullptr, std::nullopt, kBudget),
                        "cancel: begin accepted");
    readback.cancel();
    GpuOutputColorReadbackState poll = readback.state();
    while (poll == GpuOutputColorReadbackState::Pending) {
        poll = readback.poll();
    }
    expectations.expect(poll == GpuOutputColorReadbackState::Failure &&
                            readback.diagnostic().code == GpuOutputColorReadbackCode::Cancelled,
                        "cancel: a cancelled readback publishes no payload");
    expectations.expect(!readback.hasUnretiredSubmission(),
                        "cancel: the cancelled submission retires after fence proof");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 0;
        }
        testCombinedProcessAndEffect(expectations, *device.device);
        testProcessOnlyIdentity(expectations, *device.device);
        testEncodedDisplay(expectations, *device.device);
        testPreSubmitRefusals(expectations, *device.device);
        testCancellation(expectations, *device.device);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " combined readback expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: combined output-colour readback\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
