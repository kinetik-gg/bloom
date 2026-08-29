#include <bloom/project/staged_save.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Drives a REAL bloom::platform::StagedArtifactCoordinator over a per-test temporary directory,
// following the pattern (not the file) of src/platform/tests/staged_artifact_tests.cpp: a
// TempDirectory RAII helper and a thin coordinator/request-building layer, reused here because
// stageSaveArchive() only makes sense exercised against the real platform lease state machine
// (see the type's own file comment) and its fault-injection hooks.

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
using bloom::project::canonicalDocumentSize;
using bloom::project::CanonicalDocumentV1;
using bloom::project::canonicalManifestSize;
using bloom::project::CanonicalManifestV1;
using bloom::project::encodeCanonicalDocument;
using bloom::project::encodeCanonicalManifest;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::SaveArchiveContainerReadFailure;
using bloom::project::SaveArchiveExpectedContent;
using bloom::project::SaveArchiveFailure;
using bloom::project::SaveArchiveLimits;
using bloom::project::SaveArchiveStage;
using bloom::project::StagedSaveLeaseCall;
using bloom::project::StagedSavePlatformFailure;
using bloom::project::StagedSaveResult;
using bloom::project::StagedSaveStage;
using bloom::project::stageSaveArchive;
using bloom::project::verifySaveArchive;
using bloom::project::ZipContainerError;

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
        constexpr std::string_view prefix = "/tmp/bloom-staged-save-XXXXXX";
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

[[nodiscard]] std::span<const std::byte> asBytes(const std::span<const char> bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
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

[[nodiscard]] std::optional<StagedArtifactCoordinator>
makeCoordinator(Expectations& expectations, const StagedArtifactConfig config = {}) {
    auto result = StagedArtifactCoordinator::create(config);
    expectations.expect(result.succeeded(), "the staged-save coordinator is created");
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeCoordinator();
}

[[nodiscard]] StagedArtifactPreflightRequest makeRequest(
    std::filesystem::path targetPath,
    const ArtifactOverwritePolicy overwritePolicy = ArtifactOverwritePolicy::CreateOrReplace,
    std::optional<bloom::platform::ArtifactTargetObservation> expectedTarget = std::nullopt) {
    return {.targetPath = std::move(targetPath),
            .overwritePolicy = overwritePolicy,
            .expectedTarget = expectedTarget};
}

// Builds one save/reopen chain input (a fresh minimal document under `projectName`, mirroring
// src/project/tests/save_archive_tests.cpp's testMinimalGreenChain fixture) and invokes `use`
// with it. Everything the resulting CanonicalManifestV1/CanonicalDocumentV1 point into (the
// Snapshot, the ColorSettings) stays alive for the duration of `use` as locals of this function,
// never returned by value -- see that file's own comments on why a Document need not itself
// outlive snapshot() (Snapshot is backed by shared_ptr'd immutable state) but the manifest/
// document *input* structs, which hold raw pointers, do need their pointees to stay put.
void withDocumentInput(
    Expectations& expectations, const std::string_view projectName,
    const std::function<void(const CanonicalManifestV1&, const CanonicalDocumentV1&)>& use) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject(std::string(projectName), "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    const auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};
    use(manifest, documentInput);
}

// A larger fixture (few extension records, but each carrying a sizeable, poorly-compressible
// opaque payload, all owned by one provider so a single manifest requirement covers them) needed
// for budget-exhaustion probing. For a small document, the container writer's own documented
// worst-case qualified-dependency working-memory reservation (a transient, held only within its
// own stage) dwarfs the actual content, so a budget large enough to clear it during build always
// has slack left over for the tiny read-back allocation too -- the ReadBackAllocation stage is not
// reachable as a *specific* failure point for a small fixture (see the implementor's report). This
// fixture instead makes the built archive itself several times larger than that transient
// reservation, so a budget just above what build needs is provably short of what an additional,
// archive-sized read-back allocation needs.
void withBulkDocumentInput(
    Expectations& expectations,
    const std::function<void(const CanonicalManifestV1&, const CanonicalDocumentV1&)>& use) {
    using bloom::document::ExtensionRecord;
    using bloom::document::ExtensionRecordId;
    using bloom::document::NoExtensionReferences;
    using bloom::document::OpaqueExtensionPayload;

    const auto duration = bloom::core::RationalTime::create(240, 24);
    expectations.expect(duration.has_value(), "bulk fixture duration constructs");
    if (!duration.has_value()) {
        return;
    }
    auto newProject =
        bloom::document::makeNewProject("Bulk Budget Project", "Main Composition", *duration);
    constexpr std::uint64_t kRecordCount = 60;
    constexpr std::size_t kPayloadBytes = 200'000;
    // A cheap xorshift-derived pattern, not a real PRNG: just needs to resist deflate collapsing
    // the archive back down to a small compressed size the way a repeated-byte payload would.
    std::uint64_t xorshiftState = 0x9E3779B97F4A7C15ULL;
    const auto nextPseudoRandomByte = [&xorshiftState]() noexcept {
        xorshiftState ^= xorshiftState << 13U;
        xorshiftState ^= xorshiftState >> 7U;
        xorshiftState ^= xorshiftState << 17U;
        return static_cast<std::byte>(xorshiftState & 0xFFU);
    };
    for (std::uint64_t index = 1; index <= kRecordCount; ++index) {
        std::vector<std::byte> payloadBytes(kPayloadBytes);
        std::ranges::generate(payloadBytes, nextPseudoRandomByte);
        ExtensionRecord record{ExtensionRecordId::fromRaw(index),
                               "vendor.bulk",
                               "vendor.bulk.record",
                               {1, 0},
                               std::nullopt,
                               "application/octet-stream",
                               NoExtensionReferences{},
                               OpaqueExtensionPayload{std::move(payloadBytes)}};
        const bool added = newProject.project.addExtensionRecord(std::move(record));
        expectations.expect(added, "bulk fixture extension record adds");
        if (!added) {
            return;
        }
    }
    bloom::document::Document document{std::move(newProject.project)};
    const auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();
    const std::vector<bloom::project::ManifestRequirement> requirements{
        {.providerId = "vendor.bulk",
         .capabilityId = "vendor.bulk.cap",
         .schemaVersion = {1, 0},
         .providedNodeTypeIds = {}}};
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0},
                                       .requirements = requirements};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};
    use(manifest, documentInput);
}

