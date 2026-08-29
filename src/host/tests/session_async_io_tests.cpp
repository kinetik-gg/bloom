#include <bloom/host/session_async_io.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/session_open.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/manifest_requirements.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Drives beginSessionSave()/beginSessionOpen() (session_async_io.cpp, task A1, issue #68) against a
// REAL bloom::runtime::TaskScheduler with a real TaskExecutor::BlockingIo worker and REAL
// PublicationCoordinator/platform::StagedArtifactCoordinator instances -- following
// session_save_tests.cpp's/session_open_tests.cpp's fixture pattern one layer up, plus
// src/runtime/tests/task_scheduler_tests.cpp's Gate/awaitQuiescence patterns for driving the
// scheduler deterministically.

namespace {

using namespace std::chrono_literals;

using bloom::commands::SetProjectName;
using bloom::commands::Transaction;

using bloom::host::ArtifactTargetKey;
using bloom::host::AsyncSessionOpen;
using bloom::host::AsyncSessionOpenBeginStage;
using bloom::host::AsyncSessionSave;
using bloom::host::AsyncSessionSaveBeginStage;
using bloom::host::beginSessionOpen;
using bloom::host::beginSessionSave;
using bloom::host::DecodedProjectEditability;
using bloom::host::DecodedProjectSessionRequest;
using bloom::host::OpenIntentAdmissionStatus;
using bloom::host::openSessionArchive;
using bloom::host::ProjectDisplayPath;
using bloom::host::ProjectSession;
using bloom::host::ProjectSessionContentKind;
using bloom::host::ProjectSessionIdentitySource;
using bloom::host::ProjectSessionSavepointStatus;
using bloom::host::PublicationAdmission;
using bloom::host::PublicationCoordinator;
using bloom::host::PublicationCoordinatorConfig;
using bloom::host::saveProjectSession;
using bloom::host::SessionInstallStatus;
using bloom::host::SessionOpenInstallOutcome;
using bloom::host::SessionOpenNotOpenedReason;
using bloom::host::SessionOpenResult;
using bloom::host::SessionOpenStage;
using bloom::host::SessionPathIntentKind;
using bloom::host::SessionSaveCaptureOutcome;
using bloom::host::SessionSaveInputStatus;
using bloom::host::SessionSaveRequest;
using bloom::host::SessionSaveResult;
using bloom::host::SessionSaveStage;

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::ArtifactTargetObservation;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;
using bloom::platform::StagedArtifactPublicationOutcome;

using bloom::project::buildVerifiedSaveArchive;
using bloom::project::CanonicalDocumentV1;
using bloom::project::CanonicalManifestV1;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::SaveArchiveLimits;

using bloom::runtime::TaskContext;
using bloom::runtime::TaskExecutor;
using bloom::runtime::TaskOwner;
using bloom::runtime::TaskOwnerId;
using bloom::runtime::TaskOwnerKind;
using bloom::runtime::TaskPriority;
using bloom::runtime::TaskRequest;
using bloom::runtime::TaskResult;
using bloom::runtime::TaskScheduler;
using bloom::runtime::TaskSchedulerConfig;

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
        constexpr std::string_view prefix = "/tmp/bloom-session-async-io-XXXXXX";
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
// Shared plumbing (mirrors session_save_tests.cpp/session_open_tests.cpp)
// ---------------------------------------------------------------------------------------------

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

[[nodiscard]] bloom::document::Revision currentRevision(const ProjectSession& session) {
    const auto state = session.stateSnapshot();
    if (!state.currentRevision.has_value()) {
        throw std::logic_error("Expected a decoded revision");
    }
    return *state.currentRevision;
}

[[nodiscard]] Transaction rename(std::string value, const ProjectSession& session,
                                 std::string label = "Rename") {
    Transaction transaction(std::move(label), currentRevision(session));
    transaction.emplace<SetProjectName>(std::move(value));
    return transaction;
}

[[nodiscard]] std::optional<std::string> projectName(const ProjectSession& session) {
    const auto snapshot = session.decodedSnapshot();
    if (!snapshot) {
        return std::nullopt;
    }
    return std::string(snapshot.snapshot().project().name());
}

[[nodiscard]] ProjectSession
makeDecodedSession(Expectations& expectations, ProjectSessionIdentitySource& identitySource,
                   const std::string& projectName,
                   const std::optional<std::filesystem::path>& displayPath = std::nullopt) {
    auto created = bloom::document::makeNewProject(projectName, "Main Composition",
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
    expectations.expect(static_cast<bool>(result), "the decoded session fixture must be valid");
    if (!result) {
        throw std::logic_error("Could not create decoded project-session fixture");
    }
    return std::move(result).takeSession();
}

// A minimal, valid, unverified two-entry archive that always classifies Opened -- mirrors
// session_open_tests.cpp's buildMinimalArchiveBytesOrAbort() (per-test-file fixture duplication is
// this codebase's established precedent; see that file's own comment).
[[nodiscard]] std::vector<std::byte>
buildMinimalArchiveBytesOrAbort(const std::string& projectName = "Untitled Project") {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    if (!duration.has_value()) {
        std::abort();
    }
    auto newProject = bloom::document::makeNewProject(projectName, "Main Composition", *duration);
    bloom::document::Document document{std::move(newProject.project)};
    auto snapshot = document.snapshot();
    const auto colorSettings = neutralColorSettings();

    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 0}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &snapshot, .colorSettings = &colorSettings};
    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    if (!built) {
        std::abort();
    }
    const auto bytes = built.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

// A blocking gate task fixture (mirrors src/runtime/tests/task_scheduler_tests.cpp's Gate): used to
// saturate the scheduler's single BlockingIo worker so a subsequently-submitted async save/open
// task provably has not started running yet -- the deterministic way to test
// cancellation-before-the- worker-starts and destruction-while-still-queued (abandonment).
class BlockingIoGate final {
  public:
    void enterAndWait(TaskContext& context) {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_ || context.isCancellationRequested(); });
    }

    [[nodiscard]] bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 2s, [&] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

