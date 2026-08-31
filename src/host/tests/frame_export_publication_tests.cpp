#include <bloom/host/frame_export_publication.hpp>

#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/output/flat_exr_reopen_verifier.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

// Task F2 (issue #101): drives approveFrameExportV1()/executeExportPublication() against a real
// platform::StagedArtifactCoordinator + PublicationCoordinator, mirroring bloom/host/tests/
// save_publication_tests.cpp / copy_publication_tests.cpp's own top-of-file rationale for doing so.
namespace {

namespace document = bloom::document;
namespace host = bloom::host;
namespace output = bloom::output;
namespace platform = bloom::platform;
namespace runtime = bloom::runtime;

using namespace std::chrono_literals;

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

// The one place this file dereferences a std::optional (or a std::optional-shaped result type)
// without a directly-adjacent has_value()/operator bool() check: it performs that check right
// here and returns a raw pointer, so every call site below works with a plain (possibly-null)
// pointer instead -- clang-tidy's bugprone-unchecked-optional-access only tracks std::optional
// itself, so converting to a pointer once, in one place, is the established way to avoid repeating
// an unprovable-at-a-distance check at dozens of call sites.
template <typename Optional>
[[nodiscard]] auto require(Optional& value, Expectations& expectations,
                           const std::string_view message) -> decltype(&*value) {
    expectations.expect(static_cast<bool>(value), message);
    if (!value) {
        return nullptr;
    }
    return &*value; // NOLINT(bugprone-unchecked-optional-access) -- guarded immediately above.
}

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-frame-export-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        const auto* result = ::mkdtemp(pattern.data());
        if (result != nullptr) {
            path_ = result;
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code errorCode;
            std::filesystem::remove_all(path_, errorCode);
        }
    }

    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