struct StageAttempt final {
    std::optional<StagedArtifactLease> lease;
    std::optional<StagedSaveResult> result;
};

// preflight() -> stage() -> stageSaveArchive() against a real coordinator; leaves the lease (if
// one was obtained) in `lease` for the caller to publish, replay, or simply drop for RAII cleanup.
[[nodiscard]] StageAttempt
attemptStage(Expectations& expectations, StagedArtifactCoordinator& coordinator,
             const std::filesystem::path& targetPath, const ArtifactOverwritePolicy policy,
             const std::optional<bloom::platform::ArtifactTargetObservation>& expectedTarget,
             const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput,
             const SaveArchiveLimits& limits, ProjectIoOperationMemory operation) {
    StageAttempt attempt;
    auto preflight = coordinator.preflight(makeRequest(targetPath, policy, expectedTarget));
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
    attempt.result =
        stageSaveArchive(*attempt.lease, manifest, documentInput, limits, std::move(operation));
    return attempt;
}

// Independently encodes manifest.json/document.json straight from canonical_manifest.hpp /
// canonical_document.hpp -- never through save_archive.hpp/staged_save.hpp -- to serve as an
// oracle for the byte content stageSaveArchive() should have staged, mirroring
// save_archive_tests.cpp's testMinimalGreenChain oracle.
struct OracleEntries final {
    std::vector<char> manifestBytes;
    std::vector<char> documentBytes;
};

