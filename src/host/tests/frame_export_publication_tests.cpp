#include <bloom/host/frame_export_publication.hpp>

#include "gpu_route_proof_export_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/output/flat_exr_reopen_verifier.hpp>
#include <bloom/output/png_reopen_verifier.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>

// Used only by independentlyDecodePng() below -- a from-scratch reader (raw chunk parse + zlib
// inflate) that never calls into bloom::output's own PNG writer/verifier, so the PNG round-trip
// test proves the published artifact against a second, independent reading. Mirrors
// src/output/tests/png_test_support.hpp's own helper, duplicated here because a src module's tests
// may not reach across a sibling module's tests/ directory (the same boundary
// src/ui/tests/main_window_readonly_placeholder_tests.cpp documents for its own duplicate).
#include <zlib.h>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
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

namespace routeproof = bloom::gpu_route_proof_export;

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
constexpr auto kWidthParameterId = document::ParameterId::fromRaw(0x2105);
constexpr auto kHeightParameterId = document::ParameterId::fromRaw(0x2106);
constexpr auto kRevision = document::Revision::fromRaw(0x2006);
constexpr auto kLayerNodeId = document::NodeId::fromRaw(0x2007);
constexpr auto kLayerId = document::LayerId::fromRaw(0x2008);
constexpr auto kStackNodeId = document::NodeId::fromRaw(0x2009);
constexpr auto kSlotId = document::LayerSlotId::fromRaw(0x200a);
constexpr auto kPositionParameterId = document::ParameterId::fromRaw(0x200b);
constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(0x200c);
constexpr auto kAnchorParameterId = document::ParameterId::fromRaw(0x200d);
constexpr auto kScaleParameterId = document::ParameterId::fromRaw(0x200e);
constexpr auto kRotationParameterId = document::ParameterId::fromRaw(0x200f);
constexpr auto kBlendModeParameterId = document::ParameterId::fromRaw(0x2010);

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> smallSolidPlan() {
    const auto format = document::CompositionFormat::create(2, 2);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeId,
                               {kColorParameterId, bloom::core::Color4d{0.1, 0.2, 0.3, 1.0}},
                               {kWidthParameterId, 2.0},
                               {kHeightParameterId, 2.0}});
    // CompiledCompositionOutput requires a layer-stack input, not a bare solid (mirrors
    // bloom/output/tests/flat_exr_test_support.hpp's shellPlan()): solid -> layer output -> layer
    // stack -> composition output.
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNodeId, kLayerId, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{kPositionParameterId, document::Vec2d{0.5, 0.5}},
        runtime::CompiledVec2Parameter{kAnchorParameterId, document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{kScaleParameterId, document::kDefaultScale},
        runtime::CompiledScalarParameter{kRotationParameterId, document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{kOpacityParameterId, 1.0}, kBlendModeParameterId,
        bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
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

[[nodiscard]] host::OutputAnalysisAttemptRequestV1 attemptRequestFor(
    const std::filesystem::path& target,
    const output::OutputPresetV1 preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
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
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)},
        .preset = preset};
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
[[nodiscard]] std::shared_ptr<const output::OutputAnalysisAttemptV1> buildApprovableAttempt(
    Expectations& expectations, runtime::TaskScheduler& scheduler,
    platform::StagedArtifactCoordinator& artifacts, output::ExportResourceLedgerV1& ledger,
    const std::filesystem::path& target,
    const output::OutputPresetV1 preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1) {
    auto begin = host::beginOutputAnalysisAttemptV1(scheduler, artifacts, ledger,
                                                    attemptRequestFor(target, preset));
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

// -------------------------------------------------------------------------------------------
// PNG preset (issue #111)
// -------------------------------------------------------------------------------------------

struct IndependentPngDecode final {
    bool ok = false;
    std::vector<std::string> chunkTypesInOrder;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t colorType = 0;
    std::uint8_t interlaceMethod = 0;
    std::uint8_t srgbIntent = 0;
    bool everyCrcValid = false;
    bool everyRowFilterZero = false;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] std::uint32_t readBigEndianU32(const std::vector<unsigned char>& bytes,
                                             const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] IndependentPngDecode independentlyDecodePng(const std::filesystem::path& path) {
    IndependentPngDecode result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return result;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
    static constexpr std::array<unsigned char, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 8 || !std::equal(kSignature.begin(), kSignature.end(), bytes.begin())) {
        return result;
    }

    result.everyCrcValid = true;
    std::vector<unsigned char> idatConcat;
    std::size_t offset = 8;
    bool sawIend = false;
    while (offset + 8 <= bytes.size()) {
        const auto length = readBigEndianU32(bytes, offset);
        const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(offset) + 4,
                               bytes.begin() + static_cast<std::ptrdiff_t>(offset) + 8);
        const auto dataOffset = offset + 8;
        if (dataOffset + length + 4 > bytes.size()) {
            return result;
        }
        const std::span<const unsigned char> data(bytes.data() + dataOffset, length);
        auto crc = crc32_z(0L, reinterpret_cast<const Bytef*>(type.data()), 4);
        if (!data.empty()) {
            crc = crc32_z(crc, reinterpret_cast<const Bytef*>(data.data()), data.size());
        }
        const auto crcOffset = dataOffset + length;
        if (static_cast<std::uint32_t>(crc) != readBigEndianU32(bytes, crcOffset)) {
            result.everyCrcValid = false;
        }
        result.chunkTypesInOrder.push_back(type);
        if (type == "IHDR") {
            if (length != 13) {
                return result;
            }
            result.width = readBigEndianU32(bytes, dataOffset);
            result.height = readBigEndianU32(bytes, dataOffset + 4);
            result.bitDepth = data[8];
            result.colorType = data[9];
            result.interlaceMethod = data[12];
        } else if (type == "sRGB") {
            if (length != 1) {
                return result;
            }
            result.srgbIntent = data[0];
        } else if (type == "IDAT") {
            idatConcat.insert(idatConcat.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            sawIend = true;
        }
        offset = crcOffset + 4;
        if (sawIend) {
            break;
        }
    }
    if (!sawIend || offset != bytes.size()) {
        return result;
    }

    const auto expectedTotal =
        static_cast<std::size_t>(result.width) * result.height * 4 + result.height;
    std::vector<unsigned char> inflated(expectedTotal);
    z_stream inflateStream{};
    if (inflateInit(&inflateStream) != Z_OK) {
        return result;
    }
    inflateStream.next_in = idatConcat.data();
    inflateStream.avail_in = static_cast<uInt>(idatConcat.size());
    inflateStream.next_out = inflated.data();
    inflateStream.avail_out = static_cast<uInt>(inflated.size());
    const auto status = inflate(&inflateStream, Z_FINISH);
    inflateEnd(&inflateStream);
    if (status != Z_STREAM_END || inflateStream.avail_out != 0) {
        return result;
    }

    result.everyRowFilterZero = true;
    result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4);
    const auto rowRgbaBytes = static_cast<std::size_t>(result.width) * 4;
    for (std::uint32_t row = 0; row < result.height; ++row) {
        const auto rowOffset = static_cast<std::size_t>(row) * (rowRgbaBytes + 1);
        if (inflated[rowOffset] != 0) {
            result.everyRowFilterZero = false;
        }
        const auto destinationOffset = static_cast<std::size_t>(row) * rowRgbaBytes;
        std::copy_n(inflated.begin() + static_cast<std::ptrdiff_t>(rowOffset) + 1, rowRgbaBytes,
                    result.rgba.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
    }
    result.ok = true;
    return result;
}

