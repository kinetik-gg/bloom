// Bounded runtime qualification for the already-embedded fixed Bloom Neutral v1 display compute
// operation. Runs on the native device/pipeline owner thread only. See the public header for the
// contract; this translation unit contains no service, scheduler, or frame-product work.

#include <bloom/runtime/gpu_neutral_display_qualification.hpp>

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

using color::CancellationPredicateRef;
using color::PreparedCpuDisplayProcessorHandle;
using core::Sha256Digest;
using render::GpuDevice;
using render::GpuDeviceState;
using render::GpuNeutralDisplay;
using render::GpuNeutralDisplayDiagnosticCode;
using render::GpuNeutralDisplayPollResult;
using render::Rgba32f;
using render::Rgba32fImage;
using render::Rgba32fImageBuilder;
using render::Rgba32fImageDescriptor;
using render::Rgba32fImageView;
using render::Rgba8;

constexpr std::uint64_t kPerDispatchDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t kGpuByteBudget = 1ULL << 30ULL;
constexpr std::size_t kMeasuredPairCount = 3;
// Ascending: a tiny viewer size, qHD, 720p, 1080p, and 4K. The tiny and qHD sizes are what a
// default Auto-fit viewer actually resolves to, so the profile must measure them rather than assume
// a 720p floor.
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 5> kMeasuredSizes{{
    {256, 144},
    {640, 360},
    {1280, 720},
    {1920, 1080},
    {3840, 2160},
}};

[[nodiscard]] GpuNeutralDisplayQualificationDiagnostic
makeDiagnostic(const GpuNeutralDisplayQualificationDiagnosticCode code, std::string message) {
    return GpuNeutralDisplayQualificationDiagnostic{code, std::move(message)};
}

[[nodiscard]] std::optional<Rgba32fImage> makeImage(const std::span<const Rgba32f> pixels,
                                                    const std::uint32_t width,
                                                    const std::uint32_t height) {
    if (pixels.size() != static_cast<std::size_t>(width) * height) {
        return std::nullopt;
    }
    const auto windowResult = render::ImageWindow::create(0, 0, width, height);
    if (!windowResult) {
        return std::nullopt;
    }
    const auto descriptorResult = Rgba32fImageDescriptor::create(
        *windowResult.value(), *windowResult.value(), core::PixelAspectRatio::square());
    if (!descriptorResult) {
        return std::nullopt;
    }
    auto builderResult = Rgba32fImageBuilder::create(*descriptorResult.value(), 1ULL << 33ULL);
    if (!builderResult) {
        return std::nullopt;
    }
    auto builder = std::move(*builderResult.value());
    // Bulk row copy through the builder's checked row API rather than per-pixel writes.
    for (std::uint32_t y = 0; y < height; ++y) {
        auto row = builder.row(static_cast<std::int64_t>(y));
        if (!row) {
            return std::nullopt;
        }
        const auto sourceRow = pixels.subspan(static_cast<std::size_t>(y) * width, width);
        std::copy(sourceRow.begin(), sourceRow.end(), row.value()->begin());
    }
    auto frozen = std::move(builder).freeze();
    if (!frozen) {
        return std::nullopt;
    }
    return std::move(*frozen.value());
}

enum class NativeOutcome { Ok, Failed, Cancelled, Timeout };