// Submits a task that occupies the scheduler's single BlockingIo worker until `gate.release()` is
// called. `scheduler.submit<void>` keeps `gate` alive by reference for the fixture's own scope.
void occupyBlockingIoWorker(Expectations& expectations, TaskScheduler& scheduler,
                            BlockingIoGate& gate) {
    auto submission = scheduler.submit<void>(
        TaskRequest("Blocking IO gate",
                    TaskOwner{.kind = TaskOwnerKind::Application, .id = TaskOwnerId::fromRaw(999)},
                    TaskPriority::Foreground, TaskExecutor::BlockingIo),
        [&gate](TaskContext& context) {
            gate.enterAndWait(context);
            return TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted() && gate.waitUntilEntered(),
                        "the blocking-I/O gate fixture occupies the single worker");
}

bool awaitQuiescence(const TaskScheduler& scheduler) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (scheduler.isQuiescent()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

template <typename Handle> [[nodiscard]] bool pollUntilReady(const Handle& handle) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (handle.isReady()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Async save end-to-end: begin -> poll until ready -> tryComplete -> Published + Accepted, session
// clean at the published revision.
// ---------------------------------------------------------------------------------------------

void testAsyncSaveEndToEnd(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "async save end to end: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    TaskScheduler scheduler;

    auto session = makeDecodedSession(expectations, identitySource, "Async Save End To End");
    expectations.expect(session.execute(rename("Renamed", session)).changed(),
                        "async save end to end: the edit commits");
    const auto editedRevision = currentRevision(session);
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "async save end to end: Save As advances");
    if (!saveAs) {
        return;
    }

    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto begun =
        beginSessionSave(session, scheduler, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(begun),
                        "async save end to end: beginSessionSave submits a handle");
    if (!begun) {
        return;
    }
    auto handle = std::move(begun).takeHandle();
    expectations.expect(pollUntilReady(handle),
                        "async save end to end: isReady() becomes true without a blocking wait");

    auto result = handle.tryComplete(session);
    expectations.expect(result.has_value(), "async save end to end: tryComplete yields a result "
                                            "once ready");
    if (!result.has_value()) {
        return;
    }
    expectations.expect(static_cast<bool>(*result),
                        "async save end to end: the result names Published + Accepted");
    expectations.expect(
        result->stage() == SessionSaveStage::Savepoint && result->publication() != nullptr &&
            result->publication()->outcome == StagedArtifactPublicationOutcome::Published &&
            result->savepointStatus() == ProjectSessionSavepointStatus::Accepted,
        "async save end to end: the result shape matches the synchronous flow's own contract");

    const auto state = session.stateSnapshot();
    expectations.expect(state.cleanRevision == editedRevision && state.dirty == false,
                        "async save end to end: session is clean at the published revision");

    // A second tryComplete() call returns nullopt (at-most-once contract).
    auto again = handle.tryComplete(session);
    expectations.expect(!again.has_value(),
                        "async save end to end: tryComplete is idempotent-absent after the first "
                        "real result");
}

// ---------------------------------------------------------------------------------------------
// Async open end-to-end: begin -> poll -> tryComplete -> Installed.
// ---------------------------------------------------------------------------------------------

void testAsyncOpenEndToEnd(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    TaskScheduler scheduler;
    auto session = makeDecodedSession(expectations, identitySource, "Async Open Host");
    auto bytes = buildMinimalArchiveBytesOrAbort("Async Opened Content");
    auto displayPath = ProjectDisplayPath::create("/tmp/async-opened.bloom");
    expectations.expect(displayPath.has_value(), "async open end to end: display path constructs");
    if (!displayPath.has_value()) {
        return;
    }

    auto begun = beginSessionOpen(session, scheduler, std::move(bytes), displayPath,
                                  SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(begun),
                        "async open end to end: beginSessionOpen submits a handle");
    if (!begun) {
        return;
    }
    auto handle = std::move(begun).takeHandle();
    expectations.expect(pollUntilReady(handle), "async open end to end: isReady() becomes true");

    auto result = handle.tryComplete(session);
    expectations.expect(result.has_value(), "async open end to end: tryComplete yields a result");
    if (!result.has_value()) {
        return;
    }
    expectations.expect(static_cast<bool>(*result) &&
                            result->stage() == SessionOpenStage::Installation,
                        "async open end to end: installation succeeds");
    const auto* outcome = result->installOutcome();
    const auto* status = outcome != nullptr ? std::get_if<SessionInstallStatus>(outcome) : nullptr;
    expectations.expect(status != nullptr && *status == SessionInstallStatus::Installed,
                        "async open end to end: the install outcome names Installed verbatim");

    const auto state = session.stateSnapshot();
    expectations.expect(state.displayPath == displayPath && state.dirty == false,
                        "async open end to end: the session installs the opened content cleanly");
}

// ---------------------------------------------------------------------------------------------
// Async open, round-tripped newer minor: a schema {1,1} fixture with an unknown root member opens
// through the async path exactly as the synchronous flow's
// testNewProjectFullCycleInstallsBloomNeutralColor / testRoundTrippedNewerMinorSurvivesFullCycle
// counterparts do.
// ---------------------------------------------------------------------------------------------

void testAsyncOpenRoundTrippedNewerMinor(Expectations& expectations) {
    using bloom::project::canonicalDocumentSize;
    using bloom::project::decodeDocumentEnvelope;
    using bloom::project::DocumentClassification;
    using bloom::project::DocumentDecodeOutcome;
    using bloom::project::DocumentDecodeResult;
    using bloom::project::encodeCanonicalDocument;
    using bloom::project::parseStrictJsonDom;
    using bloom::project::reconstructDocument;

    ProjectSessionIdentitySource identitySource;
    TaskScheduler scheduler;
    auto session = makeDecodedSession(expectations, identitySource, "Async Round-Trip Host");

    auto newProject = bloom::document::makeNewProject("Untitled Project", "Main Composition",
                                                      bloom::core::RationalTime::fromInteger(10));
    bloom::document::Document sourceDocument{std::move(newProject.project)};
    auto sourceSnapshot = sourceDocument.snapshot();
    const auto colorSettings = neutralColorSettings();

    std::vector<char> payloadScratch(64, '\0');
    std::vector<std::size_t> sortScratch(64, 0);
    const CanonicalDocumentV1 baselineRequest{.snapshot = &sourceSnapshot,
                                              .colorSettings = &colorSettings,
                                              .payloadScratch = payloadScratch,
                                              .sortScratch = sortScratch};
    const auto size = canonicalDocumentSize(baselineRequest);
    expectations.expect(static_cast<bool>(size), "async round trip: baseline document sizes");
    if (!size) {
        return;
    }
    std::string text(*size.value(), '\0');
    const auto written =
        encodeCanonicalDocument(baselineRequest, std::span<char>(text.data(), text.size()));
    expectations.expect(static_cast<bool>(written), "async round trip: baseline document encodes");
    if (!written) {
        return;
    }
    const std::string anchor = "\"minor\": 0\n  },\n  \"project\"";
    const auto anchorPos = text.find(anchor);
    expectations.expect(anchorPos != std::string::npos, "async round trip: anchor is located");
    if (anchorPos == std::string::npos) {
        return;
    }
    text.replace(anchorPos, std::string_view("\"minor\": 0").size(), "\"minor\": 1");
    if (text.size() < 2 || text.back() != '\n' || text[text.size() - 2] != '}') {
        expectations.expect(false, "async round trip: baseline ends with the root's closing brace");
        return;
    }
    text.resize(text.size() - 2);
    text += R"(,"zzzFutureField":42})";
    text += '\n';

    auto parsed = parseStrictJsonDom(asBytes(text), {}, makeOperation());
    expectations.expect(static_cast<bool>(parsed), "async round trip: spliced document parses");
    if (!parsed) {
        return;
    }
    DocumentDecodeResult decoded = decodeDocumentEnvelope(parsed.document()->root());
    expectations.expect(decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                            decoded.value() != nullptr && decoded.roundTrip() != nullptr,
                        "async round trip: spliced document decodes with round-trip state");
    if (decoded.value() == nullptr || decoded.roundTrip() == nullptr) {
        return;
    }
    auto reconstructed = reconstructDocument(*decoded.value());
    expectations.expect(static_cast<bool>(reconstructed),
                        "async round trip: document reconstructs");
    if (!reconstructed) {
        return;
    }
    auto reconstructedSnapshot = reconstructed.value()->document->snapshot();
    const CanonicalManifestV1 manifest{.documentSchemaVersion = {1, 1}, .requirements = {}};
    const CanonicalDocumentV1 documentInput{.snapshot = &reconstructedSnapshot,
                                            .colorSettings = &reconstructed.value()->colorSettings,
                                            .roundTrip = decoded.roundTrip(),
                                            .schemaMinor = 1};
    auto built =
        buildVerifiedSaveArchive(manifest, documentInput, SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(built), "async round trip: fixture archive builds");
    if (!built) {
        return;
    }
    const auto fixtureBytes = built.archive()->bytes();
    std::vector<std::byte> ownedBytes(fixtureBytes.begin(), fixtureBytes.end());

    auto displayPath = ProjectDisplayPath::create("/tmp/async-round-trip.bloom");
    expectations.expect(displayPath.has_value(), "async round trip: display path constructs");
    if (!displayPath.has_value()) {
        return;
    }
    auto begun = beginSessionOpen(session, scheduler, std::move(ownedBytes), displayPath,
                                  SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(begun), "async round trip: beginSessionOpen submits");
    if (!begun) {
        return;
    }
    auto handle = std::move(begun).takeHandle();
    expectations.expect(pollUntilReady(handle), "async round trip: isReady() becomes true");
    auto result = handle.tryComplete(session);
    expectations.expect(result.has_value() && static_cast<bool>(*result),
                        "async round trip: the round-tripped newer-minor fixture installs "
                        "asynchronously exactly as it does synchronously");
}

// ---------------------------------------------------------------------------------------------
// Sync/async equivalence: identical content saved once synchronously and once asynchronously
// produces byte-identical published files.
// ---------------------------------------------------------------------------------------------

void testSyncAsyncEquivalence(Expectations& expectations) {
    TempDirectory directoryA;
    TempDirectory directoryB;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directoryA.isValid() || !directoryB.isValid() || !coordinator.has_value() ||
        !artifacts.has_value()) {
        expectations.expect(false, "sync/async equivalence: fixture is available");
        return;
    }
    const auto targetPathSync = directoryA.path() / "project.bloom";
    const auto targetPathAsync = directoryB.path() / "project.bloom";
    TaskScheduler scheduler;

    auto sessionSync = makeDecodedSession(expectations, identitySource, "Equivalence Project");
    const auto saveAsSync = sessionSync.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAsSync),
                        "sync/async equivalence: sync Save As advances");
    if (saveAsSync) {
        const SessionSaveRequest requestSync{
            .targetPath = targetPathSync,
            .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
            .expectedTarget = std::nullopt,
            .limits = {},
            .intent = saveAsSync.capture(),
        };
        auto resultSync =
            saveProjectSession(sessionSync, *coordinator, *artifacts, requestSync, makeOperation());
        expectations.expect(static_cast<bool>(resultSync),
                            "sync/async equivalence: the synchronous save fully succeeds");
    }

    auto sessionAsync = makeDecodedSession(expectations, identitySource, "Equivalence Project");
    const auto saveAsAsync = sessionAsync.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAsAsync),
                        "sync/async equivalence: async Save As advances");
    if (saveAsAsync) {
        const SessionSaveRequest requestAsync{
            .targetPath = targetPathAsync,
            .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
            .expectedTarget = std::nullopt,
            .limits = {},
            .intent = saveAsAsync.capture(),
        };
        auto begun = beginSessionSave(sessionAsync, scheduler, *coordinator, *artifacts,
                                      requestAsync, makeOperation());
        expectations.expect(static_cast<bool>(begun),
                            "sync/async equivalence: beginSessionSave submits a handle");
        if (begun) {
            auto handle = std::move(begun).takeHandle();
            expectations.expect(pollUntilReady(handle),
                                "sync/async equivalence: the async handle becomes ready");
            auto resultAsync = handle.tryComplete(sessionAsync);
            expectations.expect(resultAsync.has_value() && static_cast<bool>(*resultAsync),
                                "sync/async equivalence: the asynchronous save fully succeeds");
        }
    }

    const auto bytesSync = readFile(targetPathSync);
    const auto bytesAsync = readFile(targetPathAsync);
    expectations.expect(!bytesSync.empty() && bytesSync == bytesAsync,
                        "sync/async equivalence: saveProjectSession() and begin/tryComplete "
                        "produce byte-identical published files for identical input");
}

