#include <bloom/host/session_recovery.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/project_io_memory.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

// SAVEFIX-1's recovery half, driven against the same real coordinators and per-test temporary
// directories the session-save tests use one layer up. There is no dialog anywhere here: the
// startup offer is a pure function over a directory and an optional project path, so the decision
// the UI renders is the decision this test makes.

namespace {

using bloom::commands::SetProjectName;
using bloom::commands::Transaction;

using bloom::host::DecodedProjectEditability;
using bloom::host::findSessionRecoveryOffer;
using bloom::host::ProjectDisplayPath;
using bloom::host::ProjectSession;
using bloom::host::ProjectSessionIdentitySource;
using bloom::host::PublicationCoordinator;
using bloom::host::removeSessionRecoveryFile;
using bloom::host::saveProjectSession;
using bloom::host::sessionRecoveryFilePath;
using bloom::host::SessionRecoveryStatus;
using bloom::host::SessionSaveRequest;
using bloom::host::writeSessionRecoverySnapshot;

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::StagedArtifactCoordinator;

constexpr std::string_view kOwnerToken = "0123456789abcdef0123456789abcdef";

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
        constexpr std::string_view prefix = "/tmp/bloom-session-recovery-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;
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

[[nodiscard]] bloom::project::ProjectIoOperationMemory makeOperation() {
    constexpr std::uint64_t budget = 32ULL << 20U;
    auto coordinator = bloom::project::ProjectIoMemoryCoordinator::create(budget);
    if (!coordinator.has_value()) {
        throw std::logic_error("the recovery fixture needs an I/O memory coordinator");
    }
    auto operation = coordinator->createOperation(budget, budget);
    if (!operation.has_value()) {
        throw std::logic_error("the recovery fixture needs an I/O memory operation");
    }
    return std::move(*operation);
}

[[nodiscard]] bloom::document::ColorSettings neutralColorSettings() {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bloom::document::makeBloomNeutralColorSettingsV1(
        bloom::core::Sha256Digest::fromBytes(bytes));
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bloom::document::Revision currentRevision(const ProjectSession& session) {
    const auto state = session.stateSnapshot();
    if (!state.currentRevision.has_value()) {
        throw std::logic_error("the recovery fixture needs a decoded revision");
    }
    return *state.currentRevision;
}

[[nodiscard]] Transaction rename(std::string value, const ProjectSession& session) {
    Transaction transaction("Rename", currentRevision(session));
    transaction.emplace<SetProjectName>(std::move(value));
    return transaction;
}

[[nodiscard]] ProjectSession makeSession(Expectations& expectations,
                                         ProjectSessionIdentitySource& identitySource,
                                         const std::optional<std::filesystem::path>& displayPath) {
    auto created = bloom::document::makeNewProject("Recovery Project", "Main Composition",
                                                   bloom::core::RationalTime::fromInteger(10));
    std::optional<ProjectDisplayPath> path;
    if (displayPath.has_value()) {
        path = ProjectDisplayPath::create(*displayPath);
        expectations.expect(path.has_value(), "the display-path fixture is valid");
    }
    auto result = ProjectSession::createDecoded(identitySource,
                                                {.project = std::move(created.project),
                                                 .colorSettings = neutralColorSettings(),
                                                 .editability = DecodedProjectEditability::Editable,
                                                 .displayPath = std::move(path),
                                                 .persistedAllocatorHighWater = std::nullopt});
    expectations.expect(static_cast<bool>(result), "the recovery session fixture must be valid");
    if (!result) {
        throw std::logic_error("could not create the recovery session fixture");
    }
    return std::move(result).takeSession();
}

// A dirty session writes a recovery file that is a real, openable Bloom archive; the session
// itself is left exactly as dirty as it was, because recovery is not a save.
void testDirtySessionWritesAnOpenableRecoveryFile(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = PublicationCoordinator::create();
    auto artifacts = StagedArtifactCoordinator::create({});
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.succeeded()) {
        expectations.expect(false, "the recovery fixture is available");
        return;
    }
    auto artifactCoordinator = std::move(artifacts).takeCoordinator();
    auto session = makeSession(expectations, identitySource, std::nullopt);
    const auto recoveryPath = sessionRecoveryFilePath(directory.path(), kOwnerToken,
                                                      session.stateSnapshot().projectSessionId);

    const auto clean = writeSessionRecoverySnapshot(session, *coordinator, artifactCoordinator,
                                                    recoveryPath, {}, makeOperation());
    expectations.expect(clean.status() == SessionRecoveryStatus::NotDirty &&
                            !std::filesystem::exists(recoveryPath),
                        "a session with nothing unsaved writes no recovery file");

    expectations.expect(session.execute(rename("Edited", session)).changed(),
                        "the fixture edit commits and makes the session dirty");
    const auto dirtyRevision = currentRevision(session);

    const auto written = writeSessionRecoverySnapshot(session, *coordinator, artifactCoordinator,
                                                      recoveryPath, {}, makeOperation());
    expectations.expect(written.wrote() && std::filesystem::exists(recoveryPath) &&
                            std::filesystem::file_size(recoveryPath) > 0,
                        "a dirty session writes its recovery file");

    const auto state = session.stateSnapshot();
    expectations.expect(state.dirty.has_value() && *state.dirty &&
                            state.currentRevision == dirtyRevision &&
                            !state.displayPath.has_value(),
                        "writing a recovery file accepts no savepoint: the session is untouched");

    const auto bytes = readFile(recoveryPath);
    auto reopened = bloom::project::openProjectArchive(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()),
        {}, makeOperation());
    expectations.expect(reopened.outcome() == bloom::project::OpenArchiveOutcome::Opened,
                        "the recovery file is an ordinary Bloom archive that opens normally");
}

