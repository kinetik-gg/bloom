#pragma once

// The typed render-route proof contract, split out of gpu_coverage_contract.hpp to keep both
// headers bounded. It depends on the coverage contract's issue/diagnostic vocabulary and route
// list, so it includes that header; the coverage contract deliberately does not include this one,
// so a consumer that only classifies operations/features does not pull in the route proof
// machinery.

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace bloom::runtime {

// The closed set of production harnesses that can prove one render route. A route can only be
// proven by the harness that owns it; a generic "verified" flag or a bare bool cannot stand in.
enum class GpuRouteHarnessKind : std::uint8_t {
    None = 0,
    ViewerPreview,
    RamPreview,
    StillFrameExport,
    SequenceRangeExport,
    VideoExport,
    HeadlessScripted,
};

[[nodiscard]] constexpr std::string_view
gpuRouteHarnessRouteId(const GpuRouteHarnessKind harness) noexcept {
    switch (harness) {
    case GpuRouteHarnessKind::ViewerPreview:
        return "route.preview.viewer";
    case GpuRouteHarnessKind::RamPreview:
        return "route.preview.ram_preview";
    case GpuRouteHarnessKind::StillFrameExport:
        return "route.export.still_frame";
    case GpuRouteHarnessKind::SequenceRangeExport:
        return "route.export.sequence_range";
    case GpuRouteHarnessKind::VideoExport:
        return "route.export.video";
    case GpuRouteHarnessKind::HeadlessScripted:
        return "route.export.headless_scripted";
    case GpuRouteHarnessKind::None:
        return {};
    }
    return {};
}

// Preview routes are readback-free by construction; only a final-render route may read the
// composited image back at the codec/file boundary.
[[nodiscard]] constexpr bool gpuRouteAllowsFinalReadback(const std::string_view routeId) noexcept {
    return routeId == "route.export.still_frame" || routeId == "route.export.sequence_range" ||
           routeId == "route.export.video" || routeId == "route.export.headless_scripted";
}

// The typed proof an externally executed production route must publish. Every field is real
// provenance captured from the genuine run: the digest of the ordered per-frame production process
// identities, the producer-captured nonzero device ownership epoch (different processes
// legitimately have different epochs, so a consumer never compares it to its own device), the
// positive COLD native dispatch count (a warm cache rerun may dispatch nothing), the verified frame
// count, the final readback submission and payload counts, the explicit transferred bytes, and a
// captured evidence digest. Readback submissions and payloads are distinct typed totals: one final
// submission may truthfully download two payloads (process-analysis + encoded output) and must not
// be relabelled as one readback. It is deliberately structured so a bool or a generic fake cannot
// satisfy it, and no proof is registered in this repository today: every route stays MISSING until
// its real harness publishes one.
struct GpuRouteExecutionProof final {
    std::string routeId;
    GpuRouteHarnessKind harness = GpuRouteHarnessKind::None;
    std::string processIdentityDigest;
    std::string deviceOwnershipEpoch;
    std::uint64_t nativeDispatches = 0;
    std::uint64_t verifiedFrames = 0;
    std::uint64_t readbackSubmissions = 0;
    std::uint64_t payloads = 0;
    std::uint64_t transferredBytes = 0;
    std::string evidenceDigest;
};

// A nonzero device ownership epoch is a valid uint64 decimal string. The whole string must parse:
// "abc", "-1", "+1", " 1", and "0" (any number of zeros) are all rejected. The producer captures
// it; the consumer only checks it is a real nonzero epoch, never that it matches its own device.
[[nodiscard]] inline bool gpuRouteProofEpochIsNonzero(const std::string_view epoch) noexcept {
    if (epoch.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const auto* const begin = epoch.data();
    const auto* const end = epoch.data() + epoch.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    return value != 0;
}

// A canonical SHA-256 digest is exactly 64 lowercase hexadecimal characters. An arbitrary nonempty
// string (or a truncated/uppercase one) is not evidence.
[[nodiscard]] inline bool
gpuRouteProofDigestIsCanonicalSha256(const std::string_view digest) noexcept {
    if (digest.size() != 64) {
        return false;
    }
    for (const char character : digest) {
        const bool lowerHex =
            (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        if (!lowerHex) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::vector<GpuCoverageContractDiagnostic>
validateGpuRouteExecutionProof(const GpuRouteExecutionProof& proof) {
    std::vector<GpuCoverageContractDiagnostic> issues;
    if (proof.routeId.empty() || proof.harness == GpuRouteHarnessKind::None) {
        issues.push_back({GpuCoverageContractIssue::RouteProofEmptyOrGeneric,
                          "a route proof has no route id or harness kind"});
        return issues;
    }
    bool known = false;
    for (const auto& route : gpuRenderRouteCoverage()) {
        known = known || route.id == proof.routeId;
    }
    if (!known) {
        issues.push_back({GpuCoverageContractIssue::RouteProofUnknownRoute,
                          "route proof names an unknown route '" + proof.routeId + "'"});
        return issues;
    }
    if (gpuRouteHarnessRouteId(proof.harness) != proof.routeId) {
        issues.push_back(
            {GpuCoverageContractIssue::RouteProofHarnessMismatch,
             "route proof for '" + proof.routeId + "' was produced by a different harness"});
    }
    if (!gpuRouteProofDigestIsCanonicalSha256(proof.processIdentityDigest) ||
        !gpuRouteProofDigestIsCanonicalSha256(proof.evidenceDigest) ||
        !gpuRouteProofEpochIsNonzero(proof.deviceOwnershipEpoch)) {
        issues.push_back({GpuCoverageContractIssue::RouteProofMissingProvenance,
                          "route proof for '" + proof.routeId +
                              "' omits a canonical SHA-256 per-frame identity/evidence digest or a "
                              "valid nonzero device epoch"});
    }
    if (proof.nativeDispatches == 0) {
        issues.push_back(
            {GpuCoverageContractIssue::RouteProofNoNativeDispatch,
             "route proof for '" + proof.routeId +
                 "' recorded zero COLD native dispatches (a CPU fallback or warm-only run is not a "
                 "GPU pass)"});
    }
    if (proof.verifiedFrames == 0) {
        issues.push_back({GpuCoverageContractIssue::RouteProofNoVerifiedFrame,
                          "route proof for '" + proof.routeId +
                              "' recorded no verified frame with real per-frame identity"});
    }
    const bool finalRender = gpuRouteAllowsFinalReadback(proof.routeId);
    if (!finalRender) {
        if (proof.readbackSubmissions != 0 || proof.payloads != 0 || proof.transferredBytes != 0) {
            issues.push_back({GpuCoverageContractIssue::RouteProofExcessReadback,
                              "preview route proof '" + proof.routeId +
                                  "' recorded a final readback, payload, or transfer"});
        }
        return issues;
    }
    // One final submission per verified frame; up to two final payloads per frame (analysis +
    // encoded output), and never fewer payloads than submissions. Payloads are counted separately
    // from submissions, and the 2x bound is computed without overflowing UINT64_MAX.
    const auto maxPayloads = proof.verifiedFrames > std::numeric_limits<std::uint64_t>::max() / 2U
                                 ? std::numeric_limits<std::uint64_t>::max()
                                 : proof.verifiedFrames * 2U;
    if (proof.readbackSubmissions == 0 || proof.readbackSubmissions > proof.verifiedFrames ||
        proof.payloads == 0 || proof.payloads > maxPayloads ||
        proof.payloads < proof.readbackSubmissions) {
        issues.push_back(
            {GpuCoverageContractIssue::RouteProofExcessReadback,
             "export route proof '" + proof.routeId +
                 "' exceeds one final submission and two final payloads per verified frame, or has "
                 "fewer payloads than submissions"});
    }
    if (proof.transferredBytes == 0) {
        issues.push_back(
            {GpuCoverageContractIssue::RouteProofMissingTransferredBytes,
             "export route proof '" + proof.routeId + "' omits transferred-byte evidence"});
    }
    return issues;
}

// The sink a real, externally executed route harness publishes into. A proof is retained only when
// it validates; an empty/generic or fake proof is rejected by name. The gate reads this sink and
// keeps every route MISSING while it is empty.
class GpuRouteProofSink final {
  public:
    [[nodiscard]] GpuCoverageContractIssue publish(const GpuRouteExecutionProof& proof) {
        const auto issues = validateGpuRouteExecutionProof(proof);
        if (!issues.empty()) {
            return issues.front().issue;
        }
        proofs_.push_back(proof);
        return GpuCoverageContractIssue::None;
    }

    [[nodiscard]] const std::vector<GpuRouteExecutionProof>& proofs() const noexcept {
        return proofs_;
    }

    [[nodiscard]] const GpuRouteExecutionProof*
    find(const std::string_view routeId) const noexcept {
        for (const auto& proof : proofs_) {
            if (proof.routeId == routeId) {
                return &proof;
            }
        }
        return nullptr;
    }

  private:
    std::vector<GpuRouteExecutionProof> proofs_;
};

} // namespace bloom::runtime