[[nodiscard]] std::optional<OracleEntries>
buildOracleEntries(Expectations& expectations, const CanonicalManifestV1& manifest,
                   const CanonicalDocumentV1& documentInput) {
    const auto manifestSize = canonicalManifestSize(manifest);
    expectations.expect(static_cast<bool>(manifestSize), "oracle: manifest sizes");
    if (!manifestSize) {
        return std::nullopt;
    }
    std::vector<char> manifestBytes(*manifestSize.value());
    expectations.expect(static_cast<bool>(encodeCanonicalManifest(manifest, manifestBytes)),
                        "oracle: manifest encodes");

    std::vector<char> payloadScratch(64, '\0');
    std::vector<std::size_t> sortScratch(64, 0);
    CanonicalDocumentV1 oracleRequest = documentInput;
    oracleRequest.payloadScratch = payloadScratch;
    oracleRequest.sortScratch = sortScratch;
    const auto documentSize = canonicalDocumentSize(oracleRequest);
    expectations.expect(static_cast<bool>(documentSize), "oracle: document sizes");
    if (!documentSize) {
        return std::nullopt;
    }
    std::vector<char> documentBytes(*documentSize.value());
    expectations.expect(static_cast<bool>(encodeCanonicalDocument(oracleRequest, documentBytes)),
                        "oracle: document encodes");
    return OracleEntries{std::move(manifestBytes), std::move(documentBytes)};
}

// ---------------------------------------------------------------------------------------------
// Green path: preflight (CreateOnly, absent target) -> stage -> stageSaveArchive -> success ->
// caller-side publish(Proceed) -> Published. The published file is byte-compared against an
// independently built archive (buildSaveArchive over the same input; determinism -- see
// testDeterminism below and save_archive_tests.cpp's own determinism test -- makes this a valid
// oracle) and, separately, re-verified in place with verifySaveArchive() against independently
// canonical-encoded entry bytes as a second, deeper oracle.
// ---------------------------------------------------------------------------------------------

void testGreenPathEndToEnd(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "green path: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Green Path Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            {
                // Scoped so this peek-only preflight token (and the active-target admission it
                // holds) is released before attemptStage() below preflights the same identity
                // again for real.
                auto preflight = coordinator->preflight(
                    makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly));
                expectations.expect(preflight && !preflight.target()->observation().exists,
                                    "green path: create-only preflight observes an absent target");
                if (!preflight) {
                    return;
                }
            }
            auto attempt = attemptStage(expectations, *coordinator, targetPath,
                                        ArtifactOverwritePolicy::CreateOnly, std::nullopt, manifest,
                                        documentInput, SaveArchiveLimits{}, makeOperation());
            expectations.expect(attempt.lease.has_value() && attempt.result.has_value() &&
                                    static_cast<bool>(*attempt.result),
                                "green path: stageSaveArchive succeeds");
            if (!attempt.lease.has_value() || !attempt.result.has_value() || !*attempt.result) {
                return;
            }
            const auto publication = attempt.lease->publish(PublicationDisposition::Proceed);
            expectations.expect(publication.outcome == StagedArtifactPublicationOutcome::Published,
                                "green path: publish reaches Published");
            if (publication.outcome != StagedArtifactPublicationOutcome::Published) {
                return;
            }

            auto oracleArchive =
                buildSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
            expectations.expect(static_cast<bool>(oracleArchive),
                                "green path: independent buildSaveArchive oracle succeeds");
            if (!oracleArchive) {
                return;
            }
            const auto oracleBytes = oracleArchive.archive()->bytes();
            const auto published = readFile(targetPath);
            expectations.expect(
                published.size() == oracleBytes.size() &&
                    std::memcmp(published.data(), oracleBytes.data(), published.size()) == 0,
                "green path: the published file is byte-identical to the independently built "
                "archive");

            const auto oracleEntries = buildOracleEntries(expectations, manifest, documentInput);
            if (!oracleEntries.has_value()) {
                return;
            }
            const SaveArchiveExpectedContent expected{
                .manifestBytes = asBytes(oracleEntries->manifestBytes),
                .documentBytes = asBytes(oracleEntries->documentBytes),
                .documentSchemaVersion = manifest.documentSchemaVersion,
            };
            auto reverified = verifySaveArchive(asBytes(published), expected, SaveArchiveLimits{},
                                                makeOperation());
            expectations.expect(static_cast<bool>(reverified),
                                "green path: verifySaveArchive independently re-verifies the "
                                "published file's bytes as the final oracle");
        });
}

