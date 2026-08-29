#include <bloom/project/staged_copy.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/zip_container.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Drives a REAL bloom::platform::StagedArtifactCoordinator over a per-test temporary directory,
// following src/project/tests/staged_save_tests.cpp's own pattern one sibling module over: a
// TempDirectory RAII helper and a thin coordinator/request-building layer, reused here because
// stageCopyArchive() only makes sense exercised against the real platform lease state machine and
// its fault-injection hooks.

namespace {

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::PublicationDisposition;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;
using bloom::platform::StagedArtifactError;
using bloom::platform::StagedArtifactFaultPoint;
using bloom::platform::StagedArtifactLease;
using bloom::platform::StagedArtifactPreflightRequest;
using bloom::platform::StagedArtifactPublicationOutcome;

using bloom::project::buildSaveArchive;
using bloom::project::CanonicalDocumentV1;
using bloom::project::CanonicalManifestV1;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::SaveArchiveLimits;
using bloom::project::stageCopyArchive;
using bloom::project::StagedCopyByteMismatch;
using bloom::project::StagedCopyContainerSanityFailure;
using bloom::project::StagedCopyResourceExhausted;
using bloom::project::StagedCopyResult;
using bloom::project::StagedCopyStage;
using bloom::project::StagedSaveLeaseCall;
using bloom::project::StagedSavePlatformFailure;
using bloom::project::ZipContainerError;
using bloom::project::ZipContainerLimits;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-staged-copy-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------------------------
// Shared plumbing
// ---------------------------------------------------------------------------------------------

constexpr std::uint64_t kGenerousOperationBudget = 32ULL << 20U; // 32 MiB: ample for every fixture.

[[nodiscard]] ProjectIoOperationMemory makeOperation(const std::uint64_t limitBytes) {
    auto coordinator = ProjectIoMemoryCoordinator::create(limitBytes);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation = coordinator->createOperation(limitBytes, limitBytes);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
}

[[nodiscard]] ProjectIoOperationMemory makeOperation() {
    return makeOperation(kGenerousOperationBudget);
}

[[nodiscard]] std::array<std::uint8_t, 32> ascendingDigestBytes() noexcept {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes;
}

[[nodiscard]] bloom::document::ColorSettings neutralColorSettings() {
    return bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(ascendingDigestBytes()));
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] std::optional<StagedArtifactCoordinator>
makeCoordinator(Expectations& expectations, const StagedArtifactConfig config = {}) {
    auto result = StagedArtifactCoordinator::create(config);
    expectations.expect(result.succeeded(), "the staged-copy coordinator is created");
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeCoordinator();
}

[[nodiscard]] StagedArtifactPreflightRequest makeRequest(
    std::filesystem::path targetPath,
    const ArtifactOverwritePolicy overwritePolicy = ArtifactOverwritePolicy::CreateOrReplace) {
    return {.targetPath = std::move(targetPath),
            .overwritePolicy = overwritePolicy,
            .expectedTarget = std::nullopt};
}

// A real, valid, two-entry Constrained ZIP Profile archive -- the exact shape a preserved-read-only
// project's retained original archive bytes would take (docs/architecture/project-format.md's
// "Versions, Migrations, And Preservation": "Open returns a preserved-read-only result containing
// the bounded original archive"). stageCopyArchive() has no build step of its own; it copies
// whatever bytes it is given verbatim, so this fixture builds real archive bytes purely to serve as
// realistic, preflight-passing source content.
[[nodiscard]] std::vector<std::byte> buildSourceArchiveBytesOrAbort() {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Copy Source Project", "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    const auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};
    auto built = buildSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    if (!built) {
        std::abort();
    }
    const auto bytes = built.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

struct StageAttempt final {
    std::optional<StagedArtifactLease> lease;
    std::optional<StagedCopyResult> result;
};