// Drives begin/poll/readback under a bounded wall-clock deadline, checking the cancellation
// predicate before begin and between polls. On Timeout or Cancelled it marks native cancel and
// returns immediately; the queue submission and its buffers remain owned by the pipeline, which the
// caller must destroy or drain on the owner thread before reusing. No non-Ok outcome continues
// dispatching.
[[nodiscard]] NativeOutcome
runNative(GpuNeutralDisplay& pipeline, const std::span<const Rgba32f> source,
          const CancellationPredicateRef isCancelled, std::vector<Rgba8>& outPixels,
          GpuNeutralDisplayDiagnosticCode& outCode, std::string& reason) {
    if (isCancelled()) {
        reason = "the native dispatch was cancelled before begin";
        outCode = GpuNeutralDisplayDiagnosticCode::Cancelled;
        return NativeOutcome::Cancelled;
    }
    const auto begin = pipeline.begin(source, kGpuByteBudget);
    if (begin.code != GpuNeutralDisplayDiagnosticCode::None) {
        outCode = begin.code;
        reason = "native begin failed: " + begin.message;
        return begin.code == GpuNeutralDisplayDiagnosticCode::Cancelled ? NativeOutcome::Cancelled
                                                                        : NativeOutcome::Failed;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(kPerDispatchDeadlineNanoseconds);
    for (;;) {
        if (isCancelled()) {
            // Mark discard and return immediately. The submission is still owned by the pipeline;
            // destroying/draining it is the caller's owner-thread responsibility.
            pipeline.cancel();
            outCode = GpuNeutralDisplayDiagnosticCode::Cancelled;
            reason = "the native dispatch was cancelled";
            return NativeOutcome::Cancelled;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            pipeline.cancel();
            outCode = GpuNeutralDisplayDiagnosticCode::DeviceUnavailable;
            reason = "the native dispatch exceeded the bounded wall-clock deadline";
            return NativeOutcome::Timeout;
        }
        const auto poll = pipeline.poll();
        if (poll == GpuNeutralDisplayPollResult::Pending) {
            std::this_thread::yield();
            continue;
        }
        if (poll == GpuNeutralDisplayPollResult::Failure) {
            outCode = pipeline.diagnostic().code;
            reason = "native poll failed: " + pipeline.diagnostic().message;
            return outCode == GpuNeutralDisplayDiagnosticCode::Cancelled ? NativeOutcome::Cancelled
                                                                         : NativeOutcome::Failed;
        }
        auto readback = pipeline.readback();
        if (!readback) {
            outCode = readback.diagnostic.code;
            reason = "native readback failed: " + readback.diagnostic.message;
            return NativeOutcome::Failed;
        }
        // Check cancellation after readback and before publishing Ok: a cancellation requested
        // while the frame completed must not be reported as a successful sample.
        if (isCancelled()) {
            pipeline.cancel();
            outCode = GpuNeutralDisplayDiagnosticCode::Cancelled;
            reason = "the native dispatch was cancelled before publication";
            return NativeOutcome::Cancelled;
        }
        outPixels = std::move(readback.pixels);
        return NativeOutcome::Ok;
    }
}

// RGB within one code, alpha exact -- the frozen shader manifest's numeric contract.
[[nodiscard]] bool parityHolds(const std::span<const Rgba8> native,
                               const std::span<const Rgba8> cpu, std::string& reason) {
    if (native.size() != cpu.size()) {
        reason = "native and CPU pixel counts differ";
        return false;
    }
    for (std::size_t index = 0; index < native.size(); ++index) {
        const int dr =
            std::abs(static_cast<int>(native[index].red) - static_cast<int>(cpu[index].red));
        const int dg =
            std::abs(static_cast<int>(native[index].green) - static_cast<int>(cpu[index].green));
        const int db =
            std::abs(static_cast<int>(native[index].blue) - static_cast<int>(cpu[index].blue));
        if (dr > static_cast<int>(kGpuNeutralDisplayRgbToleranceCodes) ||
            dg > static_cast<int>(kGpuNeutralDisplayRgbToleranceCodes) ||
            db > static_cast<int>(kGpuNeutralDisplayRgbToleranceCodes) ||
            native[index].alpha != cpu[index].alpha) {
            reason = "pixel " + std::to_string(index) + " exceeds the numeric contract";
            return false;
        }
    }
    return true;
}

// Timing outcome of one pair, so the caller preserves Cancel/Timeout as their own report codes.
enum class PairOutcome { Ok, NativeFailed, Cancelled, Timeout, CpuFailed, ParityFailed };

// One measured pair: `nativeMs` is the full native begin->readback interval and `cpuMs` is exactly
// the produceBloomNeutralDisplayFrame call (image/view prepared before the timer, no extra oracle
// copy). `nativeFirst` alternates the order so neither path always runs on a warmed cache.
[[nodiscard]] PairOutcome
measurePair(GpuNeutralDisplay& pipeline, const PreparedCpuDisplayProcessorHandle& processor,
            const Rgba32fImageView view, const std::span<const Rgba32f> source,
            const CancellationPredicateRef isCancelled, const bool nativeFirst, double& nativeMs,
            double& cpuMs, std::string& reason) {
    std::vector<Rgba8> nativePixels;
    std::optional<render::ImageResult<color::PreparedDisplayFrame>> cpuResult;

    const auto runNativeTimed = [&]() -> PairOutcome {
        GpuNeutralDisplayDiagnosticCode code = GpuNeutralDisplayDiagnosticCode::None;
        const auto start = std::chrono::steady_clock::now();
        const auto outcome = runNative(pipeline, source, isCancelled, nativePixels, code, reason);
        const auto end = std::chrono::steady_clock::now();
        switch (outcome) {
        case NativeOutcome::Ok:
            nativeMs = std::chrono::duration<double, std::milli>(end - start).count();
            return PairOutcome::Ok;
        case NativeOutcome::Cancelled:
            return PairOutcome::Cancelled;
        case NativeOutcome::Timeout:
            return PairOutcome::Timeout;
        case NativeOutcome::Failed:
            return PairOutcome::NativeFailed;
        }
        return PairOutcome::NativeFailed;
    };
    const auto runCpuTimed = [&]() -> PairOutcome {
        const auto start = std::chrono::steady_clock::now();
        auto result = color::produceBloomNeutralDisplayFrame(
            processor, view, kGpuNeutralDisplayCpuChunkPixelCount,
            std::numeric_limits<std::size_t>::max(), isCancelled);
        const auto end = std::chrono::steady_clock::now();
        if (!result) {
            // Distinguish a cancellation from a genuine oracle failure.
            if (isCancelled()) {
                reason = "the CPU oracle was cancelled";
                return PairOutcome::Cancelled;
            }
            reason = "the CPU oracle rejected the fixture";
            return PairOutcome::CpuFailed;
        }
        cpuMs = std::chrono::duration<double, std::milli>(end - start).count();
        cpuResult.emplace(std::move(result));
        return PairOutcome::Ok;
    };

    // Run the first side, and short-circuit on its failure before running the second. The order
    // alternates by `nativeFirst`.
    if (nativeFirst) {
        const auto nativeCode = runNativeTimed();
        if (nativeCode != PairOutcome::Ok) {
            return nativeCode;
        }
        const auto cpuCode = runCpuTimed();
        if (cpuCode != PairOutcome::Ok) {
            return cpuCode;
        }
    } else {
        const auto cpuCode = runCpuTimed();
        if (cpuCode != PairOutcome::Ok) {
            return cpuCode;
        }
        const auto nativeCode = runNativeTimed();
        if (nativeCode != PairOutcome::Ok) {
            return nativeCode;
        }
    }
    if (!cpuResult.has_value()) {
        reason = "the CPU oracle produced no frame";
        return PairOutcome::CpuFailed;
    }
    return parityHolds(nativePixels, cpuResult->value()->pixels(), reason)
               ? PairOutcome::Ok
               : PairOutcome::ParityFailed;
}

[[nodiscard]] double median3(std::array<double, kMeasuredPairCount> values) {
    std::sort(values.begin(), values.end());
    return values[1];
}

[[nodiscard]] GpuNeutralDisplayQualificationDiagnostic
pairOutcomeDiagnostic(const PairOutcome outcome, std::string reason) {
    switch (outcome) {
    case PairOutcome::Cancelled:
        return makeDiagnostic(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled,
                              std::move(reason));
    case PairOutcome::Timeout:
        return makeDiagnostic(GpuNeutralDisplayQualificationDiagnosticCode::NativeTimeout,
                              std::move(reason));
    case PairOutcome::CpuFailed:
        return makeDiagnostic(GpuNeutralDisplayQualificationDiagnosticCode::CpuOracleFailure,
                              std::move(reason));
    case PairOutcome::ParityFailed:
        return makeDiagnostic(GpuNeutralDisplayQualificationDiagnosticCode::ParityFailure,
                              std::move(reason));
    case PairOutcome::NativeFailed:
        return makeDiagnostic(GpuNeutralDisplayQualificationDiagnosticCode::NativeFailure,
                              std::move(reason));
    case PairOutcome::Ok:
        break;
    }
    return makeDiagnostic(GpuNeutralDisplayQualificationDiagnosticCode::InternalInvariant,
                          std::move(reason));
}

// The interval starts at the lowest measured size of the contiguous suffix ending at the largest
// measured size in which every size improved. An empty suffix (the largest size is not faster)
// means CPU-only.
[[nodiscard]] std::optional<GpuNeutralDisplayEligibleInterval>
deriveEligibleInterval(const std::vector<GpuNeutralDisplayTimingSample>& timings) {
    if (timings.empty() || !timings.back().native_improved) {
        return std::nullopt;
    }
    std::size_t lowest = timings.size() - 1;
    while (lowest > 0 && timings[lowest - 1].native_improved) {
        --lowest;
    }
    if (!timings[lowest].native_improved) {
        return std::nullopt;
    }
    return GpuNeutralDisplayEligibleInterval{timings[lowest].pixel_count,
                                             timings.back().pixel_count};
}

} // namespace