constexpr auto kProjectId = document::ProjectId::fromRaw(0x2001);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0x2002);
constexpr auto kSolidNodeId = document::NodeId::fromRaw(0x2003);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0x2004);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0x2005);
constexpr auto kRevision = document::Revision::fromRaw(0x2006);
constexpr auto kLayerNodeId = document::NodeId::fromRaw(0x2007);
constexpr auto kLayerId = document::LayerId::fromRaw(0x2008);
constexpr auto kStackNodeId = document::NodeId::fromRaw(0x2009);
constexpr auto kSlotId = document::LayerSlotId::fromRaw(0x200a);
constexpr auto kPositionParameterId = document::ParameterId::fromRaw(0x200b);
constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(0x200c);

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> smallSolidPlan() {
    const auto format = document::CompositionFormat::create(2, 2);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{kSolidNodeId, kColorParameterId,
                                                   bloom::core::Color4d{0.1, 0.2, 0.3, 1.0}});
    // CompiledCompositionOutput requires a layer-stack input, not a bare solid (mirrors
    // bloom/output/tests/flat_exr_test_support.hpp's shellPlan()): solid -> layer output -> layer
    // stack -> composition output.
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNodeId, kLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{kPositionParameterId, document::Vec2d{0.5, 0.5}},
        runtime::CompiledScalarParameter{kOpacityParameterId, 1.0}});
    operations.emplace_back(runtime::CompiledLayerStack{
        kStackNodeId, {{kSlotId, kLayerId, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNodeId, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{.sourceRevision = kRevision,
                                                   .projectId = kProjectId,
                                                   .compositionId = kCompositionId,
                                                   .format = *format,
                                                   .operations = std::move(operations),
                                                   .output = runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] host::OutputAnalysisAttemptRequestV1
attemptRequestFor(const std::filesystem::path& target) {
    const auto plan = smallSolidPlan();
    return {
        .plan = plan,
        .evaluation = {.time = bloom::core::RationalTime::fromInteger(0),
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = 4096},
        .targetPath = target,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)}};
}

// Owns a real TaskScheduler/StagedArtifactCoordinator/PublicationCoordinator/ExportResourceLedgerV1
// fixture set. TaskScheduler is non-movable (its move constructor/assignment are explicitly
// deleted) and StagedArtifactCoordinator/PublicationCoordinator are only constructible through
// their own ::create() factories, so every member is held in-place inside a std::optional and
// populated by setUp() -- the accessor methods below are the ONE place each member's optional is
// dereferenced (guarded by setUp()'s own already-checked success), rather than repeating an
// unprovable-at-a-distance dereference at every one of this file's many call sites.
class ExportFixture final {
  public:
    [[nodiscard]] bool setUp(Expectations& expectations, const std::string_view context) {
        auto artifactsResult = platform::StagedArtifactCoordinator::create({});
        auto coordinatorResult = host::PublicationCoordinator::create();
        const bool ok =
            directory_.isValid() && artifactsResult.succeeded() && coordinatorResult.has_value();
        expectations.expect(ok, context);
        if (!ok) {
            return false;
        }
        scheduler_.emplace();
        artifacts_.emplace(std::move(artifactsResult).takeCoordinator());
        coordinator_.emplace(
            std::move(*coordinatorResult)); // NOLINT(bugprone-unchecked-optional-access)
        ledger_.emplace();
        return true;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return directory_.path(); }
    [[nodiscard]] runtime::TaskScheduler& scheduler() noexcept {
        return *scheduler_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by setUp().
    }
    [[nodiscard]] platform::StagedArtifactCoordinator& artifacts() noexcept {
        return *artifacts_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by setUp().
    }
    [[nodiscard]] host::PublicationCoordinator& coordinator() noexcept {
        return *coordinator_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by
                              // setUp().
    }
    [[nodiscard]] output::ExportResourceLedgerV1& ledger() noexcept {
        return *ledger_; // NOLINT(bugprone-unchecked-optional-access) -- guaranteed by setUp().
    }

  private:
    TempDirectory directory_;
    std::optional<runtime::TaskScheduler> scheduler_;
    std::optional<platform::StagedArtifactCoordinator> artifacts_;
    std::optional<host::PublicationCoordinator> coordinator_;
    std::optional<output::ExportResourceLedgerV1> ledger_;
};

// Runs beginOutputAnalysisAttemptV1() to a completed, approvable attempt over `target`. Every test
// fixture below needs a real attempt (frame + identity + report + digest), not a hand-built one --
// approveFrameExportV1() cannot be exercised meaningfully without one.
[[nodiscard]] std::shared_ptr<const output::OutputAnalysisAttemptV1>
buildApprovableAttempt(Expectations& expectations, runtime::TaskScheduler& scheduler,
                       platform::StagedArtifactCoordinator& artifacts,
                       output::ExportResourceLedgerV1& ledger,
                       const std::filesystem::path& target) {
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, artifacts, ledger, attemptRequestFor(target));
    expectations.expect(static_cast<bool>(begin),
                        "attempt fixture: begin submits the Resolving task");
    if (!begin) {
        return nullptr;
    }
    auto runner = std::move(begin).takeHandle();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto outcome = runner.tryComplete();
        if (!outcome.has_value()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        const auto& outcomeValue = *outcome; // NOLINT(bugprone-unchecked-optional-access)
        expectations.expect(static_cast<bool>(outcomeValue),
                            "attempt fixture: the attempt completes");
        return static_cast<bool>(outcomeValue) ? outcomeValue.attempt() : nullptr;
    }
    expectations.expect(false, "attempt fixture: the attempt reaches a terminal outcome in time");
    return nullptr;
}

// Retrieves attempt->digest() with an immediately-adjacent has_value() check (attempt->digest()
// is a fresh temporary each call, so no earlier check anywhere else in the caller can cover it).
[[nodiscard]] bloom::core::Sha256Digest
requireDigest(const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
              Expectations& expectations) {
    const auto digest = attempt->digest();
    expectations.expect(digest.has_value(), "the attempt fixture is approvable with a digest");
    if (!digest.has_value()) {
        return {};
    }
    return *digest; // NOLINT(bugprone-unchecked-optional-access) -- guarded immediately above.
}

struct ExportRun final {
    runtime::TaskHandle<void> handle;
    std::shared_ptr<std::optional<host::FrameExportPublicationResultV1>> result;
};

[[nodiscard]] std::optional<ExportRun>
beginExportRun(runtime::TaskScheduler& scheduler, platform::StagedArtifactCoordinator& artifacts,
               std::unique_ptr<host::FrameExportRequestV1> request,
               const std::filesystem::path& scratchDir,
               const output::OutputExportClockV1& clock = {}) {
    auto shared = std::make_shared<std::optional<host::FrameExportPublicationResultV1>>();
    auto sharedRequest = std::shared_ptr<host::FrameExportRequestV1>(std::move(request));
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("export-publication-test",
                             runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                .id = runtime::TaskOwnerId::fromRaw(2)},
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [&artifacts, sharedRequest, shared, scratchDir, clock](runtime::TaskContext& context) {
            auto result = host::executeExportPublication(
                context, artifacts, std::move(*sharedRequest), scratchDir, clock);
            shared->emplace(std::move(result));
            return runtime::TaskResult<void>::succeeded();
        });
    if (!submission.accepted()) {
        return std::nullopt;
    }
    return ExportRun{std::move(submission.handle), shared};
}

