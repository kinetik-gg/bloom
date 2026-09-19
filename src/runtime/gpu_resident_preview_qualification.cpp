// Runtime qualification product and orchestration for the resident GPU preview route. Runs on the
// device/pipeline owner thread only. See the public header for the contract; this translation unit
// contains no service, scheduler, frame product, or viewer activation.

#include "gpu_resident_preview_qualification_internal.hpp"

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/runtime/gpu_neutral_display_qualification.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

using color::PreparedCpuDisplayProcessorHandle;
using render::GpuDevice;
using render::GpuDeviceState;

[[nodiscard]] std::optional<core::Sha256Digest>
canonicalIdentityDigest(const color::DisplayProcessorIdentityV1& identity) noexcept {
    return core::Sha256Hasher::hash(identity.canonicalBytes());
}

[[nodiscard]] GpuResidentPreviewDiagnostic
makeDiagnostic(const GpuResidentPreviewDiagnosticCode code, std::string message) {
    return GpuResidentPreviewDiagnostic{code, std::move(message)};
}

// The interval starts at the lowest measured size of the contiguous suffix ending at the largest
// measured size in which every size improved. An empty suffix means CPU-only.
[[nodiscard]] std::optional<GpuResidentPreviewEligibleInterval>
deriveEligibleInterval(const std::vector<GpuResidentPreviewTimingSample>& timings) {
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
    return GpuResidentPreviewEligibleInterval{timings[lowest].pixel_count,
                                              timings.back().pixel_count};
}

} // namespace