bool gpuNeutralDisplayIdentityIsEligible(const color::DisplayProcessorIdentityV1& identity,
                                         std::string& reason) noexcept {
    // reason.assign may allocate; a failure here must not escape a noexcept helper as terminate.
    const auto reject = [&reason](const std::string_view message) noexcept {
        try {
            reason.assign(message);
        } catch (...) {
            reason.clear();
        }
        return false;
    };

    const auto view = identity.borrowedView();
    if (!view.has_value()) {
        return reject("the processor identity has no borrowed view");
    }

    // The exact expected record for the fixed Bloom Neutral v1 display operation. This reuses the
    // official canonical writer rather than a second parser, so a canonically valid record with a
    // different source/context/look/packing is rejected.
    const color::DisplayProcessorIdentityV1InputView expected{
        .expectedOcioRevision = color::kBloomNeutralV1ConfigDigest,
        .contextVariables = {},
        .sourceColorSpaceId = color::kDisplayProcessorIdentitySourceColorSpaceId,
        .displayName = kGpuNeutralDisplayDisplayName,
        .viewName = kGpuNeutralDisplayViewName,
        .lookMode = color::DisplayProcessorLookModeV1::Bypass,
        .lookNames = {},
        .outputColorSpaceId = color::kDisplayProcessorIdentityOutputColorSpaceId,
        .qualityId = color::kDisplayProcessorIdentityQualityId,
        .semanticsProfileId = color::kDisplayProcessorIdentitySemanticsProfileId,
        .packingId = color::kDisplayProcessorIdentityPackingId,
    };
    const auto validation = color::validateDisplayProcessorIdentityV1(expected);
    if (!validation) {
        return reject("the expected Bloom Neutral v1 identity record is not valid");
    }
    // 1024 bytes is ample for this fixed record; the writer still checks capacity.
    std::array<std::byte, 1024> expectedBytes{};
    if (validation.requiredByteCount() > expectedBytes.size()) {
        return reject("the expected Bloom Neutral v1 identity record is unexpectedly large");
    }
    const auto written = color::writeDisplayProcessorIdentityV1(expected, expectedBytes);
    if (!written) {
        return reject("the expected Bloom Neutral v1 identity record could not be written");
    }
    const auto actual = identity.canonicalBytes();
    const auto expectedCanonical =
        std::span<const std::byte>(expectedBytes.data(), written.writtenByteCount());
    if (actual.size() != expectedCanonical.size() ||
        !std::equal(actual.begin(), actual.end(), expectedCanonical.begin())) {
        return reject("the processor identity is not the exact Bloom Neutral v1 display record");
    }
    return true;
}