// Independently re-derives the prepared straight-RGBA8 stream from the attempt's retained process
// frame, through a FRESHLY built qualified display processor (never the one the export retained),
// so G1's reopen verifier can be run against the published file with inputs the export path did
// not hand it.
[[nodiscard]] bool
independentlyVerifyPng(Expectations& expectations,
                       const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
                       const std::filesystem::path& target,
                       const bloom::core::Sha256Digest& expectedSemanticDigest) {
    const auto built = runtime::buildBloomNeutralQualifiedDisplayProcessor();
    expectations.expect(built.succeeded(),
                        "independent PNG verify: a fresh qualified processor builds");
    if (!built.succeeded()) {
        return false;
    }
    const runtime::CpuQualifiedDisplayPreparer preparer(*built.handle());
    const auto prepared = preparer.prepare(
        attempt->frame(),
        {.aggregatePixelStorageByteLimit = output::kOutputExportPreparedPngBytesMaximumV1,
         .chunkPixelCount = runtime::kDefaultQualifiedDisplayChunkPixelCount,
         .viewAdjust = {},
         .displayName = {},
         .viewName = {},
         .showLook = true},
        {});
    expectations.expect(prepared.status() == runtime::QualifiedDisplayPreparationStatus::Prepared &&
                            prepared.frame() != nullptr,
                        "independent PNG verify: the display frame re-derives");
    if (prepared.status() != runtime::QualifiedDisplayPreparationStatus::Prepared ||
        prepared.frame() == nullptr) {
        return false;
    }
    const auto& buffer = prepared.frame()->buffer();
    const output::PngRgba8SrgbPreparedStreamV1 stream{.dimensions = buffer.displayWindow().extent(),
                                                      .pixels = buffer.pixels()};
    const auto& display = attempt->display();
    expectations.expect(display.isPresent(),
                        "independent PNG verify: the attempt retains its display products");
    bloom::core::Sha256Digest revision;
    if (display.expectedOcioRevision.has_value()) {
        revision = *display.expectedOcioRevision;
    } else {
        return false;
    }
    const output::PngRgba8SrgbReopenVerifierV1 verifier;
    const auto verifyResult = verifier.verify(target, stream, attempt->processIdentity(),
                                              attempt->report(), revision, display.identity, {});
    expectations.expect(verifyResult.status() == output::PngVerifyStatusV1::Verified,
                        "independent PNG verify: the published file reopen-verifies");
    if (verifyResult.status() != output::PngVerifyStatusV1::Verified) {
        return false;
    }
    expectations.expect(verifyResult.digest() == expectedSemanticDigest,
                        "independent PNG verify: the independently issued kind-1 digest equals the "
                        "digest the export surfaced");
    return true;
}

void testEndToEndPngExportPublished(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png end to end: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "published.png";
    auto attempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), target, output::OutputPresetV1::PngRgba8SrgbV1);
    if (attempt == nullptr) {
        return;
    }
    expectations.expect(attempt->preset() == output::OutputPresetV1::PngRgba8SrgbV1 &&
                            attempt->display().isPresent(),
                        "png end to end: the attempt is a PNG attempt retaining its qualified "
                        "display-processor handle, identity, and expected OCIO revision");
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "png end to end: approval succeeds");
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "png end to end: the export job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "png end to end: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    expectations.expect(static_cast<bool>(*result) && result->publication() != nullptr &&
                            result->publication()->outcome ==
                                platform::StagedArtifactPublicationOutcome::Published,
                        "png end to end: the export publishes");
    expectations.expect(std::filesystem::exists(target),
                        "png end to end: the target file now exists");
    const auto semanticDigest = result->semanticDigest();
    expectations.expect(semanticDigest.has_value() && result->artifactDigest().has_value(),
                        "png end to end: both the semantic and artifact digests are surfaced");
    if (!semanticDigest.has_value()) {
        return;
    }

    const auto decoded = independentlyDecodePng(target);
    expectations.expect(decoded.ok && decoded.everyCrcValid && decoded.everyRowFilterZero,
                        "png end to end: the published file independently decodes with valid CRCs "
                        "and zero row filters");
    expectations.expect(decoded.chunkTypesInOrder ==
                            std::vector<std::string>{"IHDR", "sRGB", "IDAT", "IEND"},
                        "png end to end: the published file carries exactly the closed chunk "
                        "profile");
    expectations.expect(decoded.width == 2 && decoded.height == 2 && decoded.bitDepth == 8 &&
                            decoded.colorType == 6 && decoded.interlaceMethod == 0 &&
                            decoded.srgbIntent == 0,
                        "png end to end: the independent decode sees the required IHDR/sRGB "
                        "fields");

    static_cast<void>(independentlyVerifyPng(
        expectations, attempt, target,
        *semanticDigest)); // NOLINT(bugprone-unchecked-optional-access) -- guarded above.
}