bool gpuResidentPreviewIdentityIsEligible(const color::DisplayProcessorIdentityV1& identity,
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

bool gpuResidentPreviewProcessorIsEligible(const PreparedCpuDisplayProcessorHandle& processor,
                                           std::string& reason) noexcept {
    const auto reject = [&reason](const std::string_view message) noexcept {
        try {
            reason.assign(message);
        } catch (...) {
            reason.clear();
        }
        return false;
    };
    if (!gpuResidentPreviewIdentityIsEligible(processor.identity(), reason)) {
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

GpuResidentPreviewQualificationReport::GpuResidentPreviewQualificationReport(
    GpuResidentPreviewQualificationReport&&) noexcept = default;

GpuResidentPreviewQualificationReport& GpuResidentPreviewQualificationReport::operator=(
    GpuResidentPreviewQualificationReport&&) noexcept = default;

bool GpuResidentPreviewQualificationReport::eligibleFor(
    const GpuDevice& device, const PreparedCpuDisplayProcessorHandle& processor) const noexcept {
    if (outcome_ != GpuResidentPreviewOutcome::PreviewOnly || !eligibleInterval_.has_value()) {
        return false;
    }
    if (device.state() != GpuDeviceState::Ready) {
        return false;
    }
    const std::uint64_t epoch = device.ownershipEpoch();
    if (epoch == 0 || epoch != ownershipEpoch_) {
        return false;
    }
    const render::GpuCapabilityReport& capability = device.capabilityReport();
    if (capability.generation != deviceGeneration_ || !(capability.identity == deviceIdentity_)) {
        return false;
    }
    std::string reason;
    if (!gpuResidentPreviewProcessorIsEligible(processor, reason)) {
        return false;
    }
    if (processor.provenance().processorCacheId != processorCacheId_) {
        return false;
    }
    const auto digest = canonicalIdentityDigest(processor.identity());
    if (!digest.has_value() || *digest != processorIdentityDigest_) {
        return false;
    }
    return true;
}

GpuResidentPreviewQualificationReport
qualifyResidentPreview(const PreparedCpuDisplayProcessorHandle& processor, GpuDevice& device,
                       const GpuResidentPreviewPipelines& pipelines,
                       const GpuResidentPreviewBudgets& budgets,
                       color::CancellationPredicateRef isCancelled) noexcept {
    // A non-allocating failure report: outcome stays Unavailable and the diagnostic code is set
    // without building a message, so no allocation can escape as a terminate.
    const auto fail = [](const GpuResidentPreviewDiagnosticCode code) {
        GpuResidentPreviewQualificationReport failed;
        failed.diagnostic_.code = code;
        return failed;
    };
    try {
        if (device.state() != GpuDeviceState::Ready) {
            return fail(GpuResidentPreviewDiagnosticCode::DeviceUnavailable);
        }
        if (!device.isOwnerThread()) {
            return fail(GpuResidentPreviewDiagnosticCode::WrongThread);
        }
        if (pipelines.solid == nullptr || pipelines.upload == nullptr ||
            pipelines.composite == nullptr || pipelines.display == nullptr) {
            return fail(GpuResidentPreviewDiagnosticCode::NotEligibleDevice);
        }
        // Every pipeline must be bound to exactly this actual device BEFORE any call. Otherwise
        // this device's report could be labelled with another device's results.
        if (!pipelines.solid->isBoundTo(device) || !pipelines.upload->isBoundTo(device) ||
            !pipelines.composite->isBoundTo(device) || !pipelines.display->isBoundTo(device)) {
            return fail(GpuResidentPreviewDiagnosticCode::NotEligibleDevice);
        }
        std::string reason;
        if (!gpuResidentPreviewProcessorIsEligible(processor, reason)) {
            GpuResidentPreviewQualificationReport rejected;
            rejected.diagnostic_ = makeDiagnostic(
                GpuResidentPreviewDiagnosticCode::NotEligibleProcessor, std::move(reason));
            return rejected;
        }
        // A refusal budget is a negative gate, not a crash.
        if (budgets.maxImageBytes < 4096ULL || budgets.maxMetadataBytes < 64ULL ||
            budgets.perDispatchDeadlineNanoseconds == 0ULL) {
            return fail(GpuResidentPreviewDiagnosticCode::OverBudget);
        }
        if (isCancelled()) {
            GpuResidentPreviewQualificationReport cancelled;
            cancelled.diagnostic_ = makeDiagnostic(
                GpuResidentPreviewDiagnosticCode::Cancelled,
                "the resident preview qualification was cancelled before any dispatch");
            return cancelled;
        }

        core::Sha256Hasher fixtureHasher;
        resident_preview_detail::ProbeContext context{
            processor,
            *pipelines.solid,
            *pipelines.upload,
            *pipelines.composite,
            *pipelines.display,
            budgets.maxImageBytes,
            budgets.maxMetadataBytes,
            budgets.perDispatchDeadlineNanoseconds,
            isCancelled,
            &fixtureHasher,
        };
        const auto rejectProbe = [](const resident_preview_detail::ProbeResult& probe) {
            GpuResidentPreviewQualificationReport rejected;
            rejected.diagnostic_ = makeDiagnostic(probe.code, probe.message);
            return rejected;
        };
        if (const auto pins = resident_preview_detail::verifyShaderPins();
            pins.code != GpuResidentPreviewDiagnosticCode::None) {
            return rejectProbe(pins);
        }
        for (resident_preview_detail::ProbeResult (*check)(
                 resident_preview_detail::ProbeContext&) noexcept :
             {resident_preview_detail::verifySolidParity,
              resident_preview_detail::verifyCoveredParity,
              resident_preview_detail::verifyUploadAndCompositeParity,
              resident_preview_detail::verifyResidentDisplayParity}) {
            const auto probe = check(context);
            if (probe.code != GpuResidentPreviewDiagnosticCode::None) {
                return rejectProbe(probe);
            }
        }
        bool subnormalRejected = false;
        if (const auto probe =
                resident_preview_detail::verifySubnormalRejection(context, subnormalRejected);
            probe.code != GpuResidentPreviewDiagnosticCode::None) {
            return rejectProbe(probe);
        }
        std::vector<GpuResidentPreviewTimingSample> timings;
        if (const auto probe = resident_preview_detail::measureEligibility(context, timings);
            probe.code != GpuResidentPreviewDiagnosticCode::None) {
            return rejectProbe(probe);
        }
        if (isCancelled()) {
            return fail(GpuResidentPreviewDiagnosticCode::Cancelled);
        }

        const render::GpuCapabilityReport& capability = device.capabilityReport();
        const auto identityDigest = canonicalIdentityDigest(processor.identity());
        if (!identityDigest.has_value()) {
            return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant);
        }
        GpuResidentPreviewQualificationReport report;
        report.deviceGeneration_ = capability.generation;
        report.ownershipEpoch_ = device.ownershipEpoch();
        report.deviceIdentity_ = capability.identity;
        report.primitiveSemanticsVersion_ = render::kCpuImagePrimitiveSemanticsVersion;
        report.coveredSemantics_ = std::string(kGpuResidentPreviewCoveredSemantics);
        report.dispatchSemantics_ = std::string(kGpuResidentPreviewDispatchSemantics);
        report.processorCacheId_ = processor.provenance().processorCacheId;
        report.processorIdentityDigest_ = *identityDigest;
        report.numericContract_ = std::string(kGpuResidentPreviewNumericContract);
        report.fixtureDigest_ = fixtureHasher.finalize();
        report.timings_ = std::move(timings);
        report.subnormalFrameRejected_ = subnormalRejected;
        report.outcome_ = GpuResidentPreviewOutcome::PreviewOnly;
        report.eligibleInterval_ = deriveEligibleInterval(report.timings_);
        report.diagnostic_ =
            report.eligibleInterval_.has_value()
                ? makeDiagnostic(GpuResidentPreviewDiagnosticCode::None,
                                 "every resident operation passed parity and a contiguous faster "
                                 "suffix was measured")
                : makeDiagnostic(
                      GpuResidentPreviewDiagnosticCode::TimingNotImproved,
                      "every resident operation passed parity but the largest measured size was "
                      "not faster; CPU-only");
        return report;
    } catch (...) {
        return fail(GpuResidentPreviewDiagnosticCode::InternalInvariant);
    }
}

} // namespace bloom::runtime