// ---------------------------------------------------------------------------------------------
// ReplaceExisting: publish once, preflight again with the observed fingerprint, save+publish a
// second (distinct) archive, verify the replacement bytes.
// ---------------------------------------------------------------------------------------------

void testReplaceExistingPath(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "replace existing: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Replace First Project",
        [&](const CanonicalManifestV1& manifest1, const CanonicalDocumentV1& documentInput1) {
            auto first = attemptStage(expectations, *coordinator, targetPath,
                                      ArtifactOverwritePolicy::CreateOnly, std::nullopt, manifest1,
                                      documentInput1, SaveArchiveLimits{}, makeOperation());
            expectations.expect(first.lease.has_value() && first.result.has_value() &&
                                    static_cast<bool>(*first.result),
                                "replace existing: first stageSaveArchive succeeds");
            if (!first.lease.has_value() || !first.result.has_value() || !*first.result) {
                return;
            }
            const auto firstPublication = first.lease->publish(PublicationDisposition::Proceed);
            expectations.expect(firstPublication.outcome ==
                                    StagedArtifactPublicationOutcome::Published,
                                "replace existing: first publish reaches Published");
            if (firstPublication.outcome != StagedArtifactPublicationOutcome::Published) {
                return;
            }

            auto observedPreflight = coordinator->preflight(
                makeRequest(targetPath, ArtifactOverwritePolicy::ReplaceExisting));
            expectations.expect(observedPreflight &&
                                    observedPreflight.target()->observation().exists,
                                "replace existing: preflight observes the published target's "
                                "fingerprint");
            if (!observedPreflight) {
                return;
            }
            const auto observedFingerprint = observedPreflight.target()->observation();

            withDocumentInput(
                expectations, "Replace Second Project",
                [&](const CanonicalManifestV1& manifest2,
                    const CanonicalDocumentV1& documentInput2) {
                    auto second = attemptStage(expectations, *coordinator, targetPath,
                                               ArtifactOverwritePolicy::ReplaceExisting,
                                               observedFingerprint, manifest2, documentInput2,
                                               SaveArchiveLimits{}, makeOperation());
                    expectations.expect(second.lease.has_value() && second.result.has_value() &&
                                            static_cast<bool>(*second.result),
                                        "replace existing: second stageSaveArchive succeeds");
                    if (!second.lease.has_value() || !second.result.has_value() ||
                        !*second.result) {
                        return;
                    }
                    const auto secondPublication =
                        second.lease->publish(PublicationDisposition::Proceed);
                    expectations.expect(secondPublication.outcome ==
                                            StagedArtifactPublicationOutcome::Published,
                                        "replace existing: second publish reaches Published");
                    if (secondPublication.outcome != StagedArtifactPublicationOutcome::Published) {
                        return;
                    }

                    auto oracleArchive = buildSaveArchive(manifest2, documentInput2,
                                                          SaveArchiveLimits{}, makeOperation());
                    expectations.expect(static_cast<bool>(oracleArchive),
                                        "replace existing: independent oracle build succeeds");
                    if (!oracleArchive) {
                        return;
                    }
                    const auto oracleBytes = oracleArchive.archive()->bytes();
                    const auto published = readFile(targetPath);
                    expectations.expect(
                        published.size() == oracleBytes.size() &&
                            std::memcmp(published.data(), oracleBytes.data(), published.size()) ==
                                0,
                        "replace existing: the replaced file's bytes match the independently "
                        "built second archive");
                });
        });
}

// ---------------------------------------------------------------------------------------------
// Fault injection at StageWrite, StageWriterClose (finishWriting), StageVerificationRead, and
// StageVerificationAccept: each surfaces the platform's typed FaultInjected error at the right
// stage/lease-call of stageSaveArchive()'s result, and every lease -- once dropped -- releases its
// active-target admission (RAII cleanup; no test seam or explicit cleanup call needed).
// ---------------------------------------------------------------------------------------------

