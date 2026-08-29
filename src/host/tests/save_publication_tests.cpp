#include <bloom/host/save_publication.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/staged_save.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

// Drives executeSavePublication() -- the host-side composition of the application
// PublicationCoordinator with the platform StagedArtifactCoordinator over
// project::stageSaveArchive() -- against REAL coordinators on per-test temporary directories,
// following src/project/tests/staged_save_tests.cpp's pattern (TempDirectory RAII helper, a thin
// coordinator/request-building layer, an independent buildSaveArchive()/verifySaveArchive()
// oracle for published bytes) one composition layer up.

namespace {

using bloom::host::executeSavePublication;
using bloom::host::PublicationAdmission;
using bloom::host::PublicationCoordinator;
using bloom::host::PublicationCoordinatorConfig;
using bloom::host::SavePublicationFailure;
using bloom::host::SavePublicationRequest;
using bloom::host::SavePublicationStage;

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::ArtifactTargetObservation;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;
using bloom::platform::StagedArtifactError;
using bloom::platform::StagedArtifactFaultPoint;
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
using bloom::project::StagedSaveFailure;
using bloom::project::StagedSaveLeaseCall;
using bloom::project::StagedSavePlatformFailure;
using bloom::project::StagedSaveStage;
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
        constexpr std::string_view prefix = "/tmp/bloom-save-publication-XXXXXX";
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

[[nodiscard]] std::optional<PublicationCoordinator>
makePublicationCoordinator(Expectations& expectations,
                           const PublicationCoordinatorConfig config = {}) {
    auto result = PublicationCoordinator::create(config);
    expectations.expect(result.has_value(), "the publication coordinator is created");
    return result;
}

[[nodiscard]] std::optional<StagedArtifactCoordinator>
makeArtifactsCoordinator(Expectations& expectations, const StagedArtifactConfig config = {}) {
    auto result = StagedArtifactCoordinator::create(config);
    expectations.expect(result.succeeded(), "the staged-artifact coordinator is created");
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeCoordinator();
}

[[nodiscard]] std::optional<PublicationAdmission> admitIntent(PublicationCoordinator& coordinator,
                                                              Expectations& expectations,
                                                              const std::string_view context) {
    auto result = coordinator.admit();
    expectations.expect(static_cast<bool>(result), context);
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeAdmission();
}

[[nodiscard]] SavePublicationRequest
makeRequest(const std::filesystem::path& targetPath, const ArtifactOverwritePolicy policy,
            const std::optional<ArtifactTargetObservation>& expectedTarget,
            const CanonicalManifestV1& manifest, const CanonicalDocumentV1& document,
            const SaveArchiveLimits& limits = {}, PublicationAdmission* preAdmitted = nullptr,
            const std::atomic_bool* cancellationFlag = nullptr) {
    return SavePublicationRequest{.targetPath = targetPath,
                                  .overwritePolicy = policy,
                                  .expectedTarget = expectedTarget,
                                  .manifest = &manifest,
                                  .document = &document,
                                  .limits = limits,
                                  .preAdmitted = preAdmitted,
                                  .cancellationFlag = cancellationFlag};
}

// Builds one save/reopen chain input (a fresh minimal document under `projectName`), mirroring
// src/project/tests/staged_save_tests.cpp's withDocumentInput fixture. Everything the resulting
// CanonicalManifestV1/CanonicalDocumentV1 point into stays alive for `use`'s duration as locals of
// this function (see that file's own comments on why this matters -- the input structs hold raw
// pointers, not owning storage).
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

// A larger fixture for budget-exhaustion probing, mirroring staged_save_tests.cpp's
// withBulkDocumentInput exactly (same record count/payload size: those numbers were derived there
// empirically so that a budget just above buildSaveArchive()'s own peak charge reliably exhausts
// at Verification/ContainerRead rather than anywhere else in the chain -- see that file's own long
// comment on why ReadBackAllocation specifically is not reachable this way).
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

// Independently encodes manifest.json/document.json straight from canonical_manifest.hpp/
// canonical_document.hpp -- never through save_archive.hpp/staged_save.hpp/save_publication.hpp
// -- to serve as an oracle for the byte content executeSavePublication() should have published.
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

void expectIdleSnapshots(Expectations& expectations, const PublicationCoordinator& coordinator,
                         const StagedArtifactCoordinator& artifacts,
                         const std::string_view context) {
    const auto coordinatorSnapshot = coordinator.snapshot();
    expectations.expect(coordinatorSnapshot.unresolvedAdmissionCount == 0 &&
                            coordinatorSnapshot.targetRecordCount == 0 &&
                            coordinatorSnapshot.activeTargetClaimCount == 0 &&
                            coordinatorSnapshot.activePublicationGuardCount == 0,
                        std::string(context) + ": the publication coordinator snapshot is idle");
    const auto artifactSnapshot = artifacts.snapshot();
    expectations.expect(artifactSnapshot.activeTargetCount == 0,
                        std::string(context) +
                            ": the platform artifact coordinator snapshot has no active targets");
}

// ---------------------------------------------------------------------------------------------
// Green path: absent target, CreateOnly -> Published. Published bytes are compared against an
// independently built archive (buildSaveArchive() over the same input) and separately
// re-verified in place with verifySaveArchive() against independently canonical-encoded entry
// bytes, mirroring staged_save_tests.cpp's testGreenPathEndToEnd two-oracle structure one layer up.
// ---------------------------------------------------------------------------------------------

void testGreenPath(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "green path: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Green Path Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            auto request = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly,
                                       std::nullopt, manifest, documentInput);
            auto result =
                executeSavePublication(*coordinator, *artifacts, request, makeOperation());
            expectations.expect(static_cast<bool>(result),
                                "green path: the executor reaches publish without a protocol "
                                "failure");
            const auto* publication = result.publication();
            expectations.expect(publication != nullptr &&
                                    publication->outcome ==
                                        StagedArtifactPublicationOutcome::Published,
                                "green path: publish reaches Published");
            expectations.expect(result.intentId().has_value() && result.intentId()->value() == 1,
                                "green path: the winning intent is reported");
            if (publication == nullptr ||
                publication->outcome != StagedArtifactPublicationOutcome::Published) {
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

            expectIdleSnapshots(expectations, *coordinator, *artifacts, "green path");
        });
}