// ---------------------------------------------------------------------------------------------
// beginSessionSave() consumes request.preAdmitted at begin (authoring-thread ownership transfer,
// not a caller-kept-alive borrow): mirrors session_save_tests.cpp's
// testSupersededLeavesSessionUntouchedThenAbandons, but drives the SAME supersession scenario
// through the async path with the caller's admission object going out of scope immediately after
// beginSessionSave() returns -- exactly the natural "admit, then let it go out of scope" caller
// pattern the fix targets. A higher-intent competitor registers for the same target first, so the
// worker's own (lower, pre-admitted) intent is Superseded; session untouched.
// ---------------------------------------------------------------------------------------------

void testAsyncPreAdmittedOwnershipSupersession(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "async preAdmitted ownership: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    TaskScheduler scheduler;

    auto session = makeDecodedSession(expectations, identitySource, "Async Supersession Project");
    expectations.expect(session.execute(rename("Edited", session)).changed(),
                        "async preAdmitted ownership: the edit commits");
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "async preAdmitted ownership: Save As advances");
    if (!saveAs) {
        return;
    }
    const auto beforeState = session.stateSnapshot();

    ArtifactTargetKey targetKey;
    {
        auto peek = artifacts->preflight({.targetPath = targetPath,
                                          .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
                                          .expectedTarget = std::nullopt});
        expectations.expect(static_cast<bool>(peek),
                            "async preAdmitted ownership: the peek preflight succeeds");
        if (!peek) {
            return;
        }
        targetKey = peek.target()->targetKey();
    }

    auto lowerAdmission =
        admitIntent(*coordinator, expectations,
                    "async preAdmitted ownership: the executor's own (lower) intent is admitted "
                    "first");
    auto competitorAdmission =
        admitIntent(*coordinator, expectations,
                    "async preAdmitted ownership: the competing (higher) intent is admitted "
                    "second");
    if (!lowerAdmission.has_value() || !competitorAdmission.has_value()) {
        return;
    }
    auto competitorRegistration =
        coordinator->registerTarget(std::move(*competitorAdmission), targetKey);
    expectations.expect(static_cast<bool>(competitorRegistration),
                        "async preAdmitted ownership: the competitor registers for the identical "
                        "target key");
    if (!competitorRegistration) {
        return;
    }
    std::move(competitorRegistration).takeClaim().reset();

    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
        .preAdmitted = &(*lowerAdmission),
    };
    auto begun =
        beginSessionSave(session, scheduler, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(begun),
                        "async preAdmitted ownership: beginSessionSave submits a handle");
    // The fix under test: beginSessionSave() already moved *lowerAdmission's state out by the time
    // it returns, on the authoring thread -- the caller's admission object is left valid-but-empty
    // (isValid() false) here, exactly as documented, and may now safely go out of scope (it does,
    // at the end of this function) without the worker (which may not even have started yet) ever
    // touching a dangling PublicationAdmission.
    expectations.expect(lowerAdmission.has_value() && !lowerAdmission->isValid(),
                        "async preAdmitted ownership: the caller's admission is moved-from "
                        "immediately when beginSessionSave() returns, not borrowed for the "
                        "worker's lifetime");
    if (!begun) {
        return;
    }
    auto handle = std::move(begun).takeHandle();

    expectations.expect(pollUntilReady(handle),
                        "async preAdmitted ownership: the worker still reaches a terminal state "
                        "using its OWN owned admission");
    auto result = handle.tryComplete(session);
    expectations.expect(result.has_value(),
                        "async preAdmitted ownership: tryComplete yields a result");
    if (!result.has_value()) {
        return;
    }
    expectations.expect(
        !static_cast<bool>(*result) && result->stage() == SessionSaveStage::Savepoint &&
            result->publication() != nullptr &&
            result->publication()->outcome == StagedArtifactPublicationOutcome::Superseded &&
            !result->savepointStatus().has_value(),
        "async preAdmitted ownership: Superseded triggers no acceptSavepoint attempt -- the "
        "worker's owned admission carried the same losing intent the caller's would have");

    const auto afterState = session.stateSnapshot();
    expectations.expect(afterState.dirty == true &&
                            afterState.cleanRevision == beforeState.cleanRevision &&
                            afterState.newestAcceptedPublicationIntent ==
                                beforeState.newestAcceptedPublicationIntent,
                        "async preAdmitted ownership: session state is untouched by the "
                        "non-published outcome");
}