// A successful Save makes the recovery file redundant, and the next recovery tick retires it.
void testSuccessfulSaveRetiresTheRecoveryFile(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = PublicationCoordinator::create();
    auto artifacts = StagedArtifactCoordinator::create({});
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.succeeded()) {
        expectations.expect(false, "the recovery fixture is available");
        return;
    }
    auto artifactCoordinator = std::move(artifacts).takeCoordinator();
    auto session = makeSession(expectations, identitySource, std::nullopt);
    const auto recoveryPath = sessionRecoveryFilePath(directory.path(), kOwnerToken,
                                                      session.stateSnapshot().projectSessionId);
    const auto projectPath = directory.path() / "project.bloom";

    expectations.expect(session.execute(rename("Edited", session)).changed(), "the edit commits");
    expectations.expect(writeSessionRecoverySnapshot(session, *coordinator, artifactCoordinator,
                                                     recoveryPath, {}, makeOperation())
                            .wrote(),
                        "the dirty session has a recovery file to retire");
    auto ownershipPath = recoveryPath;
    ownershipPath += ".owner";
    expectations.expect(std::filesystem::exists(ownershipPath),
                        "the published recovery has an ownership record");

    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "Save As advances path authority");
    if (!saveAs) {
        return;
    }
    const SessionSaveRequest request{
        .targetPath = projectPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto saved =
        saveProjectSession(session, *coordinator, artifactCoordinator, request, makeOperation());
    const auto* publication = saved.publication();
    expectations.expect(publication != nullptr && publication->targetWasPublished(),
                        "the real save publishes");

    expectations.expect(removeSessionRecoveryFile(recoveryPath) &&
                            !std::filesystem::exists(recoveryPath) &&
                            !std::filesystem::exists(ownershipPath),
                        "a successful save removes the recovery file and ownership record");
    expectations.expect(removeSessionRecoveryFile(recoveryPath),
                        "removing an already-absent recovery file is not a failure");

    const auto afterSave = writeSessionRecoverySnapshot(session, *coordinator, artifactCoordinator,
                                                        recoveryPath, {}, makeOperation());
    expectations.expect(afterSave.status() == SessionRecoveryStatus::NotDirty &&
                            !std::filesystem::exists(recoveryPath),
                        "and the now-clean session writes no new one");
}