[[nodiscard]] StageAttempt
attemptStage(Expectations& expectations, StagedArtifactCoordinator& coordinator,
             const std::filesystem::path& targetPath, const ArtifactOverwritePolicy policy,
             const std::span<const std::byte> sourceBytes, const ZipContainerLimits& limits,
             ProjectIoOperationMemory operation) {
    StageAttempt attempt;
    auto preflight = coordinator.preflight(makeRequest(targetPath, policy));
    if (!preflight) {
        expectations.expect(false, "attemptStage: preflight succeeds");
        return attempt;
    }
    auto stageResult = coordinator.stage(std::move(preflight).takeTarget());
    if (!stageResult) {
        expectations.expect(false, "attemptStage: stage succeeds");
        return attempt;
    }
    attempt.lease = std::move(stageResult).takeLease();
    attempt.result = stageCopyArchive(*attempt.lease, sourceBytes, limits, std::move(operation));
    return attempt;
}

// ---------------------------------------------------------------------------------------------
// Green path: preflight -> stage -> stageCopyArchive -> success -> caller-side publish(Proceed) ->
// Published. The published file is byte-compared against the source bytes directly -- Save Copy's
// literal contract.
// ---------------------------------------------------------------------------------------------

void testGreenPathEndToEnd(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "green path: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();

    auto attempt =
        attemptStage(expectations, *coordinator, targetPath, ArtifactOverwritePolicy::CreateOnly,
                     sourceBytes, ZipContainerLimits{}, makeOperation());
    expectations.expect(attempt.lease.has_value() && attempt.result.has_value() &&
                            static_cast<bool>(*attempt.result),
                        "green path: stageCopyArchive succeeds");
    if (!attempt.lease.has_value() || !attempt.result.has_value() || !*attempt.result) {
        return;
    }
    const auto publication = attempt.lease->publish(PublicationDisposition::Proceed);
    expectations.expect(publication.outcome == StagedArtifactPublicationOutcome::Published,
                        "green path: publish reaches Published");
    if (publication.outcome != StagedArtifactPublicationOutcome::Published) {
        return;
    }

    const auto published = readFile(targetPath);
    expectations.expect(
        published.size() == sourceBytes.size() &&
            std::memcmp(published.data(), sourceBytes.data(), published.size()) == 0,
        "green path: the published file is byte-for-byte identical to the source bytes");
}

// ---------------------------------------------------------------------------------------------
// Fault injection at StageWrite, StageWriterClose (finishWriting), StageVerificationRead, and
// StageVerificationAccept: each surfaces the platform's typed FaultInjected error at the right
// stage/lease-call of stageCopyArchive()'s result, mirroring staged_save_tests.cpp's identical
// coverage, and every lease releases its active-target admission on drop.
// ---------------------------------------------------------------------------------------------

void testFaultInjection(Expectations& expectations) {
    struct FaultCase final {
        StagedArtifactFaultPoint point;
        StagedCopyStage expectedStage;
        StagedSaveLeaseCall expectedCall;
    };
    const std::array<FaultCase, 4> cases{{
        {StagedArtifactFaultPoint::StageWrite, StagedCopyStage::StageWrite,
         StagedSaveLeaseCall::Write},
        {StagedArtifactFaultPoint::StageWriterClose, StagedCopyStage::StageFinish,
         StagedSaveLeaseCall::FinishWriting},
        {StagedArtifactFaultPoint::StageVerificationRead, StagedCopyStage::StageRead,
         StagedSaveLeaseCall::ReadForVerification},
        {StagedArtifactFaultPoint::StageVerificationAccept, StagedCopyStage::Accept,
         StagedSaveLeaseCall::AcceptVerification},
    }};

    const auto sourceBytes = buildSourceArchiveBytesOrAbort();

    for (const auto& testCase : cases) {
        TempDirectory directory;
        if (!directory.isValid()) {
            expectations.expect(false, "fault injection: temp directory is available");
            continue;
        }
        StagedArtifactConfig config;
        config.faults = {.point = testCase.point, .occurrence = 1};
        auto coordinator = makeCoordinator(expectations, config);
        if (!coordinator.has_value()) {
            continue;
        }
        const auto targetPath = directory.path() / "copy.bloom";

        auto attempt = attemptStage(expectations, *coordinator, targetPath,
                                    ArtifactOverwritePolicy::CreateOnly, sourceBytes,
                                    ZipContainerLimits{}, makeOperation());
        expectations.expect(attempt.lease.has_value(),
                            "fault injection: a lease is obtained before the injected failure");
        expectations.expect(attempt.result.has_value() && !*attempt.result,
                            "fault injection: stageCopyArchive reports failure");
        if (!attempt.result.has_value()) {
            continue;
        }
        const auto* failure = attempt.result->failure();
        expectations.expect(failure != nullptr && failure->stage() == testCase.expectedStage,
                            "fault injection: the failure stage matches the injected fault point");
        const auto* platformFailure =
            failure != nullptr ? failure->payloadAs<StagedSavePlatformFailure>() : nullptr;
        expectations.expect(platformFailure != nullptr &&
                                platformFailure->error == StagedArtifactError::FaultInjected &&
                                platformFailure->call == testCase.expectedCall,
                            "fault injection: the platform failure payload names FaultInjected and "
                            "the exact lease call");

        attempt.lease.reset();
        const auto snapshot = coordinator->snapshot();
        expectations.expect(snapshot.activeTargetCount == 0,
                            "fault injection: dropping the lease releases the active target "
                            "admission");
    }
}

