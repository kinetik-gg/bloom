// Resident Neutral display, subnormal rejection/reset and eligibility timing for the
// resident-preview qualification. See the public header for the contract; Vulkan-free.

#include "gpu_resident_preview_qualification_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime::resident_preview_detail {

using core::PixelAspectRatio;
using render::GpuCompositeDiagnosticCode;
using render::GpuDisplayImageReadbackCode;
using render::GpuImage;
using render::GpuImageUploadDiagnosticCode;
using render::GpuResidentDisplayDiagnosticCode;
using render::GpuSolidDiagnosticCode;
using render::GpuSolidParameters;
using render::ImageWindow;
using render::Rgba32f;
using render::Rgba32fImage;
using render::Rgba8;

ProbeResult verifyResidentDisplayParity(ProbeContext& context) noexcept {
    struct Case final {
        std::uint32_t width;
        std::uint32_t height;
        bool patternB;
    };
    const std::array<Case, 4> cases{{
        {257, 19, false},
        {257, 19, true},
        {1280, 720, false},
        {1280, 720, true},
    }};
    for (const Case& testCase : cases) {
        const auto window = makeWindow(0, 0, testCase.width, testCase.height);
        const auto pixels = detail::makeResidentPreviewNonUniformPixels(
            testCase.width, testCase.height, testCase.patternB);
        if (!window || !pixels) {
            return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                        "a resident display fixture did not build");
        }
        auto image = buildImage(*window, *window, PixelAspectRatio::square(), *pixels);
        if (!image) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "a resident display CPU image did not build");
        }
        // Keep the owning image alive for the whole iteration so the borrowed view stays valid; the
        // upload copies pixels into its own staging buffer and retains the shared owner.
        auto source = std::make_shared<const Rgba32fImage>(std::move(*image));
        const auto view = source->view();
        if (!view) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "a resident display CPU view did not build");
        }
        std::shared_ptr<const GpuImage> residentResult;
        {
            std::string reason;
            const auto uploaded = uploadToResident(context, source, residentResult, reason);
            if (uploaded.code != GpuResidentPreviewDiagnosticCode::None) {
                return uploaded;
            }
        }
        const auto shared = std::move(residentResult);
        std::vector<Rgba8> measured;
        std::string reason;
        auto ran = runDisplayReadback(context, shared, measured, reason);
        if (ran.code != GpuResidentPreviewDiagnosticCode::None) {
            return ran;
        }
        const render::GpuDisplayImage* output = context.display.image();
        if (output == nullptr || output->dataWindow() != *window ||
            output->displayWindow() != *window ||
            output->pixelAspect() != PixelAspectRatio::square()) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "the resident display did not preserve its data/display window and PAR");
        }
        auto cpu = color::produceBloomNeutralDisplayFrame(
            context.processor, *view.value(), kGpuResidentPreviewCpuChunkPixelCount,
            std::numeric_limits<std::size_t>::max(), context.isCancelled);
        if (!cpu) {
            return fail(context.isCancelled() ? GpuResidentPreviewDiagnosticCode::Cancelled
                                              : GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "the CPU OCIO display oracle rejected the fixture");
        }
        if (!displayMatches(measured, cpu.value()->pixels(), reason)) {
            return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                        "resident display parity failed: " + reason);
        }
        hashPixels(context, testCase.width, testCase.height, *pixels);
    }
    return ok();
}

ProbeResult verifySubnormalRejection(ProbeContext& context, bool& subnormalRejected) noexcept {
    const auto window = makeWindow(0, 0, 4, 4);
    if (!window) {
        return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                    "the subnormal fixture window did not build");
    }
    std::vector<Rgba32f> pixels(16, Rgba32f::transparent());
    const auto subnormal =
        Rgba32f::fromPremultiplied(std::numeric_limits<float>::denorm_min(), 0.0F, 0.0F, 1.0F);
    if (!subnormal) {
        return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                    "the subnormal fixture pixel was rejected by the CPU value type");
    }
    pixels[0] = *subnormal.value();
    auto image = buildImage(*window, *window, PixelAspectRatio::square(), pixels);
    if (!image) {
        return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                    "the subnormal CPU image did not build");
    }
    std::shared_ptr<const GpuImage> resident;
    {
        std::string reason;
        auto source = std::make_shared<const Rgba32fImage>(std::move(*image));
        const auto uploaded = uploadToResident(context, std::move(source), resident, reason);
        if (uploaded.code != GpuResidentPreviewDiagnosticCode::None) {
            return uploaded;
        }
    }
    const auto shared = std::move(resident);
    const auto begin = context.display.begin(shared, context.imageBudget);
    if (begin.code != GpuResidentDisplayDiagnosticCode::None) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure,
                    "the subnormal resident display begin was rejected early: " + begin.message);
    }
    std::string reason;
    const auto status =
        waitFor(context.display, context.isCancelled, context.deadlineNanoseconds, reason);
    if (status == NativeStatus::Ok) {
        static_cast<void>(context.display.takeImage());
        subnormalRejected = false;
        return fail(GpuResidentPreviewDiagnosticCode::ParityFailure,
                    "the resident display published a frame containing a nonzero subnormal value");
    }
    if (status != NativeStatus::Failed) {
        return waitFailure(status, reason);
    }
    if (context.display.diagnostic().code != GpuResidentDisplayDiagnosticCode::ShaderRejected) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure,
                    "the subnormal frame failed with an unexpected diagnostic: " + reason);
    }
    static_cast<void>(context.display.takeImage());
    subnormalRejected = true;
    // After the whole-frame rejection the pipeline must still accept a normal frame; this exercises
    // the status-word reset the dispatch fix introduced.
    const auto recoveryWindow = makeWindow(0, 0, 257, 19);
    const auto recoveryPixels = detail::makeResidentPreviewNonUniformPixels(257, 19, false);
    if (!recoveryWindow || !recoveryPixels) {
        return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                    "the status-reset recovery fixture did not build");
    }
    auto recoveryImage =
        buildImage(*recoveryWindow, *recoveryWindow, PixelAspectRatio::square(), *recoveryPixels);
    if (!recoveryImage) {
        return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                    "the status-reset recovery CPU image did not build");
    }
    auto recoverySource = std::make_shared<const Rgba32fImage>(std::move(*recoveryImage));
    std::shared_ptr<const GpuImage> recoveryResident;
    std::string recoveryReason;
    auto recoveryUpload =
        uploadToResident(context, std::move(recoverySource), recoveryResident, recoveryReason);
    if (recoveryUpload.code != GpuResidentPreviewDiagnosticCode::None) {
        return recoveryUpload;
    }
    std::vector<Rgba8> recovered;
    const auto recovery = runDisplayReadback(context, recoveryResident, recovered, recoveryReason);
    if (recovery.code != GpuResidentPreviewDiagnosticCode::None) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure,
                    "the resident display did not recover after a subnormal rejection: " +
                        recoveryReason);
    }
    return ok();
}