void testFaultInjection(Expectations& expectations) {
    struct FaultCase final {
        StagedArtifactFaultPoint point;
        StagedSaveStage expectedStage;
        StagedSaveLeaseCall expectedCall;
    };
    const std::array<FaultCase, 4> cases{{
        {StagedArtifactFaultPoint::StageWrite, StagedSaveStage::StageWrite,
         StagedSaveLeaseCall::Write},
        {StagedArtifactFaultPoint::StageWriterClose, StagedSaveStage::StageFinish,
         StagedSaveLeaseCall::FinishWriting},
        {StagedArtifactFaultPoint::StageVerificationRead, StagedSaveStage::StageRead,
         StagedSaveLeaseCall::ReadForVerification},
        {StagedArtifactFaultPoint::StageVerificationAccept, StagedSaveStage::Accept,
         StagedSaveLeaseCall::AcceptVerification},
    }};

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
        const auto targetPath = directory.path() / "project.bloom";

        withDocumentInput(
            expectations, "Fault Injection Project",
            [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
                auto attempt = attemptStage(
                    expectations, *coordinator, targetPath, ArtifactOverwritePolicy::CreateOnly,
                    std::nullopt, manifest, documentInput, SaveArchiveLimits{}, makeOperation());
                expectations.expect(attempt.lease.has_value(), "fault injection: a lease is "
                                                               "obtained before the injected "
                                                               "failure");
                expectations.expect(attempt.result.has_value() && !*attempt.result,
                                    "fault injection: stageSaveArchive reports failure");
                if (!attempt.result.has_value()) {
                    return;
                }
                const auto* failure = attempt.result->failure();
                expectations.expect(failure != nullptr &&
                                        failure->stage() == testCase.expectedStage,
                                    "fault injection: the failure stage matches the injected "
                                    "fault point");
                const auto* platformFailure =
                    failure != nullptr ? failure->payloadAs<StagedSavePlatformFailure>() : nullptr;
                expectations.expect(
                    platformFailure != nullptr &&
                        platformFailure->error == StagedArtifactError::FaultInjected &&
                        platformFailure->call == testCase.expectedCall,
                    "fault injection: the platform failure payload names FaultInjected and the "
                    "exact lease call");
            });
        // `attempt` (and its lease) is out of scope by now; the lease destructor already ran.
        const auto snapshot = coordinator->snapshot();
        expectations.expect(snapshot.activeTargetCount == 0,
                            "fault injection: dropping the lease releases the active target "
                            "admission");
    }
}

// ---------------------------------------------------------------------------------------------
// Verification failure over the staged bytes. Directly tampering the staged file's content
// through the lease API alone is not reachable: stageSaveArchive() runs write -> finish -> read
// -> verify as one monolithic noexcept call with no callback seam between finishWriting() and its
// own read-back loop, and adding one purely to make this test possible is explicitly out of scope
// (the task package forbids a production test seam for this). Reaching the platform's staged
// bytes from *outside* that call is therefore not possible without a seam either.
//
// Instead, this is reached through ordinary public parameters: a manifest whose
// documentSchemaVersion disagrees with the document's own embedded schema version (which
// CanonicalDocumentV1::schemaMinor controls, defaulted to 0 here) is a legitimate, seam-free way
// to make verifySaveArchive() fail its VersionAgreement check over the exact bytes that were
// staged and read back -- the encode step happily writes self-inconsistent input; only
// verification catches it. This is a genuine failure of stageSaveArchive()'s verification-over-
// staged-bytes step, not a redundant re-test of verifySaveArchive() in isolation (which
// save_archive_tests.cpp's testVersionDisagreement already covers) -- it additionally proves this
// module's own reject-on-failure wiring: lease.rejectVerification() is called and the lease
// replays a terminal StageVerificationRejected outcome afterward.
// ---------------------------------------------------------------------------------------------