// ---------------------------------------------------------------------------------------------
// Cancellation: requestCancellation() before the scheduler starts the task (the single BlockingIo
// worker is saturated by a blocker task first, for determinism) -> a cancelled-before-publication
// outcome, session untouched.
// ---------------------------------------------------------------------------------------------

void testCancellationBeforeWorkerStarts(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "cancellation: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    TaskScheduler scheduler;
    BlockingIoGate gate;
    occupyBlockingIoWorker(expectations, scheduler, gate);

    auto session = makeDecodedSession(expectations, identitySource, "Cancellation Project");
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "cancellation: Save As advances");
    if (!saveAs) {
        gate.release();
        return;
    }
    const auto beforeState = session.stateSnapshot();

    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto begun =
        beginSessionSave(session, scheduler, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(begun), "cancellation: beginSessionSave submits a handle "
                                                  "(still queued behind the blocker)");
    if (!begun) {
        gate.release();
        return;
    }
    auto handle = std::move(begun).takeHandle();
    handle.requestCancellation();
    gate.release();

    expectations.expect(pollUntilReady(handle), "cancellation: the cancelled task still reaches a "
                                                "terminal state");
    auto result = handle.tryComplete(session);
    expectations.expect(result.has_value(), "cancellation: tryComplete yields a result");
    if (result.has_value()) {
        expectations.expect(
            !static_cast<bool>(*result),
            "cancellation: a cancellation requested before the worker starts never publishes");
        const auto* publicationFailure = result->publicationFailure();
        const auto* publication = result->publication();
        const bool cancelledBeforePublication =
            (publicationFailure != nullptr) ||
            (publication != nullptr &&
             publication->outcome == StagedArtifactPublicationOutcome::CancelledBeforePublication);
        expectations.expect(
            cancelledBeforePublication,
            "cancellation: the result names a cancelled-before-publication outcome "
            "(a typed pipeline failure at the Cancelled stage, or the platform's own "
            "CancelledBeforePublication outcome)");
    }

    const auto afterState = session.stateSnapshot();
    expectations.expect(afterState.dirty == beforeState.dirty &&
                            afterState.cleanRevision == beforeState.cleanRevision &&
                            afterState.displayPath == beforeState.displayPath,
                        "cancellation: session state is untouched");
    expectations.expect(awaitQuiescence(scheduler), "cancellation: the scheduler reaches "
                                                    "quiescence");
}

