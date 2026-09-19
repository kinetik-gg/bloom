#pragma once

// Internal helpers for the resident-preview qualification. Not a public contract. The shared
// helpers and operation drivers are inline so the parity implementation can be split across small
// translation units; the report factory lives in the orchestration source.

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::runtime::resident_preview_detail {

struct ProbeContext final {
    const color::PreparedCpuDisplayProcessorHandle& processor;
    render::GpuSolid& solid;
    render::GpuImageUpload& upload;
    render::GpuComposite& composite;
    render::GpuResidentDisplay& display;
    std::uint64_t imageBudget = 0;
    std::uint64_t metadataBudget = 0;
    std::uint64_t deadlineNanoseconds = 0;
    color::CancellationPredicateRef isCancelled;
    core::Sha256Hasher* fixtureHasher = nullptr;
};

// None on success; any other code is a qualification failure.
struct ProbeResult final {
    GpuResidentPreviewDiagnosticCode code = GpuResidentPreviewDiagnosticCode::None;
    std::string message;
};

enum class NativeStatus : std::uint8_t { Ok, Cancelled, Timeout, Failed };

[[nodiscard]] inline ProbeResult ok() noexcept { return {}; }

[[nodiscard]] inline ProbeResult fail(const GpuResidentPreviewDiagnosticCode code,
                                      std::string message) {
    return ProbeResult{code, std::move(message)};
}

[[nodiscard]] inline ProbeResult waitFailure(const NativeStatus status, const std::string& reason) {
    switch (status) {
    case NativeStatus::Timeout:
        return fail(GpuResidentPreviewDiagnosticCode::NativeTimeout, reason);
    case NativeStatus::Cancelled:
        return fail(GpuResidentPreviewDiagnosticCode::Cancelled, reason);
    case NativeStatus::Failed:
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, reason);
    case NativeStatus::Ok:
        break;
    }
    return ok();
}

// Bounded poll loop: checks cancellation before poll and the deadline before poll, and marks the
// dispatch discarded on either. On a Failure after our own cancellation it reports Cancelled.
template <typename Pipeline>
[[nodiscard]] inline NativeStatus
waitFor(Pipeline& pipeline, const color::CancellationPredicateRef isCancelled,
        const std::uint64_t deadlineNanoseconds, std::string& reason) {
    using Poll = decltype(pipeline.poll());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::nanoseconds(deadlineNanoseconds);
    for (;;) {
        if (isCancelled()) {
            pipeline.cancel();
            reason = "the native dispatch was cancelled before the fence retired";
            return NativeStatus::Cancelled;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            pipeline.cancel();
            reason = "the native dispatch exceeded the bounded deadline; drain the pipeline on the "
                     "owner thread before reuse";
            return NativeStatus::Timeout;
        }
        const Poll poll = pipeline.poll();
        if (poll == Poll::Pending) {
            std::this_thread::yield();
            continue;
        }
        if (poll == Poll::Failure) {
            reason = pipeline.diagnostic().message;
            if (isCancelled()) {
                reason = "the native dispatch was cancelled before the fence retired";
                return NativeStatus::Cancelled;
            }
            return NativeStatus::Failed;
        }
        if (poll == Poll::Ready) {
            return NativeStatus::Ok;
        }
        reason = "the native poll returned an unexpected state";
        return NativeStatus::Failed;
    }
}

[[nodiscard]] inline std::optional<render::ImageWindow> makeWindow(const std::int64_t x,
                                                                   const std::int64_t y,
                                                                   const std::uint64_t width,
                                                                   const std::uint64_t height) {
    const auto result = render::ImageWindow::create(x, y, width, height);
    return result ? std::optional(*result.value()) : std::nullopt;
}

[[nodiscard]] inline std::optional<core::PixelAspectRatio>
makePixelAspect(const std::uint32_t numerator, const std::uint32_t denominator) {
    return core::PixelAspectRatio::create(numerator, denominator);
}