void testPngSemanticDigestStableAcrossTwoRuns(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png digest stability: fixture is available")) {
        return;
    }
    std::array<std::optional<bloom::core::Sha256Digest>, 2> digests;
    for (int index = 0; index < 2; ++index) {
        const auto target = fixture.path() / ("stable-" + std::to_string(index) + ".png");
        auto attempt = buildApprovableAttempt(expectations, fixture.scheduler(),
                                              fixture.artifacts(), fixture.ledger(), target,
                                              output::OutputPresetV1::PngRgba8SrgbV1);
        if (attempt == nullptr) {
            return;
        }
        auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                                   requireDigest(attempt, expectations));
        if (!approval) {
            expectations.expect(false, "png digest stability: approval succeeds");
            return;
        }
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        auto* runValue = require(run, expectations, "png digest stability: the job is submitted");
        if (runValue == nullptr) {
            return;
        }
        auto resultOpt = awaitExportRun(*runValue);
        const auto* result = require(resultOpt, expectations,
                                     "png digest stability: the job reaches a terminal state");
        if (result == nullptr) {
            return;
        }
        digests.at(static_cast<std::size_t>(index)) = result->semanticDigest();
    }
    expectations.expect(digests[0].has_value() && digests[1].has_value() &&
                            digests[0] == digests[1],
                        "png digest stability: the kind-1 semantic identity digest is identical "
                        "across two full publications of the identical fixture");
}

void testBothPresetsExportFromTheSameFixture(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "both presets: fixture is available")) {
        return;
    }
    const auto exrTarget = fixture.path() / "both.exr";
    const auto pngTarget = fixture.path() / "both.png";

    auto exrAttempt = buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                                             fixture.ledger(), exrTarget);
    auto pngAttempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), pngTarget, output::OutputPresetV1::PngRgba8SrgbV1);
    if (exrAttempt == nullptr || pngAttempt == nullptr) {
        return;
    }
    expectations.expect(exrAttempt->preset() ==
                                output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 &&
                            exrAttempt->display().isAbsent(),
                        "both presets: the EXR attempt retains no display product at all");
    expectations.expect(pngAttempt->preset() == output::OutputPresetV1::PngRgba8SrgbV1 &&
                            pngAttempt->display().isPresent(),
                        "both presets: the PNG attempt retains its display products");
    expectations.expect(*exrAttempt->digest() != *pngAttempt->digest(), // NOLINT
                        "both presets: the two presets' approval digests differ over the same "
                        "frame");

    std::array<std::optional<bloom::core::Sha256Digest>, 2> semantic;
    const std::array<std::shared_ptr<const output::OutputAnalysisAttemptV1>, 2> attempts{
        exrAttempt, pngAttempt};
    for (std::size_t index = 0; index < attempts.size(); ++index) {
        auto approval = host::approveFrameExportV1(fixture.coordinator(), attempts.at(index),
                                                   requireDigest(attempts.at(index), expectations));
        if (!approval) {
            expectations.expect(false, "both presets: approval succeeds");
            return;
        }
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        auto* runValue = require(run, expectations, "both presets: the job is submitted");
        if (runValue == nullptr) {
            return;
        }
        auto resultOpt = awaitExportRun(*runValue);
        const auto* result =
            require(resultOpt, expectations, "both presets: the job reaches a terminal state");
        if (result == nullptr) {
            return;
        }
        expectations.expect(static_cast<bool>(*result) && result->publication() != nullptr &&
                                result->publication()->outcome ==
                                    platform::StagedArtifactPublicationOutcome::Published,
                            "both presets: the export publishes");
        semantic.at(index) = result->semanticDigest();
    }
    expectations.expect(std::filesystem::exists(exrTarget) && std::filesystem::exists(pngTarget),
                        "both presets: both target files exist");
    expectations.expect(semantic[0].has_value() && semantic[1].has_value() &&
                            semantic[0] != semantic[1],
                        "both presets: the two presets' output semantic identities differ");

    // EXR magic (0x76 0x2f 0x31 0x01) vs the PNG signature -- proof each preset actually wrote its
    // own container, not the other one under a different extension.
    const auto readMagic = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        std::array<char, 4> magic{};
        stream.read(magic.data(), 4);
        return magic;
    };
    const auto exrMagic = readMagic(exrTarget);
    const auto pngMagic = readMagic(pngTarget);
    expectations.expect(static_cast<unsigned char>(exrMagic[0]) == 0x76U &&
                            static_cast<unsigned char>(exrMagic[1]) == 0x2FU,
                        "both presets: the .exr target really is an OpenEXR file");
    expectations.expect(static_cast<unsigned char>(pngMagic[0]) == 137U && pngMagic[1] == 'P' &&
                            pngMagic[2] == 'N' && pngMagic[3] == 'G',
                        "both presets: the .png target really is a PNG file");
}

void testPngPreparedBytesLimitExceededIsTyped(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png prepared limit: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "prepared-limit.png";
    auto attempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), target, output::OutputPresetV1::PngRgba8SrgbV1);
    if (attempt == nullptr) {
        return;
    }
    // A LOWERED prepared-bytes ceiling ("a request may lower but not raise them"): the 2x2 fixture
    // needs 16 prepared bytes, so 8 is over-limit. The closed 256 MiB ceiling itself can never be
    // exceeded through the production pipeline (the closed 2^26 pixel-count limit caps prepared
    // bytes at exactly 256 MiB), so this seam is the only way to reach the rejection -- see
    // output_limits.hpp's own reachability note.
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations),
                                               {.totalDeadline = std::chrono::hours(24),
                                                .noProgressInterval = std::chrono::seconds(120),
                                                .preparedPngByteLimit = 8});
    expectations.expect(static_cast<bool>(approval), "png prepared limit: approval succeeds");
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    auto* runValue = require(run, expectations, "png prepared limit: the job is submitted");
    if (runValue == nullptr) {
        return;
    }
    auto resultOpt = awaitExportRun(*runValue);
    const auto* result =
        require(resultOpt, expectations, "png prepared limit: the job reaches a terminal state");
    if (result == nullptr) {
        return;
    }
    const auto* failure = result->failure();
    expectations.expect(failure != nullptr &&
                            failure->payloadAs<host::FrameExportPreparedBytesExceededV1>() !=
                                nullptr &&
                            failure->stage() == host::FrameExportPublicationStageV1::ColorPreparing,
                        "png prepared limit: an over-limit prepared stream is a typed "
                        "FrameExportPreparedBytesExceededV1 failure at ColorPreparing");
    expectations.expect(!std::filesystem::exists(target),
                        "png prepared limit: nothing is published");
}

