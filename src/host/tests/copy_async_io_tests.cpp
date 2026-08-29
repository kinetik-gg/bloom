#include <bloom/host/copy_async_io.hpp>

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
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <numeric>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Drives beginCopyPublication() (copy_async_io.cpp) against a REAL bloom::runtime::TaskScheduler
// with a real TaskExecutor::BlockingIo worker and REAL PublicationCoordinator/
// platform::StagedArtifactCoordinator instances -- following
// src/host/tests/session_async_io_tests.cpp's fixture idiom (TempDirectory, Expectations,
// BlockingIoGate/awaitQuiescence/pollUntilReady) one sibling module over. Per the task package's
// test plan: end-to-end with a real scheduler, cancellation before start, and abandonment.

namespace {

using namespace std::chrono_literals;

using bloom::host::AsyncCopyPublication;
using bloom::host::AsyncCopyPublicationRequest;
using bloom::host::beginCopyPublication;
using bloom::host::PublicationCoordinator;
using bloom::host::PublicationCoordinatorConfig;

using bloom::platform::ArtifactOverwritePolicy;
using bloom::platform::StagedArtifactConfig;
using bloom::platform::StagedArtifactCoordinator;
using bloom::platform::StagedArtifactPublicationOutcome;

using bloom::project::buildSaveArchive;
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
        constexpr std::string_view prefix = "/tmp/bloom-copy-async-io-XXXXXX";
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
        bloom::document::makeNewProject("Async Copy Source", "Main Composition", *duration);
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

// Mirrors session_async_io_tests.cpp's BlockingIoGate/occupyBlockingIoWorker exactly: saturates the
// scheduler's single BlockingIo worker so a subsequently-submitted async copy task provably has not
// started running yet.
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
// End-to-end: begin -> poll until ready -> tryComplete -> Published, byte-identical to the source.
// ---------------------------------------------------------------------------------------------

void testAsyncCopyEndToEnd(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "async copy end to end: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();
    TaskScheduler scheduler;

    AsyncCopyPublicationRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .bytes = sourceBytes,
        .limits = {},
    };
    auto begun =
        beginCopyPublication(scheduler, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(begun),
                        "async copy end to end: beginCopyPublication submits a handle");
    if (!begun) {
        return;
    }
    auto handle = std::move(begun).takeHandle();
    expectations.expect(pollUntilReady(handle),
                        "async copy end to end: isReady() becomes true without a blocking wait");

    auto result = handle.tryComplete();
    expectations.expect(result.has_value(),
                        "async copy end to end: tryComplete yields a result once ready");
    if (!result.has_value()) {
        return;
    }
    expectations.expect(static_cast<bool>(*result),
                        "async copy end to end: the executor reaches publish");
    const auto* publication = result->publication();
    expectations.expect(publication != nullptr &&
                            publication->outcome == StagedArtifactPublicationOutcome::Published,
                        "async copy end to end: publish reaches Published");

    const auto published = readFile(targetPath);
    const std::string sourceText(reinterpret_cast<const char*>(sourceBytes.data()),
                                 sourceBytes.size());
    expectations.expect(published == sourceText,
                        "async copy end to end: the published file is byte-for-byte identical to "
                        "the source bytes");

    // A second tryComplete() call returns nullopt (at-most-once contract).
    auto again = handle.tryComplete();
    expectations.expect(!again.has_value(),
                        "async copy end to end: tryComplete is idempotent-absent after the first "
                        "real result");
}

// ---------------------------------------------------------------------------------------------
// Cancellation before the worker starts: the single BlockingIo worker is saturated by a blocker
// task first, for determinism.
// ---------------------------------------------------------------------------------------------

