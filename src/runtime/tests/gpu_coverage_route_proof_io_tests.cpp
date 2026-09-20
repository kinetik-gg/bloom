// Unit tests for the route-proof handoff codec. Every proof here is a clearly synthetic, unit-only
// object written to a private temporary directory; none of them is ever published into a route
// acceptance directory, and a synthetic proof never makes a route pass.

#include "gpu_route_proof_io.hpp"

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using bloom::gpu_route_proof_io::RouteProofIoStatus;
using bloom::runtime::GpuCoverageContractIssue;
using bloom::runtime::GpuRouteExecutionProof;
using bloom::runtime::GpuRouteHarnessKind;

std::size_t g_failures = 0;

void expect(const bool condition, const std::string_view what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

// Synthetic, canonical 64-character lowercase SHA-256 unit digests.
constexpr std::string_view kUnitFrameDigest =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kUnitEvidenceDigest =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

[[nodiscard]] GpuRouteExecutionProof unitPreviewProof() {
    return GpuRouteExecutionProof{.routeId = "route.preview.viewer",
                                  .harness = GpuRouteHarnessKind::ViewerPreview,
                                  .processIdentityDigest = std::string{kUnitFrameDigest},
                                  .deviceOwnershipEpoch = "987654",
                                  .nativeDispatches = 3,
                                  .verifiedFrames = 1,
                                  .readbackSubmissions = 0,
                                  .payloads = 0,
                                  .transferredBytes = 0,
                                  .evidenceDigest = std::string{kUnitEvidenceDigest}};
}

[[nodiscard]] GpuRouteExecutionProof unitCombinedExportProof() {
    return GpuRouteExecutionProof{.routeId = "route.export.video",
                                  .harness = GpuRouteHarnessKind::VideoExport,
                                  .processIdentityDigest = std::string{kUnitFrameDigest},
                                  .deviceOwnershipEpoch = "987654",
                                  .nativeDispatches = 5,
                                  .verifiedFrames = 2,
                                  .readbackSubmissions = 2,
                                  .payloads = 4,
                                  .transferredBytes = 123456,
                                  .evidenceDigest = std::string{kUnitEvidenceDigest}};
}

[[nodiscard]] std::filesystem::path makeScratchDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("bloom-route-proof-io-" + std::to_string(stamp));
}

void testRoundTrip(const std::filesystem::path& dir) {
    const std::string nonce = "unit-nonce-1";
    expect(bloom::gpu_route_proof_io::writeRunNonce(dir, nonce) == RouteProofIoStatus::Ok,
           "nonce writes");
    std::string readBack;
    expect(bloom::gpu_route_proof_io::readRunNonce(dir, readBack) == RouteProofIoStatus::Ok &&
               readBack == nonce,
           "nonce round-trips");
    const auto proof = unitPreviewProof();
    expect(bloom::gpu_route_proof_io::writeRouteProof(dir, nonce, proof) == RouteProofIoStatus::Ok,
           "preview proof writes");
    GpuRouteExecutionProof loaded;
    expect(bloom::gpu_route_proof_io::readRouteProof(dir, proof.routeId, nonce, loaded) ==
                   RouteProofIoStatus::Ok &&
               loaded.routeId == proof.routeId && loaded.harness == proof.harness &&
               loaded.processIdentityDigest == proof.processIdentityDigest &&
               loaded.deviceOwnershipEpoch == proof.deviceOwnershipEpoch &&
               loaded.nativeDispatches == proof.nativeDispatches &&
               loaded.verifiedFrames == proof.verifiedFrames && loaded.payloads == proof.payloads &&
               loaded.transferredBytes == proof.transferredBytes,
           "preview proof round-trips exactly");
    const auto exportProof = unitCombinedExportProof();
    expect(bloom::gpu_route_proof_io::writeRouteProof(dir, nonce, exportProof) ==
               RouteProofIoStatus::Ok,
           "combined export proof writes (two payloads, one submission per frame)");
    const auto loadedAll = bloom::gpu_route_proof_io::readKnownRouteProofs(dir, nonce);
    bool exportAccepted = false;
    for (const auto& result : loadedAll) {
        if (result.routeId == exportProof.routeId) {
            exportAccepted = result.accepted;
        }
    }
    expect(exportAccepted, "combined export proof validates with two payloads, not one readback");
}