ProbeResult measureEligibility(ProbeContext& context,
                               std::vector<GpuResidentPreviewTimingSample>& outTimings) noexcept {
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 5> kSizes{{
        {256, 144},
        {640, 360},
        {1280, 720},
        {1920, 1080},
        {3840, 2160},
    }};
    constexpr std::size_t kPairCount = 3;
    for (const auto& [width, height] : kSizes) {
        if (context.isCancelled()) {
            return fail(GpuResidentPreviewDiagnosticCode::Cancelled,
                        "cancellation arrived before a measured size");
        }
        const auto window = makeWindow(0, 0, width, height);
        const auto pixels = detail::makeResidentPreviewMeasuredPixels(width, height);
        if (!window || !pixels) {
            return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                        "a measured fixture did not build");
        }
        auto image = buildImage(*window, *window, PixelAspectRatio::square(), *pixels);
        if (!image) {
            return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant,
                        "a measured CPU image did not build");
        }
        const auto source = std::make_shared<const Rgba32fImage>(std::move(*image));
        const auto view = source->view();
        if (!view) {
            return fail(GpuResidentPreviewDiagnosticCode::CpuOracleFailure,
                        "a measured CPU view did not build");
        }
        const auto nativeRun = [&]() -> bool {
            std::string reason;
            std::shared_ptr<const GpuImage> uploaded;
            if (uploadToResident(context, source, uploaded, reason).code !=
                GpuResidentPreviewDiagnosticCode::None) {
                return false;
            }
            if (context.display.begin(std::move(uploaded), context.imageBudget).code !=
                GpuResidentDisplayDiagnosticCode::None) {
                return false;
            }
            if (waitFor(context.display, context.isCancelled, context.deadlineNanoseconds,
                        reason) != NativeStatus::Ok) {
                return false;
            }
            static_cast<void>(context.display.takeImage());
            return true;
        };
        const auto cpuRun = [&]() -> bool {
            auto displayed = color::produceBloomNeutralDisplayFrame(
                context.processor, *view.value(), kGpuResidentPreviewCpuChunkPixelCount,
                std::numeric_limits<std::size_t>::max(), context.isCancelled);
            return static_cast<bool>(displayed);
        };
        if (!nativeRun() || !cpuRun()) {
            return fail(GpuResidentPreviewDiagnosticCode::NativeFailure,
                        "a measured warmup dispatch failed");
        }
        std::array<double, kPairCount> nativeMs{};
        std::array<double, kPairCount> cpuMs{};
        for (std::size_t pair = 0; pair < kPairCount; ++pair) {
            if (context.isCancelled()) {
                return fail(GpuResidentPreviewDiagnosticCode::Cancelled,
                            "cancellation arrived during a measured pair");
            }
            const bool nativeFirst = (pair % 2U) == 0U;
            double nativeValue = 0.0;
            double cpuValue = 0.0;
            const auto measureNative = [&]() -> bool {
                const auto start = std::chrono::steady_clock::now();
                const bool ran = nativeRun();
                nativeValue = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
                return ran;
            };
            const auto measureCpu = [&]() -> bool {
                const auto start = std::chrono::steady_clock::now();
                const bool ran = cpuRun();
                cpuValue = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start)
                               .count();
                return ran;
            };
            const bool okPair =
                nativeFirst ? (measureNative() && measureCpu()) : (measureCpu() && measureNative());
            if (!okPair) {
                return fail(GpuResidentPreviewDiagnosticCode::NativeFailure,
                            "a measured pair failed");
            }
            nativeMs[pair] = nativeValue;
            cpuMs[pair] = cpuValue;
        }
        std::sort(nativeMs.begin(), nativeMs.end());
        std::sort(cpuMs.begin(), cpuMs.end());
        GpuResidentPreviewTimingSample sample;
        sample.width = width;
        sample.height = height;
        sample.pixel_count = static_cast<std::uint64_t>(width) * height;
        sample.native_full_ms = nativeMs[kPairCount / 2];
        sample.cpu_full_ms = cpuMs[kPairCount / 2];
        sample.native_improved = sample.native_full_ms < sample.cpu_full_ms;
        outTimings.push_back(sample);
    }
    return ok();
}

} // namespace bloom::runtime::resident_preview_detail