void testPngColorPreparingCancellationPublishesNothing(Expectations& expectations) {
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "png color cancellation: fixture is available")) {
        return;
    }
    const auto target = fixture.path() / "colour-cancelled.png";
    auto attempt =
        buildApprovableAttempt(expectations, fixture.scheduler(), fixture.artifacts(),
                               fixture.ledger(), target, output::OutputPresetV1::PngRgba8SrgbV1);
    if (attempt == nullptr) {
        return;
    }
    auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                               requireDigest(attempt, expectations));
    expectations.expect(static_cast<bool>(approval), "png color cancellation: approval succeeds");
    if (!approval) {
        return;
    }

    auto shared = std::make_shared<std::optional<host::FrameExportPublicationResultV1>>();
    auto sharedRequest =
        std::shared_ptr<host::FrameExportRequestV1>(std::move(approval).takeRequest());
    auto scratchDir = fixture.path();
    auto reachedColorPreparing = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    auto submission = fixture.scheduler().submit<void>(
        runtime::TaskRequest("export-publication-color-cancel-test",
                             runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                .id = runtime::TaskOwnerId::fromRaw(4)},
                             runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo),
        [&artifacts = fixture.artifacts(), sharedRequest, shared, scratchDir, reachedColorPreparing,
         released](runtime::TaskContext& context) {
            // Blocks at the one ColorPreparing report emitted immediately before C2's chunked
            // apply loop, giving the outer thread a deterministic point to request cancellation --
            // no sleep, no race against a background worker.
            output::OutputExportProgressCallbackV1 progress =
                [reachedColorPreparing, released](const output::OutputExportProgressV1& update) {
                    if (update.stage != output::OutputExportStageV1::ColorPreparing ||
                        update.completed != 0 || update.total != 0) {
                        return;
                    }
                    reachedColorPreparing->store(true, std::memory_order_release);
                    while (!released->load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                };
            auto result = host::executeExportPublication(
                context, artifacts, std::move(*sharedRequest), scratchDir, {}, std::move(progress));
            shared->emplace(std::move(result));
            return runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted(), "png color cancellation: the job is submitted");
    if (!submission.accepted()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!reachedColorPreparing->load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(reachedColorPreparing->load(std::memory_order_acquire),
                        "png color cancellation: the job reaches the ColorPreparing rendezvous");
    submission.handle.cancel();
    released->store(true, std::memory_order_release);

    ExportRun run{std::move(submission.handle), shared};
    auto resultOpt = awaitExportRun(run);
    const auto* result = require(resultOpt, expectations,
                                 "png color cancellation: the job reaches a terminal state");
    if (result != nullptr) {
        expectations.expect(!*result && result->publication() == nullptr,
                            "png color cancellation: a cancelled job never reaches publish()");
    }
    expectations.expect(!std::filesystem::exists(target),
                        "png color cancellation: cancellation during ColorPreparing publishes "
                        "nothing");
}

// Native GPU-composited publication parity. Requires a real loader/device
// (BLOOM_TEST_VULKAN_LOADER, an absolute path) and skips otherwise unless --require-device is
// passed. It builds a CPU reference attempt and GPU attempts over the identical plan/request,
// proves the retained process images agree within the frozen 2e-6 finite-component tolerance with
// exact alpha, then publishes the GPU attempt as EXR and both attempts as PNG, independently
// reopen-verifying the EXR and independently decoding both PNGs to compare the RGBA8 bytes (RGB
// within one code, alpha exact) against the unchanged CPU reference.
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
// Real provider with a qualified device and packaged shader tools, ready to prepare display
// commands. Returns null (with a NOTE) when the loader/device is unavailable, exactly like the
// existing GPU publication tests.
[[nodiscard]] std::shared_ptr<host::GpuExportProvider>
makeGpuDisplayTestProvider(ExportFixture& fixture, const std::string_view context) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping " << context << '\n';
        return nullptr;
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = std::filesystem::path(loader);
    auto provider = host::GpuExportProvider::create(options);
    provider->prepare(fixture.scheduler());
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!provider->deviceAvailable()) {
        std::cout << "NOTE: no compatible Vulkan device; skipping " << context << '\n';
        return nullptr;
    }
    runtime::GpuOcioCompileOptions displayCompile;
    displayCompile.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    displayCompile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    provider->setGpuDisplayCompileOptions(std::move(displayCompile));
    return provider;
}