// The startup offer: what a launch would show, decided without any dialog.
void testStartupOfferOnlyNamesWorkTheProjectDoesNotAlreadyHold(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = PublicationCoordinator::create();
    auto artifacts = StagedArtifactCoordinator::create({});
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.succeeded()) {
        expectations.expect(false, "the recovery fixture is available");
        return;
    }
    auto artifactCoordinator = std::move(artifacts).takeCoordinator();

    expectations.expect(!findSessionRecoveryOffer(directory.path(), std::nullopt).has_value(),
                        "an empty recovery directory offers nothing");
    expectations.expect(
        !findSessionRecoveryOffer(directory.path() / "missing", std::nullopt).has_value(),
        "a recovery directory that does not exist offers nothing, and does not fail");

    auto session = makeSession(expectations, identitySource, std::nullopt);
    const auto recoveryPath = sessionRecoveryFilePath(directory.path(), kOwnerToken,
                                                      session.stateSnapshot().projectSessionId);
    expectations.expect(session.execute(rename("Edited", session)).changed(), "the edit commits");
    expectations.expect(writeSessionRecoverySnapshot(session, *coordinator, artifactCoordinator,
                                                     recoveryPath, {}, makeOperation())
                            .wrote(),
                        "the abnormal-exit fixture leaves a recovery file behind");

    const auto offer = findSessionRecoveryOffer(directory.path(), std::nullopt);
    expectations.expect(offer.has_value() && offer->recoveryPath == recoveryPath &&
                            !offer->projectPath.has_value(),
                        "a never-saved project's recovery file is offered on the next launch");

    // A project file written after the recovery file already holds that work.
    const auto projectPath = directory.path() / "project.bloom";
    {
        std::error_code code;
        std::filesystem::copy_file(recoveryPath, projectPath, code);
        expectations.expect(!code, "the fixture project file is written");
        std::filesystem::last_write_time(
            projectPath,
            std::filesystem::last_write_time(recoveryPath, code) + std::chrono::seconds{10}, code);
        expectations.expect(!code, "the fixture project file is newer than the recovery file");
    }
    const auto unrelatedOffer = findSessionRecoveryOffer(directory.path(), projectPath);
    expectations.expect(
        unrelatedOffer.has_value() && unrelatedOffer->recoveryPath == recoveryPath &&
            !unrelatedOffer->projectPath.has_value(),
        "an unrelated newer saved project cannot suppress or claim a pathless recovery");

    {
        std::error_code code;
        std::filesystem::last_write_time(
            recoveryPath,
            std::filesystem::last_write_time(projectPath, code) + std::chrono::seconds{10}, code);
        expectations.expect(!code, "the fixture recovery file is made the newer of the two");
    }
    const auto newerOffer = findSessionRecoveryOffer(directory.path(), projectPath);
    expectations.expect(newerOffer.has_value() && newerOffer->recoveryPath == recoveryPath &&
                            !newerOffer->projectPath.has_value(),
                        "a pathless recovery never inherits the caller's saved-project path");

    // The old process-local naming scheme remains discoverable after filenames gain a launch
    // owner token. It is never an automatic cleanup target of a new ProjectHost.
    const auto legacyPath = directory.path() / "recovery-1.bloom";
    std::error_code code;
    std::filesystem::rename(recoveryPath, legacyPath, code);
    expectations.expect(!code, "the old-format recovery file fixture is installed");
    const auto legacyOffer = findSessionRecoveryOffer(directory.path(), std::nullopt);
    expectations.expect(legacyOffer.has_value() && legacyOffer->recoveryPath == legacyPath,
                        "a recovery artifact named by the legacy runtime session ID is found");

    const auto tiedPath = directory.path() / "recovery-2.bloom";
    std::filesystem::copy_file(legacyPath, tiedPath, code);
    const auto tiedTime = std::filesystem::last_write_time(legacyPath, code);
    std::filesystem::last_write_time(tiedPath, tiedTime, code);
    expectations.expect(!code, "equal-time recovery fixtures are installed");
    const auto tiedOffer = findSessionRecoveryOffer(directory.path(), projectPath);
    expectations.expect(tiedOffer.has_value() && tiedOffer->recoveryPath == legacyPath &&
                            !tiedOffer->projectPath.has_value(),
                        "equal-time recovery selection is deterministic and has no inferred owner");
}