bool gpuNeutralDisplayProcessorIsEligible(const PreparedCpuDisplayProcessorHandle& processor,
                                          std::string& reason) noexcept {
    const auto reject = [&reason](const std::string_view message) noexcept {
        try {
            reason.assign(message);
        } catch (...) {
            reason.clear();
        }
        return false;
    };

    // Exact canonical identity check first (revision, source, default display/view, empty
    // context/looks, packing), then the processor provenance facts.
    if (!gpuNeutralDisplayIdentityIsEligible(processor.identity(), reason)) {
        return false;
    }
    const auto& provenance = processor.provenance();
    if (provenance.processorCacheId != kGpuNeutralDisplayProcessorCacheId) {
        return reject("the processor cache ID is not the pinned Bloom Neutral v1 cache ID");
    }
    if (provenance.ocioVersion != kGpuNeutralDisplayOcioVersion) {
        return reject("the processor OCIO version is not the pinned version");
    }
    if (provenance.displayName != kGpuNeutralDisplayDisplayName ||
        provenance.viewName != kGpuNeutralDisplayViewName) {
        return reject("the processor display/view is not the pinned srgb_rec709_display pair");
    }
    return true;
}

GpuNeutralDisplayQualificationReport::GpuNeutralDisplayQualificationReport(
    GpuNeutralDisplayQualificationReport&&) noexcept = default;

GpuNeutralDisplayQualificationReport& GpuNeutralDisplayQualificationReport::operator=(
    GpuNeutralDisplayQualificationReport&&) noexcept = default;