// ---------------------------------------------------------------------------------------------
// ReplaceExisting: publish once, observe the published fingerprint, save+publish a second
// (distinct) archive with that fingerprint as the expected observation, verify the replacement.
// ---------------------------------------------------------------------------------------------

void testReplaceExistingPath(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "replace existing: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Replace First Project",
        [&](const CanonicalManifestV1& manifest1, const CanonicalDocumentV1& documentInput1) {
            auto request1 = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly,
                                        std::nullopt, manifest1, documentInput1);
            auto result1 =
                executeSavePublication(*coordinator, *artifacts, request1, makeOperation());
            expectations.expect(static_cast<bool>(result1) && result1.publication() != nullptr &&
                                    result1.publication()->outcome ==
                                        StagedArtifactPublicationOutcome::Published,
                                "replace existing: first publish reaches Published");
            if (!result1 || result1.publication() == nullptr ||
                result1.publication()->outcome != StagedArtifactPublicationOutcome::Published) {
                return;
            }

            std::optional<ArtifactTargetObservation> observedFingerprint;
            {
                // Scoped so this peek-only preflight token is released before request2's own
                // preflight (below) inspects the same identity again for real.
                auto peek = artifacts->preflight(
                    {.targetPath = targetPath,
                     .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting,
                     .expectedTarget = std::nullopt});
                expectations.expect(peek && peek.target()->observation().exists,
                                    "replace existing: preflight observes the published target's "
                                    "fingerprint");
                if (!peek) {
                    return;
                }
                observedFingerprint = peek.target()->observation();
            }

            withDocumentInput(
                expectations, "Replace Second Project",
                [&](const CanonicalManifestV1& manifest2,
                    const CanonicalDocumentV1& documentInput2) {
                    auto request2 =
                        makeRequest(targetPath, ArtifactOverwritePolicy::ReplaceExisting,
                                    observedFingerprint, manifest2, documentInput2);
                    auto result2 =
                        executeSavePublication(*coordinator, *artifacts, request2, makeOperation());
                    expectations.expect(static_cast<bool>(result2) &&
                                            result2.publication() != nullptr &&
                                            result2.publication()->outcome ==
                                                StagedArtifactPublicationOutcome::Published,
                                        "replace existing: second publish reaches Published");
                    if (!result2 || result2.publication() == nullptr ||
                        result2.publication()->outcome !=
                            StagedArtifactPublicationOutcome::Published) {
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

                    expectIdleSnapshots(expectations, *coordinator, *artifacts, "replace existing");
                });
        });
}