void testVerificationFailureOverStagedBytes(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "verification failure: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Version Mismatch Project",
        [&](const CanonicalManifestV1& baseManifest, const CanonicalDocumentV1& documentInput) {
            CanonicalManifestV1 manifest = baseManifest;
            manifest.documentSchemaVersion = {1, 1}; // documentInput still encodes {1, 0}.

            auto preflight = coordinator->preflight(
                makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly));
            expectations.expect(static_cast<bool>(preflight), "verification failure: preflight "
                                                              "succeeds");
            if (!preflight) {
                return;
            }
            auto stageResult = coordinator->stage(std::move(preflight).takeTarget());
            expectations.expect(static_cast<bool>(stageResult), "verification failure: stage "
                                                                "succeeds");
            if (!stageResult) {
                return;
            }
            auto lease = std::move(stageResult).takeLease();

            auto result = stageSaveArchive(lease, manifest, documentInput, SaveArchiveLimits{},
                                           makeOperation());
            expectations.expect(!result, "verification failure: stageSaveArchive reports failure");
            const auto* failure = result.failure();
            expectations.expect(failure != nullptr &&
                                    failure->stage() == StagedSaveStage::Verification,
                                "verification failure: reported at the Verification stage");
            const auto* saveFailure =
                failure != nullptr ? failure->payloadAs<SaveArchiveFailure>() : nullptr;
            expectations.expect(saveFailure != nullptr &&
                                    saveFailure->stage() == SaveArchiveStage::VersionAgreement,
                                "verification failure: the nested SaveArchiveFailure names "
                                "VersionAgreement");
            expectations.expect(!result.rejectDiagnostic().has_value(),
                                "verification failure: lease.rejectVerification() itself "
                                "succeeded (no secondary diagnostic)");

            const auto publication = lease.publish(PublicationDisposition::Proceed);
            expectations.expect(
                publication.outcome == StagedArtifactPublicationOutcome::FailedBeforePublication &&
                    publication.error == StagedArtifactError::StageVerificationRejected,
                "verification failure: the lease replays its rejected terminal state on publish");
        });
}

// ---------------------------------------------------------------------------------------------
// Staged-size disagreement (StagedSaveStage::StagedSizeDisagreement): stageSaveArchive() issues
// exactly one lease.write(archiveBytes) call with the complete built archive and then reads
// lease.stageBytes() straight back from the platform lease, so the built size and the staged size
// can only disagree if the platform itself misreports what it accepted. None of the shipped
// StagedArtifactFaultPoint values corrupt stageBytes() (StageWrite/StageWriterClose faults return
// a typed error instead and stageSaveArchive never reaches the size check on that path), and
// there is no other public seam to desynchronize the two values. This path is therefore not
// reachable from this test file without a production test seam, which the task package forbids
// adding solely for this; it is documented here rather than covered by an automated test.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// Budget exhaustion. A budget that covers the build but is exhausted by the next materially
// larger allocation in the chain is not reachable as specifically "covers the build, fails at
// ReadBackAllocation": see the long comment inside this test for why (short version:
// writeZipContainer()'s own transient working-memory footprint already exceeds
// steady-state-after-build plus one more archive-sized buffer, for any fixture size, so a budget
// that clears the build always has slack left for the read-back allocation too). This test
// instead pins the same typed-ResourceExhausted/zeroed-snapshot/safe-destruction contract at the
// nearest stage where it IS reachable: Verification.
// ---------------------------------------------------------------------------------------------