[[nodiscard]] inline bool exactEqual(const std::span<const render::Rgba32f> left,
                                     const std::span<const render::Rgba32f> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool closeWithin(const float left, const float right) noexcept {
    if (!std::isfinite(left) || !std::isfinite(right)) {
        return false;
    }
    const double a = static_cast<double>(left);
    const double b = static_cast<double>(right);
    const double diff = std::abs(a - b);
    const double relative =
        kGpuResidentPreviewCompositeAbsoluteOrRelative * std::max(std::abs(a), std::abs(b));
    return diff <= kGpuResidentPreviewCompositeAbsoluteOrRelative || diff <= relative;
}

[[nodiscard]] inline bool closeEqual(const std::span<const render::Rgba32f> measured,
                                     const std::span<const render::Rgba32f> expected,
                                     std::string& reason) {
    if (measured.size() != expected.size()) {
        reason = "measured and expected pixel counts differ";
        return false;
    }
    for (std::size_t index = 0; index < measured.size(); ++index) {
        if (!closeWithin(measured[index].red(), expected[index].red()) ||
            !closeWithin(measured[index].green(), expected[index].green()) ||
            !closeWithin(measured[index].blue(), expected[index].blue()) ||
            !closeWithin(measured[index].alpha(), expected[index].alpha())) {
            reason = "pixel " + std::to_string(index) + " is outside the 2e-6 abs-or-rel gate";
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool displayMatches(const std::span<const render::Rgba8> measured,
                                         const std::span<const render::Rgba8> expected,
                                         std::string& reason) {
    if (measured.size() != expected.size()) {
        reason = "resident display and CPU oracle pixel counts differ";
        return false;
    }
    for (std::size_t index = 0; index < measured.size(); ++index) {
        const int dr =
            std::abs(static_cast<int>(measured[index].red) - static_cast<int>(expected[index].red));
        const int dg = std::abs(static_cast<int>(measured[index].green) -
                                static_cast<int>(expected[index].green));
        const int db = std::abs(static_cast<int>(measured[index].blue) -
                                static_cast<int>(expected[index].blue));
        if (dr > static_cast<int>(kGpuResidentPreviewDisplayRgbToleranceCodes) ||
            dg > static_cast<int>(kGpuResidentPreviewDisplayRgbToleranceCodes) ||
            db > static_cast<int>(kGpuResidentPreviewDisplayRgbToleranceCodes) ||
            measured[index].alpha != expected[index].alpha) {
            reason = "resident display pixel " + std::to_string(index) +
                     " exceeds the rgb<=1-code alpha-exact contract";
            return false;
        }
    }
    return true;
}

inline void hashPixels(ProbeContext& context, const std::uint32_t width, const std::uint32_t height,
                       const std::span<const render::Rgba32f> pixels) {
    if (context.fixtureHasher == nullptr || pixels.empty()) {
        return;
    }
    const std::array<std::uint32_t, 2> geometry{width, height};
    static_cast<void>(context.fixtureHasher->update(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(geometry.data()), sizeof(geometry))));
    static_cast<void>(context.fixtureHasher->update(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(pixels.data()),
                                   pixels.size() * sizeof(render::Rgba32f))));
}

[[nodiscard]] inline std::optional<render::Rgba32fImage>
buildImage(const render::ImageWindow dataWindow, const render::ImageWindow displayWindow,
           const core::PixelAspectRatio pixelAspect,
           const std::span<const render::Rgba32f> pixels) {
    const auto descriptor =
        render::Rgba32fImageDescriptor::create(dataWindow, displayWindow, pixelAspect);
    if (!descriptor) {
        return std::nullopt;
    }
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1ULL << 34ULL);
    if (!builder) {
        return std::nullopt;
    }
    const std::uint32_t width = dataWindow.extent().width();
    const std::uint32_t height = dataWindow.extent().height();
    if (pixels.size() != static_cast<std::size_t>(width) * height) {
        return std::nullopt;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        auto row = builder.value()->row(dataWindow.originY() + static_cast<std::int64_t>(y));
        if (!row) {
            return std::nullopt;
        }
        const auto source = pixels.subspan(static_cast<std::size_t>(y) * width, width);
        std::copy(source.begin(), source.end(), row.value()->begin());
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

inline void cpuCoverageReference(const render::Rgba32f pixel, const float opacity,
                                 const std::span<const std::uint8_t> coverage,
                                 const std::uint32_t width, const std::uint32_t height,
                                 const std::span<render::Rgba32f> output) noexcept {
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto offset = static_cast<std::size_t>(y) * width;
        if (render::coverageSolidRow(coverage.subspan(offset, width), pixel,
                                     output.subspan(offset, width))) {
            return;
        }
        for (auto& value : output.subspan(offset, width)) {
            const auto faded =
                render::Rgba32f::fromPremultiplied(value.red() * opacity, value.green() * opacity,
                                                   value.blue() * opacity, value.alpha() * opacity);
            value = *faded.value();
        }
    }
}

// --- Operation drivers ------------------------------------------------------------------------

[[nodiscard]] inline ProbeResult runSolidReadback(ProbeContext& context,
                                                  const render::GpuSolidParameters& params,
                                                  std::vector<render::Rgba32f>& outPixels,
                                                  std::shared_ptr<const render::GpuImage>& outImage,
                                                  std::string& reason) {
    using render::GpuSolidDiagnosticCode;
    const auto begin = context.solid.begin(params, context.imageBudget);
    if (begin.code != GpuSolidDiagnosticCode::None) {
        if (begin.code == GpuSolidDiagnosticCode::OverBudget) {
            return fail(GpuResidentPreviewDiagnosticCode::OverBudget, begin.message);
        }
        if (begin.code == GpuSolidDiagnosticCode::WrongThread) {
            return fail(GpuResidentPreviewDiagnosticCode::WrongThread, begin.message);
        }
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, begin.message);
    }
    const auto status =
        waitFor(context.solid, context.isCancelled, context.deadlineNanoseconds, reason);
    if (status != NativeStatus::Ok) {
        return waitFailure(status, reason);
    }
    const auto readback = context.solid.readback();
    if (!readback) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, readback.message);
    }
    outPixels = readback.pixels;
    outImage = std::make_shared<const render::GpuImage>(context.solid.takeImage());
    if (outImage == nullptr || !outImage->isValid()) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure,
                    "the solid pipeline published no resident image");
    }
    return ok();
}