// ---------------------------------------------------------------------------------------------
// Supersession: a competing higher intent registers for the same target key between this
// executor's own (lower) admission and its guard attempt. The FROZEN arrangement from the task
// package: pre-admit and hand the executor its own (lower) admission via
// SavePublicationRequest::preAdmitted, then -- before invoking the executor -- admit+register a
// competing HIGHER intent directly against the coordinator for the identical target key (obtained
// by a scoped peek preflight, released before the executor's own real preflight runs). The
// competing claim is then dropped immediately: PublicationTargetClaim::tryEnterPublication()'s
// Superseded test is purely "does the target record's highestRegisteredIntent still equal MY
// intent", which the coordinator's tombstone semantics keep true regardless of whether the
// competing claim object itself is still alive (see docs/architecture/project-session.md's
// "Per-Target Ordering And Publication": the record persists as a tombstone while any unresolved
// intent below the high-water mark could still arrive -- exactly this executor's lower admission).
// ---------------------------------------------------------------------------------------------

void testSupersession(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "supersession: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    bloom::host::ArtifactTargetKey targetKey;
    {
        auto peek = artifacts->preflight({.targetPath = targetPath,
                                          .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                          .expectedTarget = std::nullopt});
        expectations.expect(static_cast<bool>(peek), "supersession: the peek preflight succeeds");
        if (!peek) {
            return;
        }
        targetKey = peek.target()->targetKey();
    } // Released before the executor's own real preflight runs against the same identity.

    auto lowerAdmission = admitIntent(*coordinator, expectations,
                                      "supersession: the executor's own (lower) intent "
                                      "is admitted first");
    auto competitorAdmission = admitIntent(*coordinator, expectations,
                                           "supersession: the competing (higher) intent is "
                                           "admitted second");
    if (!lowerAdmission.has_value() || !competitorAdmission.has_value()) {
        return;
    }
    expectations.expect(lowerAdmission->intentId().value() <
                            competitorAdmission->intentId().value(),
                        "supersession: the competitor's intent is strictly higher");

    auto competitorRegistration =
        coordinator->registerTarget(std::move(*competitorAdmission), targetKey);
    expectations.expect(static_cast<bool>(competitorRegistration),
                        "supersession: the competitor registers the higher intent for the "
                        "identical target key");
    if (!competitorRegistration) {
        return;
    }
    // Dropped immediately: the target record retains highestRegisteredIntent as a tombstone (see
    // the comment above), and `lowerAdmission` is still unresolved below it, so pruning does not
    // remove the record.
    std::move(competitorRegistration).takeClaim().reset();

    // Captured before the call below: executeSavePublication() takes ownership of *preAdmitted by
    // moving from it, which resets the moved-from PublicationAdmission's own intentId() to
    // invalid (see PublicationAdmission's move constructor) -- reading it afterward would not
    // observe the intent this test is asserting about.
    const auto lowerIntentId = lowerAdmission->intentId();

    withDocumentInput(
        expectations, "Supersession Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            auto request =
                makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly, std::nullopt, manifest,
                            documentInput, SaveArchiveLimits{}, &(*lowerAdmission));
            auto result =
                executeSavePublication(*coordinator, *artifacts, request, makeOperation());
            expectations.expect(static_cast<bool>(result),
                                "supersession: the executor still reaches publish (Superseded is "
                                "a publish outcome, not a pipeline failure)");
            const auto* publication = result.publication();
            expectations.expect(publication != nullptr &&
                                    publication->outcome ==
                                        StagedArtifactPublicationOutcome::Superseded,
                                "supersession: the outcome is Superseded");
            expectations.expect(result.intentId().has_value() &&
                                    result.intentId()->value() == lowerIntentId.value(),
                                "supersession: the reported intent is the executor's own (lower, "
                                "losing) intent");
            expectations.expect(!std::filesystem::exists(targetPath),
                                "supersession: the original (absent) target remains untouched");
        });

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "supersession");
}