// Runs one PNG attempt carrying a deliberately-refused `request.outputColorCommand` and proves the
// runner never launders it: no GPU display payload is retained, the retained frame and provenance
// are the honest CPU reference path, and the CPU display products still publish a valid PNG.
void expectGpuDisplayCommandRefused(
    Expectations& expectations, ExportFixture& fixture,
    const std::shared_ptr<host::GpuExportProvider>& provider,
    const std::shared_ptr<const runtime::PreparedGpuOcioCommand>& refused,
    const std::string_view context) {
    const auto target =
        fixture.path() / (std::string(context) + "-refused.png");
    auto request = attemptRequestFor(target, output::OutputPresetV1::PngRgba8SrgbV1);
    request.gpuProvider = provider;
    request.outputColorCommand = refused;
    auto begin = host::beginOutputAnalysisAttemptV1(fixture.scheduler(), fixture.artifacts(),
                                                    fixture.ledger(), std::move(request));
    expectations.expect(static_cast<bool>(begin), "gpu command refusal: the attempt begins");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        outcome = runner.tryComplete();
        if (outcome.has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(outcome.has_value() && static_cast<bool>(*outcome),
                        "gpu command refusal: the mismatched command is refused and the attempt "
                        "falls back to the honest CPU path");
    if (!outcome.has_value() || !*outcome) {
        return;
    }
    expectations.expect(!(*outcome).attempt()->gpuDisplay().isPresent(),
                        "gpu command refusal: no GPU display payload is retained");
    expectations.expect((*outcome).attempt()->frame() != nullptr &&
                            (*outcome).attempt()->frame()->identity().provider ==
                                runtime::EvaluationProvider::CpuReference,
                        "gpu command refusal: the retained frame is the CPU reference");
    const auto& provenance = (*outcome).gpuProvenance();
    expectations.expect(provenance.has_value() && !provenance->gpuEvaluated() &&
                            provenance->encodedArm == runtime::GpuOutputColorArm::None &&
                            provenance->transferredPayloads == 0,
                        "gpu command refusal: the GPU arm is never reported as evaluated");
    auto approval = host::approveFrameExportV1(fixture.coordinator(), (*outcome).attempt(),
                                               requireDigest((*outcome).attempt(), expectations));
    if (!approval) {
        return;
    }
    auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                              std::move(approval).takeRequest(), fixture.path());
    if (!run.has_value()) {
        return;
    }
    const auto resultOpt = awaitExportRun(*run);
    expectations.expect(resultOpt.has_value() && static_cast<bool>(*resultOpt) &&
                            std::filesystem::exists(target),
                        "gpu command refusal: the honest CPU fallback publishes the PNG");
}

// A command prepared for the SAME geometry but a WRONG transform (a process-effect CST rather than
// the resolved display transform) must be refused. The runner accepts a supplied command only when
// its canonical identity byte-equals the command it prepared from the exact resolved
// config/working space/display/view and geometry.
void testGpuSameGeometryWrongTransformRefused(Expectations& expectations) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "wrong transform: fixture is available")) {
        return;
    }
    auto provider = makeGpuDisplayTestProvider(fixture, "wrong transform refusal");
    if (provider == nullptr) {
        return;
    }
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest, runtime::kLinearRec709SceneColorSpaceId);
    expectations.expect(resolution.ready(),
                        "wrong transform: the built-in neutral config resolves");
    if (!resolution.ready()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto config = std::move(resolution).takeResolved();
    expectations.expect(config.has_value(), "wrong transform: the resolved config is taken");
    if (!config.has_value()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    runtime::GpuOcioCompileOptions compile;
    compile.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    compile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    runtime::GpuOcioProgramPreparer preparer(runtime::GpuOcioPreparerBudgets{});
    runtime::GpuOcioTransformSpec spec;
    spec.kind = runtime::GpuOcioTransformKind::Cst;
    spec.fromId = std::string(config->processColorSpaceId());
    spec.toId = std::string(config->outputColorSpaceId());
    auto prepared =
        preparer.prepare(*config, spec, runtime::GpuOcioCommandGeometry{2, 2}, compile);
    expectations.expect(prepared.hasValue(),
                        "wrong transform: the process-effect command itself prepares with the "
                        "same geometry");
    if (prepared.hasValue()) {
        expectations.expect(
            prepared.command->encoding() == runtime::GpuOcioOutputEncoding::FinalRgba32f,
            "wrong transform: the injected command is a process-effect (not display) command");
        expectGpuDisplayCommandRefused(expectations, fixture, provider, prepared.command,
                                       "wrong transform");
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#else
    static_cast<void>(expectations);
#endif
}

// A command prepared for a DIFFERENT config revision / frame (the ACEScg built-in rather than the
// resolved Bloom Neutral config) with the same geometry must be refused: its canonical identity
// differs from the command the runner prepared from the exact resolved config.
void testGpuCrossRevisionCommandRefused(Expectations& expectations) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "cross revision: fixture is available")) {
        return;
    }
    auto provider = makeGpuDisplayTestProvider(fixture, "cross revision refusal");
    if (provider == nullptr) {
        return;
    }
    const auto acesRevision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    expectations.expect(acesRevision.has_value(), "cross revision: the ACES revision resolves");
    if (!acesRevision.has_value()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *acesRevision, "ACEScg");
    expectations.expect(resolution.ready(), "cross revision: the ACES config resolves");
    if (!resolution.ready()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto aces = std::move(resolution).takeResolved();
    expectations.expect(aces.has_value(), "cross revision: the ACES config is taken");
    if (!aces.has_value()) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    auto prepared = provider->prepareGpuDisplayCommand(
        *aces, aces->displayName(), aces->viewName(), runtime::GpuOcioCommandGeometry{2, 2});
    expectations.expect(prepared.hasValue(),
                        "cross revision: the ACES display command itself prepares with the same "
                        "geometry");
    if (prepared.hasValue()) {
        expectGpuDisplayCommandRefused(expectations, fixture, provider, prepared.command,
                                       "cross revision");
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#else
    static_cast<void>(expectations);
#endif
}
#endif

// A stale/mismatched GPU display command (prepared for the wrong geometry) is refused, never
// silently used: the evaluator's combined readback rejects the geometry binding and the attempt
// fails typed at Evaluating. The honest CPU display path is the no-command case, covered by the
// existing CPU-only PNG publication tests.
void testGpuStaleDisplayBindingRefused(Expectations& expectations) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping stale binding refusal\n";
        return;
    }
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "stale binding: fixture is available")) {
        return;
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = std::filesystem::path(loader);
    auto provider = host::GpuExportProvider::create(options);
    provider->prepare(fixture.scheduler());
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!provider->deviceAvailable()) {
        std::cout << "NOTE: no compatible Vulkan device; skipping stale binding refusal\n";
        return;
    }
    runtime::GpuOcioCompileOptions displayCompile;
    displayCompile.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    displayCompile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    provider->setGpuDisplayCompileOptions(std::move(displayCompile));

    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest, runtime::kLinearRec709SceneColorSpaceId);
    if (!resolution.ready()) {
        return;
    }
    auto config = std::move(resolution).takeResolved();
    if (!config.has_value()) {
        return;
    }
    // Deliberately wrong geometry for the 2x2 fixture plan.
    auto prepared = provider->prepareGpuDisplayCommand(
        *config, config->displayName(), config->viewName(), runtime::GpuOcioCommandGeometry{1, 1});
    expectations.expect(prepared.hasValue(), "stale binding: the stale command itself prepares");
    if (!prepared.hasValue()) {
        return;
    }
    const auto target = fixture.path() / "stale.png";
    auto request = attemptRequestFor(target, output::OutputPresetV1::PngRgba8SrgbV1);
    request.gpuProvider = provider;
    request.outputColorCommand = prepared.command;
    auto begin = host::beginOutputAnalysisAttemptV1(fixture.scheduler(), fixture.artifacts(),
                                                    fixture.ledger(), std::move(request));
    expectations.expect(static_cast<bool>(begin), "stale binding: the attempt begins");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        outcome = runner.tryComplete();
        if (outcome.has_value()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(outcome.has_value() && static_cast<bool>(*outcome),
                        "stale binding: the mismatched command is refused and the attempt falls "
                        "back to the honest CPU display path");
    if (!outcome.has_value() || !*outcome) {
        static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
        return;
    }
    expectations.expect(!(*outcome).attempt()->gpuDisplay().isPresent(),
                        "stale binding: no GPU display payload is retained");
    const auto& provenance = (*outcome).gpuProvenance();
    expectations.expect(provenance.has_value() && !provenance->gpuEvaluated(),
                        "stale binding: the GPU arm is never reported as evaluated");
    // The CPU fallback still publishes a valid PNG (the honest retained display products).
    auto approval = host::approveFrameExportV1(fixture.coordinator(), (*outcome).attempt(),
                                               requireDigest((*outcome).attempt(), expectations));
    if (approval) {
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        if (run.has_value()) {
            const auto resultOpt = awaitExportRun(*run);
            expectations.expect(resultOpt.has_value() && static_cast<bool>(*resultOpt) &&
                                    std::filesystem::exists(target),
                                "stale binding: the CPU fallback publishes the PNG");
        }
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
#else
    static_cast<void>(expectations);
#endif
}

//
// When `proofDirectory` is non-empty this same real run also publishes the genuine
// `route.export.still_frame` proof, but only after every assertion above passed: the proof carries
// the attempt's canonical process identity, the actual cold dispatch/epoch/readback counters, and
// an evidence digest bound to the actual GPU/CPU process comparison and the independently decoded
// PNG bytes. An absent loader/device returns Skipped so the proof CTest reports 77, never a pass.
enum class GpuProofOutcome : std::uint8_t { NotRequested, Skipped, Ran };

GpuProofOutcome testGpuCompositedExportParity(Expectations& expectations, const bool requireDevice,
                                              const std::filesystem::path& proofDirectory) {
    const auto skip = [&proofDirectory] {
        return proofDirectory.empty() ? GpuProofOutcome::NotRequested : GpuProofOutcome::Skipped;
    };
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(
                false, "gpu parity: --require-device needs BLOOM_TEST_VULKAN_LOADER absolute");
        } else {
            std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping GPU publication parity\n";
        }
        return skip();
    }
    ExportFixture fixture;
    if (!fixture.setUp(expectations, "gpu parity: fixture is available")) {
        return GpuProofOutcome::Ran;
    }
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = true;
    options.loaderPath = std::filesystem::path(loader);
    auto provider = host::GpuExportProvider::create(options);
    provider->prepare(fixture.scheduler());
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "gpu parity: a device is required");
        std::cout << "NOTE: no compatible Vulkan device; skipping GPU publication parity\n";
        return skip();
    }
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    // Qualified packaged shader tools: enables the real GPU DisplayRgba8 PNG route (the CPU display
    // path remains the fallback when these are unavailable).
    runtime::GpuOcioCompileOptions displayCompile;
    displayCompile.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    displayCompile.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    provider->setGpuDisplayCompileOptions(std::move(displayCompile));