[[nodiscard]] inline ProbeResult
uploadToResident(ProbeContext& context, std::shared_ptr<const render::Rgba32fImage> source,
                 std::shared_ptr<const render::GpuImage>& outImage, std::string& reason) {
    using render::GpuImageUploadDiagnosticCode;
    const auto begin = context.upload.begin(render::GpuImageUploadParameters{std::move(source)},
                                            context.imageBudget);
    if (begin.code != GpuImageUploadDiagnosticCode::None) {
        if (begin.code == GpuImageUploadDiagnosticCode::OverBudget) {
            return fail(GpuResidentPreviewDiagnosticCode::OverBudget, begin.message);
        }
        if (begin.code == GpuImageUploadDiagnosticCode::WrongThread) {
            return fail(GpuResidentPreviewDiagnosticCode::WrongThread, begin.message);
        }
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, begin.message);
    }
    const auto status =
        waitFor(context.upload, context.isCancelled, context.deadlineNanoseconds, reason);
    if (status != NativeStatus::Ok) {
        return waitFailure(status, reason);
    }
    outImage = std::make_shared<const render::GpuImage>(context.upload.takeImage());
    if (outImage == nullptr || !outImage->isValid()) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure,
                    "the upload published no resident image");
    }
    return ok();
}

[[nodiscard]] inline ProbeResult
runTranslationReadback(ProbeContext& context, const render::GpuTranslationParameters& params,
                       std::vector<render::Rgba32f>& outPixels,
                       std::shared_ptr<const render::GpuImage>& outImage, std::string& reason) {
    using render::GpuCompositeDiagnosticCode;
    const auto begin = context.composite.beginTranslation(params, context.imageBudget);
    if (begin.code != GpuCompositeDiagnosticCode::None) {
        if (begin.code == GpuCompositeDiagnosticCode::OverBudget) {
            return fail(GpuResidentPreviewDiagnosticCode::OverBudget, begin.message);
        }
        if (begin.code == GpuCompositeDiagnosticCode::WrongThread) {
            return fail(GpuResidentPreviewDiagnosticCode::WrongThread, begin.message);
        }
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, begin.message);
    }
    const auto status =
        waitFor(context.composite, context.isCancelled, context.deadlineNanoseconds, reason);
    if (status != NativeStatus::Ok) {
        return waitFailure(status, reason);
    }
    outImage = std::make_shared<const render::GpuImage>(context.composite.takeImage());
    const auto readback = render::readbackResidentImage(*outImage, context.imageBudget);
    if (!readback) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, readback.message);
    }
    outPixels = readback.pixels;
    return ok();
}