void testOfferUsesEachRecoveryOwnProject(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = PublicationCoordinator::create();
    auto artifacts = StagedArtifactCoordinator::create({});
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.succeeded()) {
        expectations.expect(false, "the two-project recovery fixture is available");
        return;
    }
    auto artifactCoordinator = std::move(artifacts).takeCoordinator();
    const auto projectA = directory.path() / "a.bloom";
    const auto projectB = directory.path() / "b.bloom";
    auto sessionA = makeSession(expectations, identitySource, projectA);
    auto sessionB = makeSession(expectations, identitySource, projectB);
    const auto recoveryA = sessionRecoveryFilePath(directory.path(), kOwnerToken,
                                                   sessionA.stateSnapshot().projectSessionId);
    const auto recoveryB = sessionRecoveryFilePath(directory.path(), kOwnerToken,
                                                   sessionB.stateSnapshot().projectSessionId);
    expectations.expect(sessionA.execute(rename("A edit", sessionA)).changed(), "A becomes dirty");
    expectations.expect(sessionB.execute(rename("B edit", sessionB)).changed(), "B becomes dirty");
    expectations.expect(writeSessionRecoverySnapshot(sessionA, *coordinator, artifactCoordinator,
                                                     recoveryA, {}, makeOperation())
                            .wrote(),
                        "A recovery is written");
    expectations.expect(writeSessionRecoverySnapshot(sessionB, *coordinator, artifactCoordinator,
                                                     recoveryB, {}, makeOperation())
                            .wrote(),
                        "B recovery is written");
    std::error_code code;
    const auto writtenA = std::filesystem::last_write_time(recoveryA, code);
    const auto writtenB = std::filesystem::last_write_time(recoveryB, code);
    expectations.expect(!code, "recovery timestamp is available");

    // B has a newer saved file. It must not suppress A's unsaved work, even when the caller
    // names B, and B itself must not be offered.
    std::filesystem::copy_file(recoveryB, projectB, code);
    std::filesystem::last_write_time(projectB, writtenB + std::chrono::seconds{10}, code);
    expectations.expect(!code, "newer saved B fixture is available");
    const auto offerA = findSessionRecoveryOffer(directory.path(), projectB);
    expectations.expect(offerA.has_value() && offerA->recoveryPath == recoveryA &&
                            offerA->projectPath == projectA,
                        "newer saved B filters only B, leaving A with its own path");

    // Equal timestamps count as already saved for that same project.
    std::filesystem::copy_file(recoveryA, projectA, code);
    std::filesystem::last_write_time(projectA, writtenA, code);
    expectations.expect(!code, "equal-time saved A fixture is available");
    expectations.expect(!findSessionRecoveryOffer(directory.path(), projectB).has_value(),
                        "equal-time A and newer B suppress their own recoveries");

    // A corrupt newer file cannot hide a valid older one.
    std::filesystem::last_write_time(projectA, writtenA - std::chrono::seconds{10}, code);
    const auto corrupt = directory.path() / "recovery-corrupt.bloom";
    {
        std::ofstream output(corrupt, std::ios::binary);
        output << "not a Bloom archive";
    }
    std::filesystem::last_write_time(corrupt,
                                     std::max(writtenA, writtenB) + std::chrono::seconds{20}, code);
    expectations.expect(!code, "corrupt newest recovery fixture is available");
    const auto fallback = findSessionRecoveryOffer(directory.path(), projectB);
    expectations.expect(fallback.has_value() && fallback->recoveryPath == recoveryA &&
                            fallback->projectPath == projectA,
                        "corrupt newest recovery is skipped and a valid older one is offered");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testDirtySessionWritesAnOpenableRecoveryFile(expectations);
        testSuccessfulSaveRetiresTheRecoveryFile(expectations);
        testStartupOfferOnlyNamesWorkTheProjectDoesNotAlreadyHold(expectations);
        testOfferUsesEachRecoveryOwnProject(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected fixture exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
