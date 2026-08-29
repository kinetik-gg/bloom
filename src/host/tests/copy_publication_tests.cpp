#include <bloom/host/copy_publication.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

// Drives executeCopyPublication() -- the host-side composition of the application
// PublicationCoordinator with the platform StagedArtifactCoordinator over
// project::stageCopyArchive() -- against REAL coordinators on per-test temporary directories,
// following src/host/tests/save_publication_tests.cpp's identical pattern one operation over
// (TempDirectory RAII helper, a thin coordinator/request-building layer). Per the task package's
// test plan, this file covers: green publish, supersession via preAdmitted, external-modification
// stale-observation refusal, and leak-free snapshots after failures -- the same shape as
// save_publication_tests.cpp's own coverage, trimmed to what Save Copy's own contract adds nothing
// new to (fault injection and budget exhaustion are already covered at the project layer in
// staged_copy_tests.cpp, mirroring how save_publication_tests.cpp itself does not re-test every
// staged_save_tests.cpp fault case either -- only the ones this composition layer's own wiring
// could get wrong).

namespace {

using bloom::host::CopyPublicationFailure;
using bloom::host::CopyPublicationRequest;
using bloom::host::CopyPublicationStage;
using bloom::host::executeCopyPublication;
using bloom::host::PublicationAdmission;
using bloom::host::PublicationCoordinator;
using bloom::host::PublicationCoordinatorConfig;

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::ArtifactTargetObservation;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;
using bloom::platform::StagedArtifactError;
using bloom::platform::StagedArtifactPublicationOutcome;

using bloom::project::buildSaveArchive;
using bloom::project::CanonicalDocumentV1;
using bloom::project::CanonicalManifestV1;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::SaveArchiveLimits;

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
        constexpr std::string_view prefix = "/tmp/bloom-copy-publication-XXXXXX";
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

constexpr std::uint64_t kGenerousOperationBudget = 32ULL << 20U;

[[nodiscard]] ProjectIoOperationMemory makeOperation() {
    auto coordinator = ProjectIoMemoryCoordinator::create(kGenerousOperationBudget);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation =
        coordinator->createOperation(kGenerousOperationBudget, kGenerousOperationBudget);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
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

[[nodiscard]] std::vector<std::byte> buildSourceArchiveBytesOrAbort() {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject =
        bloom::document::makeNewProject("Copy Publication Source", "Main Composition", *duration);
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

[[nodiscard]] CopyPublicationRequest
makeRequest(const std::filesystem::path& targetPath, const ArtifactOverwritePolicy policy,
            const std::optional<ArtifactTargetObservation>& expectedTarget,
            const std::span<const std::byte> sourceBytes,
            PublicationAdmission* preAdmitted = nullptr) {
    return CopyPublicationRequest{.targetPath = targetPath,
                                  .overwritePolicy = policy,
                                  .expectedTarget = expectedTarget,
                                  .sourceBytes = sourceBytes,
                                  .limits = {},
                                  .preAdmitted = preAdmitted,
                                  .cancellationFlag = nullptr};
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
// Green path: absent target, CreateOnly -> Published, byte-identical to the source.
// ---------------------------------------------------------------------------------------------

void testGreenPath(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "green path: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();

    auto request =
        makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly, std::nullopt, sourceBytes);
    auto result = executeCopyPublication(*coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(result),
                        "green path: the executor reaches publish without a protocol failure");
    const auto* publication = result.publication();
    expectations.expect(publication != nullptr &&
                            publication->outcome == StagedArtifactPublicationOutcome::Published,
                        "green path: publish reaches Published");
    expectations.expect(result.intentId().has_value() && result.intentId()->value() == 1,
                        "green path: the winning intent is reported");
    if (publication == nullptr ||
        publication->outcome != StagedArtifactPublicationOutcome::Published) {
        return;
    }

    const auto published = readFile(targetPath);
    const std::string sourceText(reinterpret_cast<const char*>(sourceBytes.data()),
                                 sourceBytes.size());
    expectations.expect(published == sourceText,
                        "green path: the published file is byte-for-byte identical to the source");

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "green path");
}

// ---------------------------------------------------------------------------------------------
// Supersession: mirrors save_publication_tests.cpp's testSupersession exactly, one composition
// layer over -- a competing higher intent registers for the same target key between this
// executor's own (lower) pre-admission and its guard attempt.
// ---------------------------------------------------------------------------------------------

void testSupersession(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "supersession: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();

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
    }

    auto lowerAdmission = admitIntent(*coordinator, expectations,
                                      "supersession: the executor's own (lower) intent is "
                                      "admitted first");
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
    std::move(competitorRegistration).takeClaim().reset();

    const auto lowerIntentId = lowerAdmission->intentId();

    auto request = makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly, std::nullopt,
                               sourceBytes, &(*lowerAdmission));
    auto result = executeCopyPublication(*coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(result),
                        "supersession: the executor still reaches publish (Superseded is a "
                        "publish outcome, not a pipeline failure)");
    const auto* publication = result.publication();
    expectations.expect(publication != nullptr &&
                            publication->outcome == StagedArtifactPublicationOutcome::Superseded,
                        "supersession: the outcome is Superseded");
    expectations.expect(result.intentId().has_value() &&
                            result.intentId()->value() == lowerIntentId.value(),
                        "supersession: the reported intent is the executor's own (lower, losing) "
                        "intent");
    expectations.expect(!std::filesystem::exists(targetPath),
                        "supersession: the original (absent) target remains untouched");

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "supersession");
}