GpuNeutralDisplayQualificationReport
qualifyGpuNeutralDisplay(const PreparedCpuDisplayProcessorHandle& processor, GpuDevice& device,
                         GpuNeutralDisplay& pipeline,
                         const CancellationPredicateRef isCancelled) noexcept {
    // A non-allocating failure report: outcome defaults to Unavailable and the diagnostic code is
    // set without building a message, so no allocation can escape as a terminate.
    const auto fail = [](const GpuNeutralDisplayQualificationDiagnosticCode code) {
        GpuNeutralDisplayQualificationReport failed;
        failed.diagnostic_.code = code;
        return failed;
    };
    try {
        if (device.state() != GpuDeviceState::Ready) {
            return fail(GpuNeutralDisplayQualificationDiagnosticCode::NotEligibleDevice);
        }
        const auto& capability = device.capabilityReport();
        GpuNeutralDisplayQualificationReport report;
        report.deviceGeneration_ = capability.generation;
        report.deviceIdentity_ = capability.identity;

        // The pipeline must be bound to exactly this device; otherwise this device's report would
        // be labelled with another device's results.
        if (!pipeline.isBoundTo(device)) {
            return fail(GpuNeutralDisplayQualificationDiagnosticCode::NotEligibleDevice);
        }

        std::string reason;
        if (!gpuNeutralDisplayProcessorIsEligible(processor, reason)) {
            return fail(GpuNeutralDisplayQualificationDiagnosticCode::NotEligibleProcessor);
        }
        report.configRevision_ = color::kBloomNeutralV1ConfigDigest;
        report.processorCacheId_ = processor.provenance().processorCacheId;
        report.shaderDigest_ = kGpuNeutralDisplayShaderDigest;
        report.numericContract_ = std::string(kGpuNeutralDisplayNumericContract);

        if (isCancelled()) {
            return fail(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled);
        }

        const auto fixtures = detail::makeGpuNeutralDisplayParityFixtures();
        if (!fixtures.has_value()) {
            return fail(GpuNeutralDisplayQualificationDiagnosticCode::InternalInvariant);
        }
        report.fixtureDigest_ = detail::gpuNeutralDisplayFixtureDigest(*fixtures);

        // Parity pass over every deterministic fixture.
        for (const detail::GpuNeutralDisplayFixture& fixture : *fixtures) {
            if (isCancelled()) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled);
            }
            const auto image = makeImage(fixture.pixels, fixture.width, fixture.height);
            if (!image.has_value()) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::CpuOracleFailure);
            }
            std::vector<Rgba8> nativePixels;
            GpuNeutralDisplayDiagnosticCode code = GpuNeutralDisplayDiagnosticCode::None;
            const auto outcome =
                runNative(pipeline, fixture.pixels, isCancelled, nativePixels, code, reason);
            if (outcome == NativeOutcome::Cancelled) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled);
            }
            if (outcome == NativeOutcome::Timeout) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::NativeTimeout);
            }
            if (outcome != NativeOutcome::Ok) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::NativeFailure);
            }
            const auto imageView = image->view();
            if (!imageView) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::CpuOracleFailure);
            }
            auto cpuResult = color::produceBloomNeutralDisplayFrame(
                processor, *imageView.value(), kGpuNeutralDisplayCpuChunkPixelCount,
                std::numeric_limits<std::size_t>::max(), isCancelled);
            if (!cpuResult) {
                return fail(isCancelled()
                                ? GpuNeutralDisplayQualificationDiagnosticCode::Cancelled
                                : GpuNeutralDisplayQualificationDiagnosticCode::CpuOracleFailure);
            }
            if (!parityHolds(nativePixels, cpuResult.value()->pixels(), reason)) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::ParityFailure);
            }
        }

        // Nonzero subnormal input is intentionally rejected whole-frame by the shader (error flag
        // 8) and remains a per-frame CPU fallback. Only an observed ShaderRejected proves that;
        // device loss, cancellation, or timeout abort qualification with their own code and are
        // never counted as subnormal proof.
        {
            const auto subnormalPixel = Rgba32f::fromPremultiplied(
                std::numeric_limits<float>::denorm_min(), 0.0F, 0.0F, 1.0F);
            if (subnormalPixel) {
                const std::vector<Rgba32f> subnormal{*subnormalPixel.value()};
                std::vector<Rgba8> ignored;
                GpuNeutralDisplayDiagnosticCode code = GpuNeutralDisplayDiagnosticCode::None;
                const auto outcome =
                    runNative(pipeline, subnormal, isCancelled, ignored, code, reason);
                if (outcome == NativeOutcome::Cancelled) {
                    return fail(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled);
                }
                if (outcome == NativeOutcome::Timeout) {
                    return fail(GpuNeutralDisplayQualificationDiagnosticCode::NativeTimeout);
                }
                if (outcome == NativeOutcome::Ok) {
                    report.subnormalFrameRejected_ = false;
                } else if (code == GpuNeutralDisplayDiagnosticCode::ShaderRejected) {
                    report.subnormalFrameRejected_ = true;
                } else {
                    // Device loss or any other native failure is abortive, not subnormal proof.
                    return fail(GpuNeutralDisplayQualificationDiagnosticCode::NativeFailure);
                }
            }
        }

        // Timing pass: one cancellable warmup, then three alternating pairs per measured size. The
        // source image and its view are preconstructed outside every timed region.
        for (const auto& [width, height] : kMeasuredSizes) {
            if (isCancelled()) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled);
            }
            const std::size_t count = static_cast<std::size_t>(width) * height;
            const auto source = detail::makeGpuNeutralDisplayMeasuredPixels(width, height);
            if (!source.has_value()) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::InternalInvariant);
            }
            const auto image = makeImage(*source, width, height);
            if (!image.has_value()) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::CpuOracleFailure);
            }
            const auto imageView = image->view();
            if (!imageView) {
                return fail(GpuNeutralDisplayQualificationDiagnosticCode::CpuOracleFailure);
            }
            const Rgba32fImageView view = *imageView.value();

            double warmNative = 0.0;
            double warmCpu = 0.0;
            const auto warm = measurePair(pipeline, processor, view, *source, isCancelled, true,
                                          warmNative, warmCpu, reason);
            if (warm != PairOutcome::Ok) {
                return fail(pairOutcomeDiagnostic(warm, std::move(reason)).code);
            }

            std::array<double, kMeasuredPairCount> nativeMs{};
            std::array<double, kMeasuredPairCount> cpuMs{};
            for (std::size_t pair = 0; pair < kMeasuredPairCount; ++pair) {
                if (isCancelled()) {
                    return fail(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled);
                }
                double nativeValue = 0.0;
                double cpuValue = 0.0;
                const bool nativeFirst = (pair % 2) == 0;
                const auto outcome = measurePair(pipeline, processor, view, *source, isCancelled,
                                                 nativeFirst, nativeValue, cpuValue, reason);
                if (outcome != PairOutcome::Ok) {
                    return fail(pairOutcomeDiagnostic(outcome, std::move(reason)).code);
                }
                nativeMs[pair] = nativeValue;
                cpuMs[pair] = cpuValue;
            }
            GpuNeutralDisplayTimingSample sample;
            sample.width = width;
            sample.height = height;
            sample.pixel_count = count;
            sample.native_full_ms = median3(nativeMs);
            sample.cpu_full_ms = median3(cpuMs);
            sample.native_improved = sample.native_full_ms < sample.cpu_full_ms;
            report.timings_.push_back(sample);
        }

        // Before publishing a successful report, confirm no cancellation arrived during the last
        // measured pair. A cancelled run never yields a partial eligible report.
        if (isCancelled()) {
            return fail(GpuNeutralDisplayQualificationDiagnosticCode::Cancelled);
        }

        // Parity held, so the honest ceiling is PreviewOnly. The interval is derived from the
        // contiguous faster suffix at the largest measured size; otherwise CPU-only.
        report.outcome_ = GpuNeutralDisplayQualificationOutcome::PreviewOnly;
        report.eligibleInterval_ = deriveEligibleInterval(report.timings_);
        report.diagnostic_ =
            report.eligibleInterval_.has_value()
                ? makeDiagnostic(GpuNeutralDisplayQualificationDiagnosticCode::None,
                                 "parity held and a contiguous faster suffix was measured")
                : makeDiagnostic(
                      GpuNeutralDisplayQualificationDiagnosticCode::TimingNotImproved,
                      "parity held but the largest measured size was not faster; CPU-only");
        return report;
    } catch (...) {
        return fail(GpuNeutralDisplayQualificationDiagnosticCode::InternalInvariant);
    }
}

} // namespace bloom::runtime