// ---------------------------------------------------------------------------------------------
// Container sanity-check failure (check (b)): source bytes that are not a conforming Constrained
// ZIP Profile container round-trip byte-for-byte (there is no disk corruption -- write, read back,
// and compare all agree), but detail::preflightZipContainer() over the read-back bytes still
// legitimately fails. This is reachable through ordinary public parameters -- stageCopyArchive()
// itself never requires its caller's `sourceBytes` to already be a conforming container, only the
// application-layer contract that Save Copy's caller opened the source successfully does -- unlike
// a genuine byte-content MISMATCH (check (a)), which is NOT reachable this way: see the comment
// block below.
// ---------------------------------------------------------------------------------------------

void testContainerSanityCheckFailure(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "container sanity check: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = asBytes("this is not a zip container at all");

    auto preflight =
        coordinator->preflight(makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly));
    expectations.expect(static_cast<bool>(preflight), "container sanity check: preflight succeeds");
    if (!preflight) {
        return;
    }
    auto stageResult = coordinator->stage(std::move(preflight).takeTarget());
    expectations.expect(static_cast<bool>(stageResult), "container sanity check: stage succeeds");
    if (!stageResult) {
        return;
    }
    auto lease = std::move(stageResult).takeLease();

    auto result = stageCopyArchive(lease, sourceBytes, ZipContainerLimits{}, makeOperation());
    expectations.expect(!result, "container sanity check: stageCopyArchive reports failure");
    const auto* failure = result.failure();
    expectations.expect(failure != nullptr && failure->stage() == StagedCopyStage::Verification,
                        "container sanity check: reported at the Verification stage");
    const auto* sanityFailure =
        failure != nullptr ? failure->payloadAs<StagedCopyContainerSanityFailure>() : nullptr;
    expectations.expect(sanityFailure != nullptr && sanityFailure->error != ZipContainerError::None,
                        "container sanity check: the payload names a real ZipContainerError, typed "
                        "distinctly from a byte mismatch");
    expectations.expect(!result.rejectDiagnostic().has_value(),
                        "container sanity check: lease.rejectVerification() itself succeeded (no "
                        "secondary diagnostic)");

    const auto publication = lease.publish(PublicationDisposition::Proceed);
    expectations.expect(
        publication.outcome == StagedArtifactPublicationOutcome::FailedBeforePublication &&
            publication.error == StagedArtifactError::StageVerificationRejected,
        "container sanity check: the lease replays its rejected terminal state on publish");
}