// ---------------------------------------------------------------------------------------------
// Edit during async open: begin open, edit the current project while the worker runs, tryComplete
// -> RevisionChanged, current project intact.
// ---------------------------------------------------------------------------------------------

void testEditDuringAsyncOpen(Expectations& expectations) {
    ProjectSessionIdentitySource identitySource;
    TaskScheduler scheduler;
    auto session = makeDecodedSession(expectations, identitySource, "Edit During Async Open");
    auto bytes = buildMinimalArchiveBytesOrAbort("Should Not Install");
    auto displayPath = ProjectDisplayPath::create("/tmp/edit-during-open.bloom");
    expectations.expect(displayPath.has_value(), "edit during async open: display path constructs");
    if (!displayPath.has_value()) {
        return;
    }

    auto begun = beginSessionOpen(session, scheduler, std::move(bytes), displayPath,
                                  SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(begun), "edit during async open: beginSessionOpen "
                                                  "submits a handle");
    if (!begun) {
        return;
    }
    auto handle = std::move(begun).takeHandle();

    // The edit lands deterministically before tryComplete(): whether the worker has already
    // finished by this point does not matter for RevisionChanged -- the gate is the session's
    // tracked revision at INSTALL time, not at worker-completion time.
    expectations.expect(session.execute(rename("Edited While Opening", session)).changed(),
                        "edit during async open: the edit commits");
    const auto editedRevision = currentRevision(session);
    const auto editedName = projectName(session);

    expectations.expect(pollUntilReady(handle), "edit during async open: isReady() becomes true");
    auto result = handle.tryComplete(session);
    expectations.expect(result.has_value(), "edit during async open: tryComplete yields a result");
    if (!result.has_value()) {
        return;
    }
    expectations.expect(!static_cast<bool>(*result) &&
                            result->stage() == SessionOpenStage::Installation,
                        "edit during async open: installation is refused");
    const auto* outcome = result->installOutcome();
    const auto* status = outcome != nullptr ? std::get_if<SessionInstallStatus>(outcome) : nullptr;
    expectations.expect(status != nullptr && *status == SessionInstallStatus::RevisionChanged,
                        "edit during async open: the refusal names RevisionChanged");

    const auto afterState = session.stateSnapshot();
    expectations.expect(afterState.currentRevision == editedRevision,
                        "edit during async open: the current project (including the edit) is "
                        "intact");
    if (afterState.contentKind == ProjectSessionContentKind::DecodedDocument) {
        expectations.expect(projectName(session) == editedName,
                            "edit during async open: the edited content was never replaced");
    }
}