[[nodiscard]] inline ProbeResult
runSourceOverReadback(ProbeContext& context, const render::GpuSourceOverParameters& params,
                      std::vector<render::Rgba32f>& outPixels,
                      std::shared_ptr<const render::GpuImage>& outImage, std::string& reason) {
    using render::GpuCompositeDiagnosticCode;
    const auto begin = context.composite.beginSourceOver(params, context.imageBudget);
    if (begin.code != GpuCompositeDiagnosticCode::None) {
        if (begin.code == GpuCompositeDiagnosticCode::OverBudget) {
            return fail(GpuResidentPreviewDiagnosticCode::OverBudget, begin.message);
        }
        if (begin.code == GpuCompositeDiagnosticCode::WrongThread) {
            return fail(GpuResidentPreviewDiagnosticCode::WrongThread, begin.message);
        }
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, begin.message);
    }
    const auto status =
        waitFor(context.composite, context.isCancelled, context.deadlineNanoseconds, reason);
    if (status != NativeStatus::Ok) {
        return waitFailure(status, reason);
    }
    outImage = std::make_shared<const render::GpuImage>(context.composite.takeImage());
    const auto readback = render::readbackResidentImage(*outImage, context.imageBudget);
    if (!readback) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, readback.message);
    }
    outPixels = readback.pixels;
    return ok();
}

[[nodiscard]] inline ProbeResult
runDisplayReadback(ProbeContext& context, const std::shared_ptr<const render::GpuImage>& input,
                   std::vector<render::Rgba8>& outPixels, std::string& reason) {
    using render::GpuDisplayImageReadbackCode;
    using render::GpuResidentDisplayDiagnosticCode;
    const auto begin = context.display.begin(input, context.imageBudget);
    if (begin.code != GpuResidentDisplayDiagnosticCode::None) {
        if (begin.code == GpuResidentDisplayDiagnosticCode::OverBudget) {
            return fail(GpuResidentPreviewDiagnosticCode::OverBudget, begin.message);
        }
        if (begin.code == GpuResidentDisplayDiagnosticCode::WrongThread) {
            return fail(GpuResidentPreviewDiagnosticCode::WrongThread, begin.message);
        }
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, begin.message);
    }
    const auto status =
        waitFor(context.display, context.isCancelled, context.deadlineNanoseconds, reason);
    if (status != NativeStatus::Ok) {
        return waitFailure(status, reason);
    }
    const auto readback = context.display.readback();
    if (readback.code != GpuDisplayImageReadbackCode::None) {
        return fail(GpuResidentPreviewDiagnosticCode::NativeFailure, readback.message);
    }
    outPixels = readback.pixels;
    return ok();
}

// --- Verifiers ---------------------------------------------------------------------------------

[[nodiscard]] ProbeResult verifyShaderPins(const core::Sha256Hasher* = nullptr) noexcept;
[[nodiscard]] ProbeResult verifySolidParity(ProbeContext& context) noexcept;
[[nodiscard]] ProbeResult verifyCoveredParity(ProbeContext& context) noexcept;
[[nodiscard]] ProbeResult verifyUploadAndCompositeParity(ProbeContext& context) noexcept;
[[nodiscard]] ProbeResult verifyResidentDisplayParity(ProbeContext& context) noexcept;
[[nodiscard]] ProbeResult verifySubnormalRejection(ProbeContext& context,
                                                   bool& subnormalRejected) noexcept;
[[nodiscard]] ProbeResult
measureEligibility(ProbeContext& context,
                   std::vector<GpuResidentPreviewTimingSample>& outTimings) noexcept;

} // namespace bloom::runtime::resident_preview_detail