#endif

    const auto buildAttempt =
        [&](const std::filesystem::path& target, const output::OutputPresetV1 preset,
            const std::shared_ptr<host::GpuExportProvider>& gpu,
            std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
            std::optional<host::OutputAnalysisAttemptGpuProvenanceV1>& provenance) -> bool {
        auto request = attemptRequestFor(target, preset);
        request.gpuProvider = gpu;
        auto begin = host::beginOutputAnalysisAttemptV1(fixture.scheduler(), fixture.artifacts(),
                                                        fixture.ledger(), std::move(request));
        if (!begin) {
            return false;
        }
        auto runner = std::move(begin).takeHandle();
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto outcome = runner.tryComplete();
            if (!outcome.has_value()) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            if (!*outcome) {
                return false;
            }
            attempt = outcome->attempt();
            provenance = outcome->gpuProvenance();
            return attempt != nullptr;
        }
        return false;
    };
    const auto publish = [&](const std::shared_ptr<const output::OutputAnalysisAttemptV1>& attempt,
                             const std::filesystem::path& target) -> bool {
        auto approval = host::approveFrameExportV1(fixture.coordinator(), attempt,
                                                   requireDigest(attempt, expectations));
        if (!approval) {
            return false;
        }
        auto run = beginExportRun(fixture.scheduler(), fixture.artifacts(),
                                  std::move(approval).takeRequest(), fixture.path());
        if (!run.has_value()) {
            return false;
        }
        auto resultOpt = awaitExportRun(*run);
        return resultOpt.has_value() && static_cast<bool>(*resultOpt) &&
               resultOpt->publication() != nullptr &&
               resultOpt->publication()->outcome ==
                   platform::StagedArtifactPublicationOutcome::Published &&
               std::filesystem::exists(target);
    };

    std::shared_ptr<const output::OutputAnalysisAttemptV1> cpuAttempt;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> gpuExrAttempt;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> gpuPngAttempt;
    std::optional<host::OutputAnalysisAttemptGpuProvenanceV1> cpuProvenance;
    std::optional<host::OutputAnalysisAttemptGpuProvenanceV1> gpuExrProvenance;
    std::optional<host::OutputAnalysisAttemptGpuProvenanceV1> gpuPngProvenance;
    const auto cpuPngTarget = fixture.path() / "parity-cpu.png";
    const auto gpuExrTarget = fixture.path() / "parity-gpu.exr";
    const auto gpuPngTarget = fixture.path() / "parity-gpu.png";
    if (!buildAttempt(cpuPngTarget, output::OutputPresetV1::PngRgba8SrgbV1, nullptr, cpuAttempt,
                      cpuProvenance) ||
        !buildAttempt(gpuExrTarget, output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                      provider, gpuExrAttempt, gpuExrProvenance) ||
        !buildAttempt(gpuPngTarget, output::OutputPresetV1::PngRgba8SrgbV1, provider, gpuPngAttempt,
                      gpuPngProvenance)) {
        expectations.expect(false, "gpu parity: the CPU/GPU attempts complete");
        return GpuProofOutcome::Ran;
    }
    expectations.expect(
        gpuExrAttempt->frame()->identity().provider == runtime::EvaluationProvider::GpuResident &&
            gpuPngAttempt->frame()->identity().provider == runtime::EvaluationProvider::GpuResident,
        "gpu parity: both GPU attempts retain GpuResident provenance");
    expectations.expect(gpuExrProvenance.has_value() && gpuExrProvenance->gpuEvaluated() &&
                            gpuExrProvenance->counters.readbacks == 1 &&
                            gpuPngProvenance.has_value() && gpuPngProvenance->gpuEvaluated() &&
                            gpuPngProvenance->counters.readbacks == 1,
                        "gpu parity: both GPU attempts performed exactly one final readback");

    // Process parity against the unchanged CPU final reference: finite-component absolute/relative
    // 2e-6, alpha exact.
    const auto& cpuPixels = cpuAttempt->frame()->processImage().pixels();
    const auto& gpuPixels = gpuExrAttempt->frame()->processImage().pixels();
    constexpr float kTolerance = 2e-6F;
    bool processParity = cpuPixels.size() == gpuPixels.size();
    if (processParity) {
        for (std::size_t index = 0; index < cpuPixels.size() && processParity; ++index) {
            const auto& cpuPixel = cpuPixels[index];
            const auto& gpuPixel = gpuPixels[index];
            if (cpuPixel.alpha() != gpuPixel.alpha()) {
                processParity = false;
                break;
            }
            for (std::size_t component = 0; component < 3; ++component) {
                const auto a = cpuPixel.components()[component];
                const auto b = gpuPixel.components()[component];
                const auto difference = std::abs(a - b);
                if (difference > kTolerance &&
                    difference > kTolerance * std::max(std::abs(a), std::abs(b))) {
                    processParity = false;
                    break;
                }
            }
        }
    }
    expectations.expect(processParity,
                        "gpu parity: GPU process pixels match the CPU reference within 2e-6 with "
                        "exact alpha");

    expectations.expect(publish(gpuExrAttempt, gpuExrTarget),
                        "gpu parity: the GPU-composited EXR publishes");
    const output::FlatExrRgba32fLinRec709SceneReopenVerifierV1 exrVerifier;
    const auto exrVerify = exrVerifier.verify(gpuExrTarget, gpuExrAttempt->processIdentity(),
                                              gpuExrAttempt->report(), {});
    expectations.expect(exrVerify.status() == output::FlatExrVerifyStatusV1::Verified,
                        "gpu parity: the GPU-composited EXR independently reopen-verifies pixels "
                        "and descriptors");

    expectations.expect(publish(cpuAttempt, cpuPngTarget) && publish(gpuPngAttempt, gpuPngTarget),
                        "gpu parity: both PNGs publish");
    const auto cpuDecoded = independentlyDecodePng(cpuPngTarget);
    const auto gpuDecoded = independentlyDecodePng(gpuPngTarget);
    expectations.expect(cpuDecoded.ok && gpuDecoded.ok && cpuDecoded.width == gpuDecoded.width &&
                            cpuDecoded.height == gpuDecoded.height &&
                            cpuDecoded.rgba.size() == gpuDecoded.rgba.size(),
                        "gpu parity: both PNGs independently decode with matching descriptors");
    bool pngParity =
        cpuDecoded.ok && gpuDecoded.ok && cpuDecoded.rgba.size() == gpuDecoded.rgba.size();
    if (pngParity) {
        for (std::size_t index = 0; index < cpuDecoded.rgba.size(); ++index) {
            const int difference = std::abs(static_cast<int>(cpuDecoded.rgba[index]) -
                                            static_cast<int>(gpuDecoded.rgba[index]));
            const bool alpha = index % 4U == 3U;
            if ((alpha && difference != 0) || (!alpha && difference > 1)) {
                pngParity = false;
                break;
            }
        }
    }
    expectations.expect(pngParity,
                        "gpu parity: GPU-composited PNG pixels match the CPU reference within one "
                        "code with exact alpha");

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    // Real GPU display route: the PNG attempt retains the verified DisplayRgba8 payload directly
    // (no CPU per-pixel display conversion), transferred in the SAME single submission as the
    // process payload.
    expectations.expect(gpuPngAttempt->gpuDisplay().isPresent(),
                        "gpu parity: the PNG attempt retains a GPU-encoded display payload");
    expectations.expect(gpuPngProvenance.has_value() &&
                            gpuPngProvenance->encodedArm ==
                                runtime::GpuOutputColorArm::DisplayRgba8 &&
                            gpuPngProvenance->readbackSubmissions == 1 &&
                            gpuPngProvenance->transferredPayloads == 2 &&
                            gpuPngProvenance->encodedPayloadBytes > 0 &&
                            gpuPngProvenance->counters.nativeDispatches > 0,
                        "gpu parity: the PNG route makes one submission carrying process+encoded "
                        "payloads with real dispatches");