// ---------------------------------------------------------------------------------------------
// External modification: publish once, capture the published fingerprint, modify the target
// directly on disk, then run again with that now-stale expected observation. The caller-supplied
// expectedTarget is compared against a fresh inspection inside preflight() itself (see
// src/platform/tests/staged_artifact_tests.cpp's testTargetChecksAndExpectedState, the same
// public-API path this reaches), so this surfaces as a typed Preflight-stage failure carrying
// platform::StagedArtifactError::ExternalModificationConflict -- the identical error the
// project-format.md outcome table's ExternalModificationConflict row names, reached at the
// earliest point the public contract can report it for a caller-supplied stale expectation
// (see the implementor's report for why the OTHER reachable path -- publish()'s own final
// revalidation against what preflight itself just observed -- needs a genuine mid-call race and
// is not reachable through one synchronous executeSavePublication() call without a forbidden test
// seam).
// ---------------------------------------------------------------------------------------------

void testExternalModification(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "external modification: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "External Modification First",
        [&](const CanonicalManifestV1& manifest1, const CanonicalDocumentV1& documentInput1) {
            auto request1 = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly,
                                        std::nullopt, manifest1, documentInput1);
            auto result1 =
                executeSavePublication(*coordinator, *artifacts, request1, makeOperation());
            expectations.expect(static_cast<bool>(result1) && result1.publication() != nullptr &&
                                    result1.publication()->outcome ==
                                        StagedArtifactPublicationOutcome::Published,
                                "external modification: first publish reaches Published");
            if (!result1 || result1.publication() == nullptr ||
                result1.publication()->outcome != StagedArtifactPublicationOutcome::Published) {
                return;
            }

            std::optional<ArtifactTargetObservation> staleFingerprint;
            {
                auto peek = artifacts->preflight(
                    {.targetPath = targetPath,
                     .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting,
                     .expectedTarget = std::nullopt});
                expectations.expect(peek && peek.target()->observation().exists,
                                    "external modification: the fingerprint is observed before "
                                    "the external change");
                if (!peek) {
                    return;
                }
                staleFingerprint = peek.target()->observation();
            }

            {
                std::ofstream mutate(targetPath, std::ios::binary | std::ios::app);
                expectations.expect(static_cast<bool>(mutate),
                                    "external modification: the target opens for an external "
                                    "append");
                mutate << "external-modification-marker";
            }
            const auto tamperedBytes = readFile(targetPath);

            withDocumentInput(
                expectations, "External Modification Second",
                [&](const CanonicalManifestV1& manifest2,
                    const CanonicalDocumentV1& documentInput2) {
                    auto request2 =
                        makeRequest(targetPath, ArtifactOverwritePolicy::ReplaceExisting,
                                    staleFingerprint, manifest2, documentInput2);
                    auto result2 =
                        executeSavePublication(*coordinator, *artifacts, request2, makeOperation());
                    expectations.expect(!static_cast<bool>(result2),
                                        "external modification: the executor reports a typed "
                                        "pipeline failure, never reaching publish");
                    const auto* failure = result2.failure();
                    expectations.expect(failure != nullptr &&
                                            failure->stage() == SavePublicationStage::Preflight,
                                        "external modification: reported at the Preflight stage");
                    const auto* platformError =
                        failure != nullptr ? failure->payloadAs<StagedArtifactError>() : nullptr;
                    expectations.expect(platformError != nullptr &&
                                            *platformError ==
                                                StagedArtifactError::ExternalModificationConflict,
                                        "external modification: the payload names "
                                        "ExternalModificationConflict");

                    expectations.expect(readFile(targetPath) == tamperedBytes,
                                        "external modification: the externally modified target "
                                        "is left untouched");
                });

            expectIdleSnapshots(expectations, *coordinator, *artifacts, "external modification");
        });
}

// ---------------------------------------------------------------------------------------------
// Failure stages: invalid target path (Preflight), a fault injected at StageWrite (StagedSave,
// verbatim project::StagedSaveFailure payload), a fault at AtomicPublication (reaches publish;
// FailedBeforePublication + the platform error), and a fault at ParentDurability
// (PublishedWithDurabilityWarning -- a PUBLISHED outcome with a warning). After every case the
// coordinator and platform snapshots are asserted idle.
// ---------------------------------------------------------------------------------------------