void testValidationNegatives() {
    const auto expectIssue = [](const GpuRouteExecutionProof& proof,
                                const GpuCoverageContractIssue issue, const std::string_view what) {
        const auto issues = bloom::runtime::validateGpuRouteExecutionProof(proof);
        expect(!issues.empty() && issues.front().issue == issue, what);
    };
    auto zeroDispatch = unitPreviewProof();
    zeroDispatch.nativeDispatches = 0;
    expectIssue(zeroDispatch, GpuCoverageContractIssue::RouteProofNoNativeDispatch,
                "zero cold dispatch rejected");
    auto zeroFrame = unitPreviewProof();
    zeroFrame.verifiedFrames = 0;
    expectIssue(zeroFrame, GpuCoverageContractIssue::RouteProofNoVerifiedFrame,
                "zero verified frame rejected");
    auto zeroEpoch = unitPreviewProof();
    zeroEpoch.deviceOwnershipEpoch = "0000";
    expectIssue(zeroEpoch, GpuCoverageContractIssue::RouteProofMissingProvenance,
                "zero device epoch rejected");
    auto alphaEpoch = unitPreviewProof();
    alphaEpoch.deviceOwnershipEpoch = "abc";
    expectIssue(alphaEpoch, GpuCoverageContractIssue::RouteProofMissingProvenance,
                "non-decimal device epoch rejected");
    auto negativeEpoch = unitPreviewProof();
    negativeEpoch.deviceOwnershipEpoch = "-1";
    expectIssue(negativeEpoch, GpuCoverageContractIssue::RouteProofMissingProvenance,
                "negative device epoch rejected");
    auto shortDigest = unitPreviewProof();
    shortDigest.processIdentityDigest = "not-a-sha256";
    expectIssue(shortDigest, GpuCoverageContractIssue::RouteProofMissingProvenance,
                "non-canonical frame digest rejected");
    auto uppercaseDigest = unitPreviewProof();
    uppercaseDigest.evidenceDigest =
        "FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210";
    expectIssue(uppercaseDigest, GpuCoverageContractIssue::RouteProofMissingProvenance,
                "uppercase evidence digest rejected");
    auto previewReadback = unitPreviewProof();
    previewReadback.readbackSubmissions = 1;
    expectIssue(previewReadback, GpuCoverageContractIssue::RouteProofExcessReadback,
                "preview readback rejected");
    auto threePayloads = unitCombinedExportProof();
    threePayloads.verifiedFrames = 1;
    threePayloads.readbackSubmissions = 1;
    threePayloads.payloads = 3; // one verified frame -> at most two payloads
    expectIssue(threePayloads, GpuCoverageContractIssue::RouteProofExcessReadback,
                "three payloads for one frame rejected");
    auto fewerPayloads = unitCombinedExportProof();
    fewerPayloads.payloads = 1; // two submissions -> at least two payloads
    expectIssue(fewerPayloads, GpuCoverageContractIssue::RouteProofExcessReadback,
                "fewer payloads than submissions rejected");
    auto overflowPayloads = unitCombinedExportProof();
    overflowPayloads.verifiedFrames = 1;
    overflowPayloads.readbackSubmissions = 1;
    overflowPayloads.payloads = std::numeric_limits<std::uint64_t>::max();
    expectIssue(overflowPayloads, GpuCoverageContractIssue::RouteProofExcessReadback,
                "payloads at UINT64_MAX rejected without overflow");
    auto overflowSubmissions = unitCombinedExportProof();
    overflowSubmissions.verifiedFrames = 1;
    overflowSubmissions.readbackSubmissions = std::numeric_limits<std::uint64_t>::max();
    expectIssue(overflowSubmissions, GpuCoverageContractIssue::RouteProofExcessReadback,
                "submissions at UINT64_MAX rejected");
    auto noBytes = unitCombinedExportProof();
    noBytes.transferredBytes = 0;
    expectIssue(noBytes, GpuCoverageContractIssue::RouteProofMissingTransferredBytes,
                "export without transferred bytes rejected");
    expect(bloom::runtime::validateGpuRouteExecutionProof(unitCombinedExportProof()).empty(),
           "combined export with two payloads per frame validates");
}