#endif

    // Publish the genuine still-frame route proof from the exact run above. It is written only
    // after every assertion passed, so a failed parity/publication can never become a passing
    // proof; the evidence digest is bound to the actual process comparison and the independent PNG
    // decode, and the counters are the real attempt provenance.
    if (!proofDirectory.empty() && expectations.failures() == 0) {
        const auto identityDigest =
            routeproof::sha256Hex(gpuExrAttempt->processIdentity()->canonicalBytes());
        const auto& proofCpuPixels = cpuAttempt->frame()->processImage().pixels();
        const auto& proofGpuPixels = gpuExrAttempt->frame()->processImage().pixels();
        constexpr double kProofTolerance = static_cast<double>(kTolerance);
        routeproof::FrameEvidence evidence;
        evidence.identityHex = identityDigest;
        evidence.comparedPixels = proofCpuPixels.size();
        evidence.alphaExact = proofCpuPixels.size() == proofGpuPixels.size();
        double maxFloatDelta = 0.0;
        std::uint64_t processMismatches = 0;
        if (proofCpuPixels.size() == proofGpuPixels.size()) {
            for (std::size_t index = 0; index < proofCpuPixels.size(); ++index) {
                const auto& cpuPixel = proofCpuPixels[index];
                const auto& gpuPixel = proofGpuPixels[index];
                if (cpuPixel.alpha() != gpuPixel.alpha()) {
                    evidence.alphaExact = false;
                    ++processMismatches;
                    continue;
                }
                bool pixelBad = false;
                for (std::size_t component = 0; component < 3; ++component) {
                    const double cpuValue = static_cast<double>(cpuPixel.components()[component]);
                    const double gpuValue = static_cast<double>(gpuPixel.components()[component]);
                    const double difference = std::abs(cpuValue - gpuValue);
                    maxFloatDelta = std::max(maxFloatDelta, difference);
                    if (difference > kProofTolerance &&
                        difference >
                            kProofTolerance * std::max(std::abs(cpuValue), std::abs(gpuValue))) {
                        pixelBad = true;
                    }
                }
                if (pixelBad) {
                    ++processMismatches;
                }
            }
        }
        evidence.maxFloatDelta = maxFloatDelta;
        evidence.mismatchedPixels = processMismatches;
        evidence.cpuDecodedDigest =
            routeproof::sha256Pixels(std::span<const std::uint8_t>(cpuDecoded.rgba));
        evidence.gpuDecodedDigest =
            routeproof::sha256Pixels(std::span<const std::uint8_t>(gpuDecoded.rgba));
        std::uint64_t decodedMismatches = 0;
        std::uint64_t maxIntegerDelta = 0;
        bool decodedAlphaExact = true;
        const auto decodedCount = std::min(cpuDecoded.rgba.size(), gpuDecoded.rgba.size());
        for (std::size_t index = 0; index < decodedCount; ++index) {
            const auto difference =
                static_cast<std::uint64_t>(std::abs(static_cast<int>(cpuDecoded.rgba[index]) -
                                                    static_cast<int>(gpuDecoded.rgba[index])));
            maxIntegerDelta = std::max(maxIntegerDelta, difference);
            const bool alpha = index % 4U == 3U;
            if (alpha) {
                if (difference != 0) {
                    decodedAlphaExact = false;
                    ++decodedMismatches;
                }
            } else if (difference > 1) {
                ++decodedMismatches;
            }
        }
        evidence.maxIntegerDelta = maxIntegerDelta;
        evidence.mismatchedPixels += decodedMismatches;
        evidence.alphaExact = evidence.alphaExact && decodedAlphaExact;

        routeproof::ExportProofCounters counters;
        counters.deviceOwnershipEpoch = gpuExrProvenance->deviceOwnershipEpoch;
        counters.nativeDispatches = gpuExrProvenance->counters.nativeDispatches;
        counters.verifiedFrames = 1;
        counters.readbackSubmissions = gpuExrProvenance->readbackSubmissions;
        // The ACTUAL combined-readback payload count and bytes from the attempt provenance: the
        // identity/EXR arm transfers one process payload; a display/PNG arm transfers two (process +
        // encoded) under the same one submission. Never derived from the readback count.
        counters.payloads = gpuExrProvenance->transferredPayloads;
        counters.transferredBytes = gpuExrProvenance->processPayloadBytes +
                                    gpuExrProvenance->encodedPayloadBytes;

        const std::array<std::string, 1> identityList{identityDigest};
        const std::array<routeproof::FrameEvidence, 1> evidenceList{evidence};
        const auto processDigest = routeproof::orderedIdentityDigest(identityList);
        const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidenceList);
        std::string nonce;
        if (!routeproof::readProofNonce(proofDirectory, nonce)) {
            expectations.expect(false,
                                "gpu parity: a fresh run nonce is required for the still proof");
        } else {
            const auto written = routeproof::publishExportProof(
                proofDirectory, nonce, "route.export.still_frame",
                bloom::runtime::GpuRouteHarnessKind::StillFrameExport, counters, processDigest,
                capturedEvidenceDigest);
            if (!written.written) {
                expectations.expect(false, "gpu parity: the still route proof was rejected: " +
                                               written.detail);
            } else {
                std::cout << "PASS(route-proof) route.export.still_frame frames="
                          << counters.verifiedFrames << " dispatches=" << counters.nativeDispatches
                          << " readbacks=" << counters.readbackSubmissions
                          << " bytes=" << counters.transferredBytes << '\n';
            }
        }
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
    return GpuProofOutcome::Ran;
}

} // namespace