// ---------------------------------------------------------------------------------------------
// External modification: publish once, capture the fingerprint, modify the target externally, then
// run again with that now-stale expected observation -- the same typed Preflight-stage refusal
// save_publication_tests.cpp's testExternalModification exercises.
// ---------------------------------------------------------------------------------------------

void testExternalModification(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "external modification: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();

    auto request1 =
        makeRequest(targetPath, ArtifactOverwritePolicy::CreateOnly, std::nullopt, sourceBytes);
    auto result1 = executeCopyPublication(*coordinator, *artifacts, request1, makeOperation());
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
        auto peek =
            artifacts->preflight({.targetPath = targetPath,
                                  .overwritePolicy = ArtifactOverwritePolicy::ReplaceExisting,
                                  .expectedTarget = std::nullopt});
        expectations.expect(peek && peek.target()->observation().exists,
                            "external modification: the fingerprint is observed before the "
                            "external change");
        if (!peek) {
            return;
        }
        staleFingerprint = peek.target()->observation();
    }

    {
        std::ofstream mutate(targetPath, std::ios::binary | std::ios::app);
        expectations.expect(static_cast<bool>(mutate),
                            "external modification: the target opens for an external append");
        mutate << "external-modification-marker";
    }
    const auto tamperedBytes = readFile(targetPath);

    auto request2 = makeRequest(targetPath, ArtifactOverwritePolicy::ReplaceExisting,
                                staleFingerprint, sourceBytes);
    auto result2 = executeCopyPublication(*coordinator, *artifacts, request2, makeOperation());
    expectations.expect(!static_cast<bool>(result2),
                        "external modification: the executor reports a typed pipeline failure, "
                        "never reaching publish");
    const auto* failure = result2.failure();
    expectations.expect(failure != nullptr && failure->stage() == CopyPublicationStage::Preflight,
                        "external modification: reported at the Preflight stage");
    const auto* platformError =
        failure != nullptr ? failure->payloadAs<StagedArtifactError>() : nullptr;
    expectations.expect(platformError != nullptr &&
                            *platformError == StagedArtifactError::ExternalModificationConflict,
                        "external modification: the payload names ExternalModificationConflict");

    expectations.expect(readFile(targetPath) == tamperedBytes,
                        "external modification: the externally modified target is left untouched");

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "external modification");
}

// ---------------------------------------------------------------------------------------------
// Leak-free snapshots after a failure that never reaches publish(): an invalid target path is
// refused at Preflight, mirroring save_publication_tests.cpp's testInvalidTargetPath.
// ---------------------------------------------------------------------------------------------

void testLeakFreeSnapshotsAfterFailure(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "leak-free snapshots: fixture is available");
        return;
    }
    // A trailing separator makes filename() empty, which platform preflight rejects as
    // StagedArtifactError::InvalidTargetPath before any parent resolution or admission bookkeeping.
    const auto invalidTargetPath = std::filesystem::path(directory.path().string() + "/");
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();

    auto request = makeRequest(invalidTargetPath, ArtifactOverwritePolicy::CreateOnly, std::nullopt,
                               sourceBytes);
    auto result = executeCopyPublication(*coordinator, *artifacts, request, makeOperation());
    expectations.expect(!static_cast<bool>(result),
                        "leak-free snapshots: the executor reports a typed pipeline failure");
    const auto* failure = result.failure();
    expectations.expect(failure != nullptr && failure->stage() == CopyPublicationStage::Preflight,
                        "leak-free snapshots: reported at the Preflight stage");
    const auto* platformError =
        failure != nullptr ? failure->payloadAs<StagedArtifactError>() : nullptr;
    expectations.expect(platformError != nullptr &&
                            *platformError == StagedArtifactError::InvalidTargetPath,
                        "leak-free snapshots: the payload names InvalidTargetPath");

    expectIdleSnapshots(expectations, *coordinator, *artifacts, "leak-free snapshots");
}

} // namespace

int main() {
    Expectations expectations;
    testGreenPath(expectations);
    testSupersession(expectations);
    testExternalModification(expectations);
    testLeakFreeSnapshotsAfterFailure(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