// Polls handle.tryTakeResult() to terminality, then returns the worker's own recorded outcome.
[[nodiscard]] std::optional<host::FrameExportPublicationResultV1> awaitExportRun(ExportRun& run) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (run.handle.tryTakeResult().has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::move(*run.result);
}

// -------------------------------------------------------------------------------------------
// Approval
// -------------------------------------------------------------------------------------------

void testDigestMismatchRejected(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "digest mismatch: fixture is available")) {
        return;
    }
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                          fixture.ledger(), fixture.path() / "mismatch.exr");
    if (attempt == nullptr) {
        return;
    }

    const auto goodDigest = requireDigest(attempt, expectations);
    auto mutableBytes = bloom::core::Sha256Digest::Bytes{};
    std::ranges::copy(goodDigest.bytes(), mutableBytes.begin());
    mutableBytes[0] = static_cast<std::uint8_t>(mutableBytes[0] ^ 0xFFU);
    const auto flipped = bloom::core::Sha256Digest::fromBytes(mutableBytes);

    auto result = host::approveFrameExportV1(fixture.coordinator(), attempt, flipped);
    expectations.expect(
        result.status() == host::FrameExportApprovalStatusV1::DigestMismatch,
        "an approver digest that does not byte-equal the attempt's own digest is rejected");
    expectations.expect(!result, "a rejected approval carries no request");
}

void testIntentRegisteredExactlyOnce(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "intent-once: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "intent-once.exr";
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                          fixture.ledger(), target);
    if (attempt == nullptr) {
        return;
    }
    const auto targetKey = attempt->target().targetKey;

    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval),
                        "approval with the exact retained digest succeeds");
    if (!approval) {
        return;
    }
    auto snapshot = fixture.coordinator().targetSnapshot(targetKey);
    expectations.expect(static_cast<bool>(snapshot) &&
                            snapshot.snapshot().activeTargetClaimCount == 1,
                        "approval registers exactly one active target claim for this target");
}

// -------------------------------------------------------------------------------------------
// Job execution
// -------------------------------------------------------------------------------------------