void testCancellationBeforeWorkerStarts(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "cancellation: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();
    TaskScheduler scheduler;
    BlockingIoGate gate;
    occupyBlockingIoWorker(expectations, scheduler, gate);

    AsyncCopyPublicationRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .bytes = sourceBytes,
        .limits = {},
    };
    auto begun =
        beginCopyPublication(scheduler, *coordinator, *artifacts, request, makeOperation());
    expectations.expect(static_cast<bool>(begun),
                        "cancellation: beginCopyPublication submits a handle (still queued behind "
                        "the blocker)");
    if (!begun) {
        gate.release();
        return;
    }
    auto handle = std::move(begun).takeHandle();
    handle.requestCancellation();
    gate.release();

    expectations.expect(pollUntilReady(handle),
                        "cancellation: the cancelled task still reaches a terminal state");
    auto result = handle.tryComplete();
    expectations.expect(result.has_value(), "cancellation: tryComplete yields a result");
    if (result.has_value()) {
        expectations.expect(!static_cast<bool>(*result),
                            "cancellation: a cancellation requested before the worker starts never "
                            "publishes");
        const auto* failure = result->failure();
        const auto* publication = result->publication();
        const bool cancelledBeforePublication =
            (failure != nullptr) ||
            (publication != nullptr &&
             publication->outcome == StagedArtifactPublicationOutcome::CancelledBeforePublication);
        expectations.expect(
            cancelledBeforePublication,
            "cancellation: the result names a cancelled-before-publication outcome (a typed "
            "pipeline failure at the Cancelled stage, or the platform's own "
            "CancelledBeforePublication outcome)");
    }
    expectations.expect(!std::filesystem::exists(targetPath),
                        "cancellation: no target was ever created");
    expectations.expect(awaitQuiescence(scheduler),
                        "cancellation: the scheduler reaches quiescence");
}

// ---------------------------------------------------------------------------------------------
// Abandonment: destroy a handle mid-flight (still queued behind a saturated worker); no crash, no
// leaked coordinator state, scheduler drains cleanly.
// ---------------------------------------------------------------------------------------------

void testAbandonmentMidFlight(Expectations& expectations) {
    TempDirectory directory;
    auto coordinator = makePublicationCoordinator(expectations);
    auto artifacts = makeArtifactsCoordinator(expectations);
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.has_value()) {
        expectations.expect(false, "abandonment: fixture is available");
        return;
    }
    const auto targetPath = directory.path() / "copy.bloom";
    const auto sourceBytes = buildSourceArchiveBytesOrAbort();
    TaskScheduler scheduler;
    BlockingIoGate gate;
    occupyBlockingIoWorker(expectations, scheduler, gate);

    AsyncCopyPublicationRequest request{
        .targetPath = targetPath,
        .overwritePolicy = ArtifactOverwritePolicy::CreateOnly,
        .expectedTarget = std::nullopt,
        .bytes = sourceBytes,
        .limits = {},
    };
    {
        auto begun =
            beginCopyPublication(scheduler, *coordinator, *artifacts, request, makeOperation());
        expectations.expect(static_cast<bool>(begun),
                            "abandonment: beginCopyPublication submits a handle (still queued)");
        if (begun) {
            auto handle = std::move(begun).takeHandle();
            // Handle destroyed here, mid-flight, without ever calling tryComplete().
        }
    }
    gate.release();

    expectations.expect(awaitQuiescence(scheduler),
                        "abandonment: the scheduler drains cleanly (quiescence per its own API)");
    expectations.expect(!std::filesystem::exists(targetPath),
                        "abandonment: no target was ever created");

    const auto coordinatorSnapshot = coordinator->snapshot();
    expectations.expect(coordinatorSnapshot.unresolvedAdmissionCount == 0 &&
                            coordinatorSnapshot.activeTargetClaimCount == 0 &&
                            coordinatorSnapshot.activePublicationGuardCount == 0,
                        "abandonment: the publication coordinator has no leaked bookkeeping");
    const auto artifactSnapshot = artifacts->snapshot();
    expectations.expect(artifactSnapshot.activeTargetCount == 0,
                        "abandonment: the platform artifact coordinator has no active targets");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testAsyncCopyEndToEnd(expectations);
        testCancellationBeforeWorkerStarts(expectations);
        testAbandonmentMidFlight(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected fixture exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