// ---------------------------------------------------------------------------------------------
// Replacement-survives-capture (the dangling-view regression test): begin a save, then (before
// tryComplete) install replacement content into the SAME session via the O2 path. The worker's
// captured view stays valid -- the save completes with its ORIGINAL content -- and its tryComplete
// savepoint is then refused by acceptSavepoint()'s own checks (StaleIntent, since installation
// advanced SessionResultAcceptanceGeneration/SessionPathIntentGeneration out from under the
// outstanding Save As capture). The session's NEW (replacement) content is untouched by the stale
// savepoint attempt.
// ---------------------------------------------------------------------------------------------

void testReplacementSurvivesCapture(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "replacement survives capture: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    TaskScheduler scheduler;

    auto session = makeDecodedSession(expectations, identitySource, "Original Content");
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs),
                        "replacement survives capture: Save As advances");
    if (!saveAs) {
        return;
    }

    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    auto begun =
        beginSessionSave(session, scheduler, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(begun),
                        "replacement survives capture: beginSessionSave submits a handle");
    if (!begun) {
        return;
    }
    auto handle = std::move(begun).takeHandle();

    // Before tryComplete(): install replacement content into the SAME session via the O2 Open
    // path. This advances SessionResultAcceptanceGeneration/SessionPathIntentGeneration,
    // invalidating the outstanding Save As capture the worker already snapshotted at begin() time.
    auto replacementBytes = buildMinimalArchiveBytesOrAbort("Replacement Content");
    auto replacementPath = ProjectDisplayPath::create("/tmp/replacement.bloom");
    expectations.expect(replacementPath.has_value(),
                        "replacement survives capture: replacement display path constructs");
    if (!replacementPath.has_value()) {
        return;
    }
    auto installed = openSessionArchive(session, replacementBytes, replacementPath,
                                        SaveArchiveLimits{}, makeOperation());
    expectations.expect(static_cast<bool>(installed),
                        "replacement survives capture: the O2 replacement installs");

    expectations.expect(pollUntilReady(handle),
                        "replacement survives capture: the save worker still reaches a terminal "
                        "state, unaffected by the session replacement");
    auto result = handle.tryComplete(session);
    expectations.expect(result.has_value(),
                        "replacement survives capture: tryComplete yields a result");
    if (!result.has_value()) {
        return;
    }
    expectations.expect(
        !static_cast<bool>(*result) && result->stage() == SessionSaveStage::Savepoint &&
            result->publication() != nullptr && result->publication()->targetWasPublished(),
        "replacement survives capture: the worker's captured (original) view still published a "
        "real file -- unaffected by the later replacement -- but the savepoint is refused");
    expectations.expect(result->savepointStatus() == ProjectSessionSavepointStatus::StaleIntent,
                        "replacement survives capture: the refusal is typed as StaleIntent");

    // The published file on disk names the ORIGINAL content, proving the worker's captured view
    // (SessionSaveOwningInput) stayed valid and was never affected by the replacement install.
    // Decoded (not a raw byte search): the archive is a ZIP container and its entries may be
    // compressed.
    const auto published = readFile(targetPath);
    auto reopened = bloom::project::openProjectArchive(asBytes(published), SaveArchiveLimits{},
                                                       makeOperation());
    expectations.expect(reopened.outcome() == bloom::project::OpenArchiveOutcome::Opened,
                        "replacement survives capture: the published file reopens");
    if (reopened.outcome() == bloom::project::OpenArchiveOutcome::Opened) {
        auto openedValue = std::move(reopened).takeOpened();
        expectations.expect(
            std::string(openedValue.document->snapshot().project().name()) == "Original Content",
            "replacement survives capture: the published bytes reflect the ORIGINAL captured "
            "content, not the replacement");
    }

    // The session's NEW (replacement) content is untouched by the stale savepoint attempt.
    const auto state = session.stateSnapshot();
    expectations.expect(state.displayPath == replacementPath,
                        "replacement survives capture: the session's replacement content (display "
                        "path) is untouched by the refused stale savepoint");
    expectations.expect(projectName(session) == std::optional<std::string>{"Replacement Content"},
                        "replacement survives capture: the session's replacement content (project "
                        "name) is untouched by the refused stale savepoint");
}