void testEndToEndExportPublished(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "end to end: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "published.exr";
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                          fixture.ledger(), target);
    if (attempt == nullptr) {
        return;
    }
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "end to end: approval succeeds");
    if (!approval) {
        return;
    }

    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "end to end: the export job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "end to end: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    expectations.expect(static_cast<bool>(*result) && result->publication() != nullptr &&
                            result->publication()->outcome ==
                                platform::StagedArtifactPublicationOutcome::Published,
                        "end to end: the export publishes");
    expectations.expect(std::filesystem::exists(target), "end to end: the target file now exists");
    const auto semanticDigest = result->semanticDigest();
    expectations.expect(semanticDigest.has_value() && result->artifactDigest().has_value(),
                        "end to end: both the semantic and artifact digests are surfaced");
    if (!semanticDigest.has_value()) {
        return;
    }

    // "reopened file passes the F1 verifier independently".
    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 independentVerifier;
    const auto verifyResult =
        independentVerifier.verify(target, attempt->processIdentity(), attempt->report(), {});
    expectations.expect(
        verifyResult.status() == output::FlatExrVerifyStatusV1::Verified &&
            verifyResult.digest() == *semanticDigest, // NOLINT(bugprone-unchecked-optional-access)
        "the published target independently reopen-verifies to the surfaced digest");
}

void testSupersessionOlderPublishesSecond(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "supersession: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "superseded.exr";

    auto attemptA = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                           fixture.ledger(), target);
    auto attemptB = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                           fixture.ledger(), target);
    if (attemptA == nullptr || attemptB == nullptr) {
        return;
    }
    auto approvalA = host::approveFrameExportV1(fixture.coordinator(), attemptA,
                                                requireDigest(attemptA, expectations));
    auto approvalB = host::approveFrameExportV1(fixture.coordinator(), attemptB,
                                                requireDigest(attemptB, expectations));
    expectations.expect(static_cast<bool>(approvalA) && static_cast<bool>(approvalB),
                        "supersession: both intents approve for the same target");
    if (!approvalA || !approvalB) {
        return;
    }
    auto requestA = std::move(approvalA).takeRequest();
    auto requestB = std::move(approvalB).takeRequest();
    const auto olderIntent = requestA->intentId();
    const auto newerIntent = requestB->intentId();
    expectations.expect(olderIntent.value() < newerIntent.value(),
                        "supersession: the first-approved intent is numerically older");

    // The older intent's job runs FULLY to completion first, while the target is still untouched
    // (the newer intent has not published yet): its own preflight/writing/verifying all succeed
    // against the still-absent target, so only its final Guard step observes that a newer
    // same-target intent already exists.
    auto runA = beginExportRun(fixture.scheduler(), fixture.artifacts(), std::move(requestA),
                               fixture.path());
    auto* runAValue = require(runA, expectations, "supersession: the older job is submitted");
    if (runAValue == nullptr) {
        return;
    }
    auto resultAOpt = awaitExportRun(*runAValue);
    const auto* resultA =
        require(resultAOpt, expectations, "supersession: the older job reaches a terminal state");
    if (resultA == nullptr) {
        return;
    }
    expectations.expect(static_cast<bool>(*resultA) && resultA->publication() != nullptr &&
                            resultA->publication()->outcome ==
                                platform::StagedArtifactPublicationOutcome::Superseded,
                        "the older intent's own publish observes Superseded");
    expectations.expect(!std::filesystem::exists(target),
                        "a superseded older intent leaves the target untouched");

    auto runB = beginExportRun(fixture.scheduler(), fixture.artifacts(), std::move(requestB),
                               fixture.path());
    auto* runBValue = require(runB, expectations, "supersession: the newer job is submitted");
    if (runBValue == nullptr) {
        return;
    }
    auto resultBOpt = awaitExportRun(*runBValue);
    const auto* resultB =
        require(resultBOpt, expectations, "supersession: the newer job reaches a terminal state");
    if (resultB == nullptr) {
        return;
    }
    expectations.expect(static_cast<bool>(*resultB) && resultB->publication() != nullptr &&
                            resultB->publication()->outcome ==
                                platform::StagedArtifactPublicationOutcome::Published,
                        "the newer intent publishes normally");
    expectations.expect(std::filesystem::exists(target), "the newer intent's file is intact");
}