void testInvalidTargetPath(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "invalid target path: fixture is available");
        return;
    }
    // A trailing separator makes filename() empty, which platform preflight rejects as
    // StagedArtifactError::InvalidTargetPath before any parent resolution or admission bookkeeping
    // (see src/platform/staged_artifact_linux.cpp's preflightChecked()).
    const auto invalidTargetPath = std::filesystem::path(directory.path().string() + "/");

    withDocumentInput(
        expectations, "Invalid Target Path Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            auto request = makeRequest(invalidTargetPath, ArtifactOverwritePolicy::CreateOnly,
                                       std::nullopt, manifest, documentInput);
            auto result =
                executeSavePublication(*coordinator, *artifacts, request, makeOperation());
            expectations.expect(!static_cast<bool>(result),
                                "invalid target path: the executor reports a typed pipeline "
                                "failure");
            const auto* failure = result.failure();
            expectations.expect(failure != nullptr &&
                                    failure->stage() == SavePublicationStage::Preflight,
                                "invalid target path: reported at the Preflight stage");
            const auto* platformError =
                failure != nullptr ? failure->payloadAs<StagedArtifactError>() : nullptr;
            expectations.expect(platformError != nullptr &&
                                    *platformError == StagedArtifactError::InvalidTargetPath,
                                "invalid target path: the payload names InvalidTargetPath");
        });

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "invalid target path");
}

void testStagedSaveFaultInjection(Expectations& expectations) {
    TempDirectory directory;
    StagedArtifactConfig config;
    config.faults = {.point = StagedArtifactFaultPoint::StageWrite, .occurrence = 1};
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations, config);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "staged-save fault: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Staged Save Fault Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            auto request = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly,
                                       std::nullopt, manifest, documentInput);
            auto result =
                executeSavePublication(*coordinator, *artifacts, request, makeOperation());
            expectations.expect(!static_cast<bool>(result),
                                "staged-save fault: the executor reports a typed pipeline "
                                "failure");
            const auto* failure = result.failure();
            expectations.expect(failure != nullptr &&
                                    failure->stage() == SavePublicationStage::StagedSave,
                                "staged-save fault: reported at the StagedSave stage");
            const auto* nested =
                failure != nullptr ? failure->payloadAs<StagedSaveFailure>() : nullptr;
            expectations.expect(nested != nullptr && nested->stage() == StagedSaveStage::StageWrite,
                                "staged-save fault: the nested project::StagedSaveFailure names "
                                "StageWrite verbatim");
            const auto* platformFailure =
                nested != nullptr ? nested->payloadAs<StagedSavePlatformFailure>() : nullptr;
            expectations.expect(
                platformFailure != nullptr &&
                    platformFailure->error == StagedArtifactError::FaultInjected &&
                    platformFailure->call == StagedSaveLeaseCall::Write,
                "staged-save fault: the platform failure payload names FaultInjected and the "
                "exact lease call");
        });

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "staged-save fault");
}

void testAtomicPublicationFault(Expectations& expectations) {
    TempDirectory directory;
    StagedArtifactConfig config;
    config.faults = {.point = StagedArtifactFaultPoint::AtomicPublication, .occurrence = 1};
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations, config);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "atomic publication fault: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Atomic Publication Fault Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            auto request = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly,
                                       std::nullopt, manifest, documentInput);
            auto result =
                executeSavePublication(*coordinator, *artifacts, request, makeOperation());
            expectations.expect(static_cast<bool>(result),
                                "atomic publication fault: the executor still reaches publish "
                                "(no earlier pipeline failure)");
            const auto* publication = result.publication();
            expectations.expect(
                publication != nullptr &&
                    publication->outcome ==
                        StagedArtifactPublicationOutcome::FailedBeforePublication &&
                    publication->error == StagedArtifactError::FaultInjected &&
                    !publication->targetWasPublished(),
                "atomic publication fault: FailedBeforePublication with the injected platform "
                "error");
            expectations.expect(!std::filesystem::exists(targetPath),
                                "atomic publication fault: the target was never created");
        });

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "atomic publication fault");
}