void testBudgetExhaustion(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makeCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "budget exhaustion: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withBulkDocumentInput(expectations, [&](const CanonicalManifestV1& manifest,
                                            const CanonicalDocumentV1& documentInput) {
        // First pass: measure buildSaveArchive()'s own peak charge (manifest + document +
        // archive bytes, all live at once) with a generous budget. The bulk fixture's payload
        // is large enough that the ordinary 32 MiB "generous" budget used elsewhere in this
        // file is not generous enough here.
        constexpr std::uint64_t kBulkProbeBudget = 256ULL << 20U;
        auto probeMemory = ProjectIoMemoryCoordinator::create(kBulkProbeBudget);
        expectations.expect(probeMemory.has_value(), "budget exhaustion: probe coordinator "
                                                     "constructs");
        if (!probeMemory.has_value()) {
            return;
        }
        auto probeOperation = probeMemory->createOperation(kBulkProbeBudget, kBulkProbeBudget);
        expectations.expect(probeOperation.has_value(), "budget exhaustion: probe operation "
                                                        "constructs");
        if (!probeOperation.has_value()) {
            return;
        }
        std::uint64_t buildPeakBytes = 0;
        {
            auto probeBuild =
                buildSaveArchive(manifest, documentInput, SaveArchiveLimits{}, *probeOperation);
            expectations.expect(static_cast<bool>(probeBuild), "budget exhaustion: probe "
                                                               "build succeeds");
            if (!probeBuild) {
                return;
            }
            buildPeakBytes = probeOperation->snapshot().peakBytes;
        }

        // Second pass: a budget just above that build-only peak. Empirically (see the
        // implementor's report), stageSaveArchive()'s ReadBackAllocation stage specifically is
        // NOT reachable this way for this writer implementation: writeZipContainer() itself
        // transiently holds a full document-entry-sized deflate-attempt buffer alongside the
        // still-live manifest/document input bytes (plus its own documented qualified-
        // compressor working-memory reservation), so its own peak already exceeds
        // (steady-state-after-build + one more archive-sized read-back buffer) by roughly a
        // constant multiple, for any fixture size -- a budget that clears the build always has
        // slack left over for the tiny read-back allocation on top. The next stage that
        // legitimately needs materially more memory than the build did is Verification
        // (verifySaveArchive() reopens, parses, decodes, reconstructs, and re-encodes), so a
        // budget just above the build's own peak reliably exhausts there instead. This still
        // exercises the exact contract this test cares about -- typed ResourceExhausted,
        // zeroed snapshot, a safely destructible lease -- just one stage later than
        // ReadBackAllocation specifically.
        constexpr std::uint64_t kMargin = 64;
        const auto budget = buildPeakBytes + kMargin;
        auto memory = ProjectIoMemoryCoordinator::create(kBulkProbeBudget);
        expectations.expect(memory.has_value(), "budget exhaustion: coordinator constructs");
        if (!memory.has_value()) {
            return;
        }
        auto operation = memory->createOperation(budget, budget);
        expectations.expect(operation.has_value(), "budget exhaustion: constrained operation "
                                                   "constructs");
        if (!operation.has_value()) {
            return;
        }

        auto preflight =
            coordinator->preflight(makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly));
        expectations.expect(static_cast<bool>(preflight), "budget exhaustion: preflight "
                                                          "succeeds");
        if (!preflight) {
            return;
        }
        auto stageResult = coordinator->stage(std::move(preflight).takeTarget());
        expectations.expect(static_cast<bool>(stageResult), "budget exhaustion: stage "
                                                            "succeeds");
        if (!stageResult) {
            return;
        }
        {
            auto lease = std::move(stageResult).takeLease();
            auto result = stageSaveArchive(lease, manifest, documentInput, SaveArchiveLimits{},
                                           std::move(*operation));
            expectations.expect(!result, "budget exhaustion: a budget just above the build's "
                                         "own peak still fails somewhere in the chain");
            const auto* failure = result.failure();
            expectations.expect(failure != nullptr &&
                                    failure->stage() == StagedSaveStage::Verification,
                                "budget exhaustion: reported at the Verification stage (the "
                                "next stage past the build with materially higher memory "
                                "needs -- see the comment above)");
            const auto* nestedFailure =
                failure != nullptr ? failure->payloadAs<SaveArchiveFailure>() : nullptr;
            expectations.expect(nestedFailure != nullptr &&
                                    nestedFailure->stage() == SaveArchiveStage::ContainerRead,
                                "budget exhaustion: the nested verifySaveArchive() failure is "
                                "scoped to ContainerRead (reopening the staged bytes needs the "
                                "reader's own qualified-decompressor working-memory "
                                "reservation)");
            const auto* containerReadFailure =
                nestedFailure != nullptr
                    ? nestedFailure->payloadAs<SaveArchiveContainerReadFailure>()
                    : nullptr;
            expectations.expect(
                containerReadFailure != nullptr &&
                    containerReadFailure->error == ZipContainerError::ResourceExhausted,
                "budget exhaustion: the nested failure is a typed ResourceExhausted from the "
                "container reader");
        } // The lease is destroyed here; its stage cleanup is exercised by this scope exit.
        const auto memorySnapshot = memory->snapshot();
        expectations.expect(memorySnapshot.currentBytes == 0,
                            "budget exhaustion: the constrained coordinator's charge returns "
                            "to zero");
        const auto artifactSnapshot = coordinator->snapshot();
        expectations.expect(artifactSnapshot.activeTargetCount == 0,
                            "budget exhaustion: the lease remained safely destructible after "
                            "resource exhaustion");
    });
}