void testExternalModificationConflict(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "external conflict: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "conflict.exr";
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                          fixture.ledger(), target);
    if (attempt == nullptr) {
        return;
    }
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "external conflict: approval succeeds");
    if (!approval) {
        return;
    }

    // An out-of-band write between approval and publish -- the attempt observed an absent target,
    // but by the time the job runs a file now exists there.
    {
        std::ofstream external(target, std::ios::binary);
        external << "not a bloom export";
    }

    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "external conflict: the job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "external conflict: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    const auto* failure = result->failure();
    expectations.expect(failure != nullptr &&
                            failure->payloadAs<platform::StagedArtifactError>() != nullptr &&
                            *failure->payloadAs<platform::StagedArtifactError>() ==
                                platform::StagedArtifactError::ExternalModificationConflict,
                        "an externally-modified target between approval and publish is a typed "
                        "conflict, not a crash");
    std::ifstream reopened(target);
    std::string contents((std::istreambuf_iterator<char>(reopened)),
                         std::istreambuf_iterator<char>());
    expectations.expect(contents == "not a bloom export",
                        "the externally-written target is left exactly as it was");
}

[[nodiscard]] host::OutputAnalysisAttemptRequestV1
attemptRequestWithPolicy(const std::filesystem::path& target,
                         const platform::ArtifactOverwritePolicy policy) {
    auto request = attemptRequestFor(target);
    request.overwritePolicy = policy;
    return request;
}

void testOverwritePolicyCreateOnlyDeniesExisting(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "overwrite create-only: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "create-only.exr";
    {
        std::ofstream existing(target, std::ios::binary);
        existing << "already here";
    }
    auto begin = host::beginOutputAnalysisAttemptV1(
        fixture.scheduler(), fixture.artifacts(), fixture.ledger(),
        attemptRequestWithPolicy(target, platform::ArtifactOverwritePolicy::CreateOnly));
    expectations.expect(static_cast<bool>(begin),
                        "overwrite create-only: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    while (std::chrono::steady_clock::now() < deadline) {
        outcome = runner.tryComplete();
        if (outcome.has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto* outcomeValue = require(
        outcome, expectations, "overwrite create-only: the attempt reaches a terminal outcome");
    if (outcomeValue == nullptr) {
        return;
    }
    expectations.expect(
        !*outcomeValue,
        "CreateOnly against an existing target fails the attempt's own Resolving preflight");
}

void testOverwritePolicyReplaceExistingSucceeds(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "overwrite replace-existing: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "replace-existing.exr";
    {
        std::ofstream existing(target, std::ios::binary);
        existing << "already here";
    }
    auto begin = host::beginOutputAnalysisAttemptV1(
        fixture.scheduler(), fixture.artifacts(), fixture.ledger(),
        attemptRequestWithPolicy(target, platform::ArtifactOverwritePolicy::ReplaceExisting));
    expectations.expect(static_cast<bool>(begin),
                        "overwrite replace-existing: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    while (std::chrono::steady_clock::now() < deadline) {
        outcome = runner.tryComplete();
        if (outcome.has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto* outcomeValue =
        require(outcome, expectations,
                "overwrite replace-existing: the attempt reaches a terminal outcome");
    if (outcomeValue == nullptr) {
        return;
    }
    expectations.expect(static_cast<bool>(*outcomeValue),
                        "ReplaceExisting against an existing target completes the attempt");
    if (!*outcomeValue) {
        return;
    }
    auto attempt = outcomeValue->attempt();
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval),
                        "overwrite replace-existing: approval succeeds");
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "overwrite replace-existing: the job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result = require(resultOpt, expectations,
                                 "overwrite replace-existing: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    expectations.expect(static_cast<bool>(*result) && result->publication() != nullptr &&
                            result->publication()->targetWasPublished(),
                        "ReplaceExisting overwrites the pre-existing target with the export");
}

void testDurabilityWarningViaFaultInjection(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "durability warning: fixture is available")) {
        return;
    }
    // A test seam the platform surface already exposes (bloom/platform/staged_artifact.hpp's
    // StagedArtifactFaultPlan): a fresh coordinator configured to inject a ParentDurability
    // failure at the parent-directory fsync step so the publish outcome is deterministically
    // PublishedWithDurabilityWarning rather than depending on an unreliable real filesystem/host
    // condition.
    auto injectedArtifacts = platform::StagedArtifactCoordinator::create(
        {.faults = {.point = platform::StagedArtifactFaultPoint::ParentDurability,
                    .occurrence = 1}});
    expectations.expect(injectedArtifacts.succeeded(),
                        "durability warning: the fault-injecting coordinator is created");
    if (!injectedArtifacts) {
        return;
    }
    auto artifacts = std::move(injectedArtifacts).takeCoordinator();
    const auto target = fixture.path() / "durability.exr";
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), artifacts,
                                          fixture.ledger(), target);
    if (attempt == nullptr) {
        return;
    }
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "durability warning: approval succeeds");
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), artifacts, std::move(approval).takeRequest(),
                              fixture.path());
    auto* runValue = require(run, expectations, "durability warning: the job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "durability warning: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    expectations.expect(
        static_cast<bool>(*result) && result->publication() != nullptr &&
            result->publication()->outcome ==
                platform::StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning,
        "an injected parent-durability fault yields PublishedWithDurabilityWarning");
    expectations.expect(std::filesystem::exists(target),
                        "the target is visible despite the durability warning");
}