// ---------------------------------------------------------------------------------------------
// Byte mismatch (check (a)) and staged-size disagreement are NOT covered by an automated test here,
// documented honestly rather than faked: stageCopyArchive() issues exactly one
// lease.write(sourceBytes) call with the complete source buffer, reads back exactly what
// lease.stageBytes() reports was staged, and compares that read-back to the SAME `sourceBytes`
// value the caller passed in -- there is only one source of truth throughout the call, no second
// independently-derived "expected" value to legitimately disagree with it (unlike staged_save.cpp's
// Verification-stage content mismatch, which compares staged bytes against an independently-derived
// expected value built from the manifest/document that can be made self-inconsistent through
// ordinary public parameters; Save Copy's write and its own read-back verification of the identical
// bytes have no such second lever). A genuine disagreement between what was written and what reads
// back can only come from real platform-level corruption between finishWriting() and the read-back
// loop, which the public StagedArtifactLease API and its shipped StagedArtifactFaultPoint values
// (all of which report a typed FaultInjected error rather than silently corrupting bytes -- see
// testFaultInjection above) cannot produce, and this task package forbids adding a production test
// seam solely to make it reachable. Reaching the platform's staged bytes from outside
// stageCopyArchive() for direct tamper is therefore not possible without a forbidden seam, exactly
// as staged_save_tests.cpp documents for its own StagedSizeDisagreement case.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// Budget exhaustion, zeroed snapshot. Unlike staged_save's build-then-verify chain,
// stageCopyArchive() has no build step: the only allocation charged through `operation` is the PMR
// read-back buffer, sized to exactly `sourceBytes.size()`. A budget smaller than that size is
// therefore guaranteed to exhaust at ReadBackAllocation specifically -- no two-pass probing needed.
// ---------------------------------------------------------------------------------------------

void testBudgetExhaustion(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "budget exhaustion: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();
    expectations.expect(sourceBytes.size() > 1,
                        "budget exhaustion: the fixture archive is non-trivial");

    const std::uint64_t budget = static_cast<std::uint64_t>(sourceBytes.size()) - 1;
    auto memory = ProjectIoMemoryCoordinator::create(kGenerousOperationBudget);
    expectations.expect(memory.has_value(), "budget exhaustion: coordinator constructs");
    if (!memory.has_value()) {
        return;
    }
    auto operation = memory->createOperation(budget, budget);
    expectations.expect(operation.has_value(),
                        "budget exhaustion: constrained operation constructs");
    if (!operation.has_value()) {
        return;
    }

    auto preflight =
        coordinator->preflight(makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly));
    expectations.expect(static_cast<bool>(preflight), "budget exhaustion: preflight succeeds");
    if (!preflight) {
        return;
    }
    auto stageResult = coordinator->stage(std::move(preflight).takeTarget());
    expectations.expect(static_cast<bool>(stageResult), "budget exhaustion: stage succeeds");
    if (!stageResult) {
        return;
    }
    {
        auto lease = std::move(stageResult).takeLease();
        auto result =
            stageCopyArchive(lease, sourceBytes, ZipContainerLimits{}, std::move(*operation));
        expectations.expect(!result, "budget exhaustion: a budget below the source size fails");
        const auto* failure = result.failure();
        expectations.expect(failure != nullptr &&
                                failure->stage() == StagedCopyStage::ReadBackAllocation,
                            "budget exhaustion: reported at the ReadBackAllocation stage");
        const auto* resourceExhausted =
            failure != nullptr ? failure->payloadAs<StagedCopyResourceExhausted>() : nullptr;
        expectations.expect(resourceExhausted != nullptr,
                            "budget exhaustion: the payload names StagedCopyResourceExhausted");
    } // The lease is destroyed here; its stage cleanup is exercised by this scope exit.
    const auto memorySnapshot = memory->snapshot();
    expectations.expect(memorySnapshot.currentBytes == 0,
                        "budget exhaustion: the constrained coordinator's charge returns to zero");
    const auto artifactSnapshot = coordinator->snapshot();
    expectations.expect(artifactSnapshot.activeTargetCount == 0,
                        "budget exhaustion: the lease remained safely destructible after resource "
                        "exhaustion");
}

} // namespace

int main() {
    Expectations expectations;
    testGreenPathEndToEnd(expectations);
    testFaultInjection(expectations);
    testContainerSanityCheckFailure(expectations);
    testBudgetExhaustion(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