// ---------------------------------------------------------------------------------------------
// Determinism: two staged saves of the same input, published to two distinct targets, produce
// byte-identical published files.
// ---------------------------------------------------------------------------------------------

void testDeterminism(Expectations& expectations) {
    TempDirectory directoryA;
    TempDirectory directoryB;
    auto coordinator = makeCoordinator(expectations);
    if (!directoryA.isValid() || !directoryB.isValid() || !coordinator.has_value()) {
        expectations.expect(false, "determinism: fixture is available");
        return;
    }
    const auto targetPathA = directoryA.path() / "project.bloom";
    const auto targetPathB = directoryB.path() / "project.bloom";

    withDocumentInput(
        expectations, "Determinism Project",
        [&](const CanonicalManifestV1& manifestA, const CanonicalDocumentV1& documentInputA) {
            auto attemptA = attemptStage(
                expectations, *coordinator, targetPathA, ArtifactOverwritePolicy::CreateOnly,
                std::nullopt, manifestA, documentInputA, SaveArchiveLimits{}, makeOperation());
            expectations.expect(attemptA.result.has_value() && static_cast<bool>(*attemptA.result),
                                "determinism: first stageSaveArchive succeeds");
            if (!attemptA.lease.has_value() || !attemptA.result.has_value() || !*attemptA.result) {
                return;
            }
            const auto publicationA = attemptA.lease->publish(PublicationDisposition::Proceed);
            expectations.expect(publicationA.outcome == StagedArtifactPublicationOutcome::Published,
                                "determinism: first publish reaches Published");
        });
    withDocumentInput(
        expectations, "Determinism Project",
        [&](const CanonicalManifestV1& manifestB, const CanonicalDocumentV1& documentInputB) {
            auto attemptB = attemptStage(
                expectations, *coordinator, targetPathB, ArtifactOverwritePolicy::CreateOnly,
                std::nullopt, manifestB, documentInputB, SaveArchiveLimits{}, makeOperation());
            expectations.expect(attemptB.result.has_value() && static_cast<bool>(*attemptB.result),
                                "determinism: second stageSaveArchive succeeds");
            if (!attemptB.lease.has_value() || !attemptB.result.has_value() || !*attemptB.result) {
                return;
            }
            const auto publicationB = attemptB.lease->publish(PublicationDisposition::Proceed);
            expectations.expect(publicationB.outcome == StagedArtifactPublicationOutcome::Published,
                                "determinism: second publish reaches Published");
        });

    const auto bytesA = readFile(targetPathA);
    const auto bytesB = readFile(targetPathB);
    expectations.expect(!bytesA.empty() && bytesA == bytesB,
                        "determinism: two staged saves of the same input produce byte-identical "
                        "published files");
}

} // namespace

int main() {
    Expectations expectations;
    testGreenPathEndToEnd(expectations);
    testReplaceExistingPath(expectations);
    testFaultInjection(expectations);
    testVerificationFailureOverStagedBytes(expectations);
    testBudgetExhaustion(expectations);
    testDeterminism(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