void testCancellationBeforeStagingLeavesTargetIntact(Expectations& expectations) {
    // "Cancellation mid-Writing and mid-Verifying" -- see the implementor's report for why the
    // frozen runtime::CancellationToken API cannot preempt a single synchronous F1 writer/verifier
    // call from outside it (no self-cancellation seam exists); this exercises the closest
    // deterministic equivalent this pipeline can offer: cancellation observed at the stage
    // boundary immediately before Writing/Verifying begins, with the identical caller-observable
    // contract ("previous target intact, typed outcome").
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "cancellation: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "cancelled.exr";
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                          fixture.ledger(), target);
    if (attempt == nullptr) {
        return;
    }
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "cancellation: approval succeeds");
    if (!approval) {
        return;
    }

    auto shared = std::make_shared<std::optional<host::FrameExportPublicationResultV1>>();
    auto sharedRequest =
        std::shared_ptr<host::FrameExportRequestV1>(std::move(approval).takeRequest());
    auto scratchDir = fixture.path();
    auto submission = fixture.scheduler().submit<void>(
        runtime::TaskRequest("export-publication-cancel-test",
                             runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                .id = runtime::TaskOwnerId::fromRaw(3)},
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [&artifacts = fixture.artifacts(), sharedRequest, shared,
         scratchDir](runtime::TaskContext& context) {
            while (!context.isCancellationRequested()) {
            }
            auto result = host::executeExportPublication(context, artifacts,
                                                         std::move(*sharedRequest), scratchDir);
            shared->emplace(std::move(result));
            return runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted(), "cancellation: the job is submitted");
    if (!submission.accepted()) {
        return;
    }
    submission.handle.cancel();
    ExportRun run{std::move(submission.handle), shared};
    // Two legitimate outcomes race here, both correct: the scheduler may cancel the task before
    // ever invoking this closure (shared stays unpopulated -- awaitExportRun() then returns
    // nullopt), or the closure starts, observes cancellation at its very first checkpoint, and
    // returns a typed CancelledResult. Either way nothing is published.
    auto resultOpt = awaitExportRun(run);
    if (resultOpt.has_value()) {
        expectations.expect(!*resultOpt, // NOLINT(bugprone-unchecked-optional-access)
                            "cancellation: an early-cancelled job never reaches publish()");
    }
    expectations.expect(!std::filesystem::exists(target),
                        "cancellation before staging leaves the (nonexistent) target untouched");
}