void testParentDurabilityWarning(Expectations& expectations) {
    TempDirectory directory;
    StagedArtifactConfig config;
    config.faults = {.point = StagedArtifactFaultPoint::ParentDurability, .occurrence = 1};
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations, config);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "parent durability warning: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withDocumentInput(
        expectations, "Parent Durability Warning Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            auto request = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly,
                                       std::nullopt, manifest, documentInput);
            auto result =
                executeSavePublication(*coordinator, *artifacts, request, makeOperation());
            expectations.expect(static_cast<bool>(result),
                                "parent durability warning: the executor reaches publish");
            const auto* publication = result.publication();
            expectations.expect(
                publication != nullptr &&
                    publication->outcome ==
                        StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning &&
                    publication->error == StagedArtifactError::FaultInjected &&
                    publication->targetWasPublished(),
                "parent durability warning: PublishedWithDurabilityWarning is a PUBLISHED "
                "outcome (targetWasPublished() is true) carrying the injected platform error");
            expectations.expect(std::filesystem::exists(targetPath),
                                "parent durability warning: the target was actually created");
        });

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "parent durability warning");
}

// ---------------------------------------------------------------------------------------------
// Cancellation: an already-set caller-checked flag is observed at the first checkpoint (right
// after admission, before any platform path work), reported as a typed Cancelled-stage failure
// that never calls publish(). This is the request's optional, trivially-composed
// SavePublicationRequest::cancellationFlag seam (see the header comment); the frozen test plan
// does not require this case, but the implementation carries the feature so it is exercised here.
// ---------------------------------------------------------------------------------------------

void testCancellationBeforePreflight(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "cancellation: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    std::atomic_bool cancelled{true};

    withDocumentInput(
        expectations, "Cancellation Project",
        [&](const CanonicalManifestV1& manifest, const CanonicalDocumentV1& documentInput) {
            auto request =
                makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly, std::nullopt, manifest,
                            documentInput, SaveArchiveLimits{}, nullptr, &cancelled);
            auto result =
                executeSavePublication(*coordinator, *artifacts, request, makeOperation());
            expectations.expect(!static_cast<bool>(result),
                                "cancellation: the executor reports a typed pipeline failure, "
                                "never reaching publish");
            const auto* failure = result.failure();
            expectations.expect(failure != nullptr &&
                                    failure->stage() == SavePublicationStage::Cancelled,
                                "cancellation: reported at the Cancelled stage");
            expectations.expect(!std::filesystem::exists(targetPath),
                                "cancellation: no target was ever created");
        });

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "cancellation");
}

// ---------------------------------------------------------------------------------------------
// Budget exhaustion, threaded through the StagedSave stage. Mirrors
// staged_save_tests.cpp::testBudgetExhaustion's two-pass technique (probe buildSaveArchive()'s
// own peak charge with a generous budget, then constrain the real operation to just above that
// peak) one composition layer up: see that file's own long comment for why the resulting failure
// lands at Verification/ContainerRead rather than ReadBackAllocation.
// ---------------------------------------------------------------------------------------------

void testBudgetExhaustion(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "budget exhaustion: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";

    withBulkDocumentInput(expectations, [&](const CanonicalManifestV1& manifest,
                                            const CanonicalDocumentV1& documentInput) {
        constexpr std::uint64_t kBulkProbeBudget = 256ULL << 20U;
        auto probeMemory = ProjectIoMemoryCoordinator::create(kBulkProbeBudget);
        expectations.expect(probeMemory.has_value(),
                            "budget exhaustion: probe coordinator constructs");
        if (!probeMemory.has_value()) {
            return;
        }
        auto probeOperation = probeMemory->createOperation(kBulkProbeBudget, kBulkProbeBudget);
        expectations.expect(probeOperation.has_value(),
                            "budget exhaustion: probe operation constructs");
        if (!probeOperation.has_value()) {
            return;
        }
        std::uint64_t buildPeakBytes = 0;
        {
            auto probeBuild =
                buildSaveArchive(manifest, documentInput, SaveArchiveLimits{}, *probeOperation);
            expectations.expect(static_cast<bool>(probeBuild),
                                "budget exhaustion: probe build succeeds");
            if (!probeBuild) {
                return;
            }
            buildPeakBytes = probeOperation->snapshot().peakBytes;
        }

        constexpr std::uint64_t kMargin = 64;
        const auto budget = buildPeakBytes + kMargin;
        auto memory = ProjectIoMemoryCoordinator::create(kBulkProbeBudget);
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

        auto request = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly, std::nullopt,
                                   manifest, documentInput);
        auto result =
            executeSavePublication(*coordinator, *artifacts, request, std::move(*operation));
        expectations.expect(!static_cast<bool>(result),
                            "budget exhaustion: a budget just above the build's own peak still "
                            "fails somewhere in the chain");
        const auto* failure = result.failure();
        expectations.expect(failure != nullptr &&
                                failure->stage() == SavePublicationStage::StagedSave,
                            "budget exhaustion: reported at the StagedSave stage");
        const auto* nested = failure != nullptr ? failure->payloadAs<StagedSaveFailure>() : nullptr;
        expectations.expect(nested != nullptr && nested->stage() == StagedSaveStage::Verification,
                            "budget exhaustion: the nested StagedSaveFailure is scoped to "
                            "Verification");
        const auto* saveFailure =
            nested != nullptr ? nested->payloadAs<SaveArchiveFailure>() : nullptr;
        expectations.expect(saveFailure != nullptr &&
                                saveFailure->stage() == SaveArchiveStage::ContainerRead,
                            "budget exhaustion: the doubly-nested SaveArchiveFailure is scoped "
                            "to ContainerRead");
        const auto* containerReadFailure =
            saveFailure != nullptr ? saveFailure->payloadAs<SaveArchiveContainerReadFailure>()
                                   : nullptr;
        expectations.expect(containerReadFailure != nullptr &&
                                containerReadFailure->error == ZipContainerError::ResourceExhausted,
                            "budget exhaustion: the innermost failure is a typed ResourceExhausted "
                            "from the container reader");

        const auto memorySnapshot = memory->snapshot();
        expectations.expect(memorySnapshot.currentBytes == 0,
                            "budget exhaustion: the constrained coordinator's charge returns to "
                            "zero");
    });

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "budget exhaustion");
}