int main(const int argc, char** argv) {
    Expectations expectations;
    bool requireDevice = false;
    std::filesystem::path proofDirectory;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--require-device") {
            requireDevice = true;
        } else if (argument == "--route-proof-dir" && index + 1 < argc) {
            proofDirectory = argv[++index];
        }
    }
    GpuProofOutcome stillOutcome = GpuProofOutcome::NotRequested;
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
        testEndToEndPngExportPublished(expectations);
        testPngSemanticDigestStableAcrossTwoRuns(expectations);
        testBothPresetsExportFromTheSameFixture(expectations);
        testPngPreparedBytesLimitExceededIsTyped(expectations);
        testPngColorPreparingCancellationPublishesNothing(expectations);
        testGpuStaleDisplayBindingRefused(expectations);
        testGpuSameGeometryWrongTransformRefused(expectations);
        testGpuCrossRevisionCommandRefused(expectations);
        stillOutcome = testGpuCompositedExportParity(expectations, requireDevice, proofDirectory);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (expectations.failures() != 0) {
        return EXIT_FAILURE;
    }
    // With --route-proof-dir and no loader/device the honest result is a CTest skip, never a pass
    // labelled as a proof.
    return stillOutcome == GpuProofOutcome::Skipped ? 77 : EXIT_SUCCESS;
}