// A deterministic fake clock (design decision 5's injectable clock seam): the first two calls
// establish `start`/`lastProgress`; every call after that jumps `jump` ahead, tripping the
// relevant deadline/no-progress limit without depending on real elapsed wall time.
[[nodiscard]] output::OutputExportClockV1
jumpingClock(const std::chrono::steady_clock::duration jump) {
    auto calls = std::make_shared<int>(0);
    return [calls, jump]() noexcept {
        ++*calls;
        if (*calls <= 2) {
            return std::chrono::steady_clock::time_point{};
        }
        return std::chrono::steady_clock::time_point{} + jump;
    };
}

void testDeadlineExpiryViaInjectedClock(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "deadline expiry: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "deadline.exr";
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                          fixture.ledger(), target);
    if (attempt == nullptr) {
        return;
    }
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "deadline expiry: approval succeeds");
    if (!approval) {
        return;
    }

    // Jumps 25 hours ahead, past the default 24-hour deadline.
    auto run =
        beginExportRun(fixture.scheduler(), fixture.artifacts(), std::move(approval).takeRequest(),
                       fixture.path(), jumpingClock(std::chrono::hours(25)));
    auto* runValue = require(run, expectations, "deadline expiry: the job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "deadline expiry: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    const auto* failure = result->failure();
    expectations.expect(
        failure != nullptr && failure->payloadAs<host::FrameExportDeadlineExceededV1>() != nullptr,
        "the injected clock jump is diagnosed as a typed deadline-exceeded resource failure");
    expectations.expect(!std::filesystem::exists(target), "deadline expiry publishes nothing");
}

void testNoProgressExpiryViaInjectedClock(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "no-progress expiry: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "no-progress.exr";
    auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                          fixture.ledger(), target);
    if (attempt == nullptr) {
        return;
    }
    // A 1-nanosecond no-progress interval (design decision 5's injectable-limits half of the same
    // seam: "a request may lower but not raise" the closed version 1 limits) trips
    // deterministically on the real system clock's very first between-stage gap, regardless of
    // exactly how many times F1's own writer/verifier progress callback fires for this fixture's
    // tiny image -- a fake clock jump landing precisely between a no-progress "check" call and the
    // immediately preceding progress-driven "reset" call would otherwise be sensitive to that
    // internal, implementation-owned call cadence (see testDeadlineExpiryViaInjectedClock for the
    // sibling case where a fake clock jump IS safely deterministic: total-deadline is checked
    // against a fixed `start`, never reset mid-flight).
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations),
                                               {.totalDeadline = std::chrono::hours(24),
                                                .noProgressInterval = std::chrono::nanoseconds(1)});
    expectations.expect(static_cast<bool>(approval), "no-progress expiry: approval succeeds");
    if (!approval) {
        return;
    }

    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "no-progress expiry: the job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "no-progress expiry: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    const auto* failure = result->failure();
    expectations.expect(
        failure != nullptr &&
            failure->payloadAs<host::FrameExportNoProgressExceededV1>() != nullptr,
        "the injected clock jump is diagnosed as a typed no-progress resource failure");
    expectations.expect(!std::filesystem::exists(target), "no-progress expiry publishes nothing");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testDigestMismatchRejected(expectations);
        testIntentRegisteredExactlyOnce(expectations);
        testEndToEndExportPublished(expectations);
        testSupersessionOlderPublishesSecond(expectations);
        testExternalModificationConflict(expectations);
        testOverwritePolicyCreateOnlyDeniesExisting(expectations);
        testOverwritePolicyReplaceExistingSucceeds(expectations);
        testDurabilityWarningViaFaultInjection(expectations);
        testCancellationBeforeStagingLeavesTargetIntact(expectations);
        testDeadlineExpiryViaInjectedClock(expectations);
        testNoProgressExpiryViaInjectedClock(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