// ---------------------------------------------------------------------------------------------
// Abandonment: destroy a handle mid-flight (still queued behind a saturated worker); no crash, no
// session mutation, scheduler drains cleanly.
// ---------------------------------------------------------------------------------------------

void testAbandonmentMidFlight(Expectations& expectations) {
    TempDirectory directory;
    ProjectSessionIdentitySource identitySource;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "abandonment: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "project.bloom";
    TaskScheduler scheduler;
    BlockingIoGate gate;
    occupyBlockingIoWorker(expectations, scheduler, gate);

    auto session = makeDecodedSession(expectations, identitySource, "Abandonment Project");
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "abandonment: Save As advances");
    if (!saveAs) {
        gate.release();
        return;
    }
    const auto beforeState = session.stateSnapshot();

    const SessionSaveRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .limits = {},
        .intent = saveAs.capture(),
    };
    {
        auto begun = beginSessionSave(session, scheduler, *coordinator, *artifacts, request,
                                      makeOperation());
        expectations.expect(static_cast<bool>(begun),
                            "abandonment: beginSessionSave submits a handle (still queued)");
        if (begun) {
            auto handle = std::move(begun).takeHandle();
            // Handle destroyed here, mid-flight, without ever calling tryComplete().
        }
    }
    gate.release();

    expectations.expect(awaitQuiescence(scheduler), "abandonment: the scheduler drains cleanly "
                                                    "(quiescence per its own API)");
    const auto afterState = session.stateSnapshot();
    expectations.expect(afterState.dirty == beforeState.dirty &&
                            afterState.cleanRevision == beforeState.cleanRevision &&
                            afterState.displayPath == beforeState.displayPath &&
                            afterState.newestAcceptedPublicationIntent ==
                                beforeState.newestAcceptedPublicationIntent,
                        "abandonment: no crash, no session mutation");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testAsyncSaveEndToEnd(expectations);
        testAsyncOpenEndToEnd(expectations);
        testAsyncOpenRoundTrippedNewerMinor(expectations);
        testSyncAsyncEquivalence(expectations);
        testAsyncPreAdmittedOwnershipSupersession(expectations);
        testCancellationBeforeWorkerStarts(expectations);
        testEditDuringAsyncOpen(expectations);
        testReplacementSurvivesCapture(expectations);
        testAbandonmentMidFlight(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected fixture exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