void testCodecNegatives(const std::filesystem::path& dir) {
    const std::string nonce = "unit-nonce-2";
    expect(bloom::gpu_route_proof_io::writeRunNonce(dir, nonce) == RouteProofIoStatus::Ok,
           "second nonce writes");
    const auto proof = unitPreviewProof();
    const auto path = bloom::gpu_route_proof_io::routeProofPath(dir, proof.routeId);
    // A hand-written malformed record (duplicate key) is not produced by writeRouteProof.
    expect(bloom::gpu_route_proof_io::writeBoundedFileAtomic(path, "schema=x\nschema=y\n") ==
               RouteProofIoStatus::Ok,
           "malformed sample written directly");
    GpuRouteExecutionProof loaded;
    expect(bloom::gpu_route_proof_io::readRouteProof(dir, proof.routeId, nonce, loaded) ==
               RouteProofIoStatus::DuplicateOrConflicting,
           "duplicate record rejected");
    expect(bloom::gpu_route_proof_io::writeBoundedFileAtomic(path, "schema=other\n") ==
               RouteProofIoStatus::Ok,
           "wrong-schema sample written directly");
    expect(bloom::gpu_route_proof_io::readRouteProof(dir, proof.routeId, nonce, loaded) ==
               RouteProofIoStatus::SchemaMismatch,
           "schema mismatch rejected");
    expect(bloom::gpu_route_proof_io::writeRouteProof(dir, nonce, proof) == RouteProofIoStatus::Ok,
           "valid proof rewritten");
    expect(bloom::gpu_route_proof_io::readRouteProof(dir, proof.routeId, "other-nonce", loaded) ==
               RouteProofIoStatus::StaleNonce,
           "stale nonce rejected");
    // This route file is never written in this test.
    expect(bloom::gpu_route_proof_io::readRouteProof(dir, "route.export.headless_scripted", nonce,
                                                     loaded) == RouteProofIoStatus::Missing,
           "missing proof reported Missing");
    // A proof whose route field does not match its file name is conflicting: an export proof
    // written to the preview path.
    const auto mismatched =
        bloom::gpu_route_proof_io::serializeRouteProof(nonce, unitCombinedExportProof());
    expect(bloom::gpu_route_proof_io::writeBoundedFileAtomic(path, mismatched) ==
               RouteProofIoStatus::Ok,
           "mismatched-route sample written");
    expect(bloom::gpu_route_proof_io::readRouteProof(dir, proof.routeId, nonce, loaded) ==
               RouteProofIoStatus::DuplicateOrConflicting,
           "route/file mismatch reported conflicting");
}

void testClearOnlyKnownFiles(const std::filesystem::path& dir) {
    const std::string nonce = "unit-nonce-3";
    expect(bloom::gpu_route_proof_io::writeRunNonce(dir, nonce) == RouteProofIoStatus::Ok,
           "clear-test nonce writes");
    expect(bloom::gpu_route_proof_io::writeRouteProof(dir, nonce, unitPreviewProof()) ==
               RouteProofIoStatus::Ok,
           "clear-test proof writes");
    const auto foreign = dir / "foreign.txt";
    expect(bloom::gpu_route_proof_io::writeBoundedFileAtomic(foreign, "keep me\n") ==
               RouteProofIoStatus::Ok,
           "foreign file writes");
    expect(bloom::gpu_route_proof_io::clearKnownProofFiles(dir) == RouteProofIoStatus::Ok,
           "known files clear");
    expect(!std::filesystem::exists(
               bloom::gpu_route_proof_io::routeProofPath(dir, "route.preview.viewer")),
           "known proof removed");
    expect(std::filesystem::exists(bloom::gpu_route_proof_io::routeProofNoncePath(dir)),
           "nonce retained");
    expect(std::filesystem::exists(foreign), "foreign file retained");
    expect(!std::filesystem::exists(dir / "route.preview.viewer.proof.tmp"),
           "atomic write leaves no temp file");
}

} // namespace

int main() {
    const auto dir = makeScratchDirectory();
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    testRoundTrip(dir);
    testValidationNegatives();
    testCodecNegatives(dir);
    testClearOnlyKnownFiles(dir);
    std::filesystem::remove_all(dir, error);
    if (g_failures != 0) {
        std::cerr << g_failures << " route-proof io expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: route-proof io codec unit tests\n";
    return 0;
}
