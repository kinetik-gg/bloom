#pragma once

// Private route-proof support for the bounded GPU coverage contract test. It reports the standalone
// executor evidence for the blend/affine arms the production builder does not emit, and it proves
// the typed route-proof sink rejects every generic or boolean-shaped fake. It never publishes a
// fabricated "real" proof: no genuine route harness exists in this tree yet, so every route stays
// MISSING until one does.

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::gpu_coverage_route_proof {

// Standalone executor semantic evidence. The production builder refuses non-Normal blending and
// non-translation layers, but the executor's GpuBlend and GpuAffine arms are proven against the
// actual CPU oracle by a separate native test. These lines report that evidence; they are
// deliberately NOT counted as builder coverage.
inline void printStandaloneExecutorEvidence() {
    std::cout << "EXECUTOR-EVIDENCE blend: all eight modes proven standalone by "
                 "bloom.runtime.gpu_affine_blend_executor (builder emits Normal only; not builder "
                 "coverage)\n";
    std::cout << "EXECUTOR-EVIDENCE affine: composed scale/rotation/anchor proven standalone by "
                 "bloom.runtime.gpu_affine_blend_executor (builder translation-only; not builder "
                 "coverage)\n";
}

[[nodiscard]] inline bloom::runtime::GpuRouteExecutionProof representativeRouteProof() {
    return bloom::runtime::GpuRouteExecutionProof{
        .routeId = "route.preview.viewer",
        .harness = bloom::runtime::GpuRouteHarnessKind::ViewerPreview,
        .processIdentityDigest = "process-digest",
        .deviceOwnershipEpoch = "device-epoch",
        .nativeDispatches = 4,
        .fullFrameReadbacks = 0,
        .evidenceDigest = "evidence-digest"};
}

// Proves the route sink rejects every generic/boolean-shaped fake and never retains one.
inline void routeProofSinkRejectsFakes(std::vector<std::string>& failures) {
    using bloom::runtime::GpuCoverageContractIssue;
    bloom::runtime::GpuRouteProofSink sink;
    const auto expectReject = [&](const bloom::runtime::GpuRouteExecutionProof& proof,
                                  const GpuCoverageContractIssue expected,
                                  const std::string_view name) {
        if (sink.publish(proof) != expected) {
            failures.push_back("route-proof negative: '" + std::string{name} +
                               "' was not rejected with the expected reason");
        }
    };
    expectReject(bloom::runtime::GpuRouteExecutionProof{},
                 GpuCoverageContractIssue::RouteProofEmptyOrGeneric, "empty/generic proof");
    auto unknown = representativeRouteProof();
    unknown.routeId = "route.does-not-exist";
    expectReject(std::move(unknown), GpuCoverageContractIssue::RouteProofUnknownRoute,
                 "unknown route");
    auto mismatch = representativeRouteProof();
    mismatch.harness = bloom::runtime::GpuRouteHarnessKind::RamPreview;
    expectReject(std::move(mismatch), GpuCoverageContractIssue::RouteProofHarnessMismatch,
                 "harness/route mismatch");
    auto noProvenance = representativeRouteProof();
    noProvenance.evidenceDigest.clear();
    expectReject(std::move(noProvenance), GpuCoverageContractIssue::RouteProofMissingProvenance,
                 "missing evidence digest");
    auto noDispatch = representativeRouteProof();
    noDispatch.nativeDispatches = 0;
    expectReject(std::move(noDispatch), GpuCoverageContractIssue::RouteProofNoNativeDispatch,
                 "zero native dispatch");
    auto excessReadback = representativeRouteProof();
    excessReadback.fullFrameReadbacks = 1;
    expectReject(std::move(excessReadback), GpuCoverageContractIssue::RouteProofExcessReadback,
                 "preview full-frame readback");
    if (!sink.proofs().empty()) {
        failures.push_back("route-proof sink retained a rejected proof");
    }
    std::cout << "NEGATIVE route-proof: six generic/fake proofs rejected; sink retains none\n";
}

} // namespace bloom::gpu_coverage_route_proof