// ---------------------------------------------------------------------------------------------
// Determinism: two full green executeSavePublication() runs to two different targets produce
// byte-identical published files.
// ---------------------------------------------------------------------------------------------

void testDeterminism(Expectations& expectations) {
    TempDirectory directoryA;
    TempDirectory directoryB;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directoryA.isValid() || !directoryB.isValid() || !coordinator.has_value() ||
        !artifacts.has_value()) {
        expectations.expect(false, "determinism: fixture is available");
        return;
    }
    const auto targetPathA = directoryA.path() / "project.bloom";
    const auto targetPathB = directoryB.path() / "project.bloom";

    withDocumentInput(
        expectations, "Determinism Project",
        [&](const CanonicalManifestV1& manifestA, const CanonicalDocumentV1& documentInputA) {
            auto requestA = makeRequest(targetPathA, ArtifactOverwritePolicy::CreateOnly,
                                        std::nullopt, manifestA, documentInputA);
            auto resultA =
                executeSavePublication(*coordinator, *artifacts, requestA, makeOperation());
            expectations.expect(static_cast<bool>(resultA) && resultA.publication() != nullptr &&
                                    resultA.publication()->outcome ==
                                        StagedArtifactPublicationOutcome::Published,
                                "determinism: first publish reaches Published");
        });
    withDocumentInput(
        expectations, "Determinism Project",
        [&](const CanonicalManifestV1& manifestB, const CanonicalDocumentV1& documentInputB) {
            auto requestB = makeRequest(targetPathB, ArtifactOverwritePolicy::CreateOnly,
                                        std::nullopt, manifestB, documentInputB);
            auto resultB =
                executeSavePublication(*coordinator, *artifacts, requestB, makeOperation());
            expectations.expect(static_cast<bool>(resultB) && resultB.publication() != nullptr &&
                                    resultB.publication()->outcome ==
                                        StagedArtifactPublicationOutcome::Published,
                                "determinism: second publish reaches Published");
        });

    const auto bytesA = readFile(targetPathA);
    const auto bytesB = readFile(targetPathB);
    expectations.expect(!bytesA.empty() && bytesA == bytesB,
                        "determinism: two executeSavePublication runs of the same input produce "
                        "byte-identical published files");

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "determinism");
}

} // namespace

int main() {
    Expectations expectations;
    testGreenPath(expectations);
    testReplaceExistingPath(expectations);
    testSupersession(expectations);
    testExternalModification(expectations);
    testInvalidTargetPath(expectations);
    testStagedSaveFaultInjection(expectations);
    testAtomicPublicationFault(expectations);
    testParentDurabilityWarning(expectations);
    testCancellationBeforePreflight(expectations);
    testBudgetExhaustion(expectations);
    testDeterminism(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
