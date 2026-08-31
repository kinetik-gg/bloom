#include <bloom/host/output_analysis_attempt_runner.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

// Task F2 (issue #101): drives beginOutputAnalysisAttemptV1() -- the Resolving (BlockingIo) ->
// Evaluating/Identifying/Analyzing (Cpu) two-task chain -- against a REAL bloom::runtime::
// TaskScheduler with real BlockingIo/Cpu workers and a REAL platform::StagedArtifactCoordinator,
// exactly mirroring bloom/host/tests/session_async_io_tests.cpp's own top-of-file rationale for
// doing so.
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

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-attempt-runner-XXXXXX";
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

constexpr auto kProjectId = document::ProjectId::fromRaw(0x1001);
constexpr auto kCompositionId = document::CompositionId::fromRaw(0x1002);
constexpr auto kSolidNodeId = document::NodeId::fromRaw(0x1003);
constexpr auto kOutputNodeId = document::NodeId::fromRaw(0x1004);
constexpr auto kColorParameterId = document::ParameterId::fromRaw(0x1005);
constexpr auto kRevision = document::Revision::fromRaw(0x1006);
constexpr auto kLayerNodeId = document::NodeId::fromRaw(0x1007);
constexpr auto kLayerId = document::LayerId::fromRaw(0x1008);
constexpr auto kStackNodeId = document::NodeId::fromRaw(0x1009);
constexpr auto kSlotId = document::LayerSlotId::fromRaw(0x100a);
constexpr auto kPositionParameterId = document::ParameterId::fromRaw(0x100b);
constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(0x100c);

// A trivial one-node (solid -> composition output) plan: a real, directly evaluable composition,
// unlike bloom/output/tests/flat_exr_test_support.hpp's shellPlan() (which evaluates a plan only
// to immediately overwrite the result with a fixture). This test exercises the real
// runtime::CpuCompositionEvaluator through beginOutputAnalysisAttemptV1()'s own Cpu stage.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> smallSolidPlan() {
    const auto format = document::CompositionFormat::create(2, 2);
    if (!format) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(runtime::CompiledSolid{kSolidNodeId, kColorParameterId,
                                                   bloom::core::Color4d{0.25, 0.5, 0.75, 1.0}});
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
requestFor(const std::filesystem::path& targetPath) {
    const auto plan = smallSolidPlan();
    return {
        .plan = plan,
        .evaluation = {.time = bloom::core::RationalTime::fromInteger(0),
                       .output = plan->output(),
                       .resolution = runtime::CompositionFormatResolution{},
                       .quality = runtime::EvaluationQuality::Reference,
                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                       .pixelStorageByteLimit = 4096},
        .targetPath = targetPath,
        .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
        .owner = {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(1)}};
}

[[nodiscard]] std::optional<host::OutputAnalysisAttemptOutcomeV1>
pumpUntilComplete(host::OutputAnalysisAttemptRunnerV1& runner) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto outcome = runner.tryComplete()) {
            return outcome;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

void testFullGraphProducesStableDigestAcrossTwoRuns(Expectations& expectations) {
    TempDirectory directory;
    expectations.expect(directory.isValid(), "full graph: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(),
                        "full graph: staged-artifact coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;

    std::optional<bloom::core::Sha256Digest> firstDigest;
    for (int run = 0; run < 2; ++run) {
        auto begin = host::beginOutputAnalysisAttemptV1(
            scheduler, coordinator, ledger,
            requestFor(directory.path() / ("attempt-" + std::to_string(run) + ".exr")));
        expectations.expect(static_cast<bool>(begin),
                            "full graph: begin submits the Resolving task");
        if (!begin) {
            continue;
        }
        auto runner = std::move(begin).takeHandle();
        auto outcome = pumpUntilComplete(runner);
        expectations.expect(outcome.has_value(),
                            "full graph: the attempt reaches a terminal outcome");
        if (!outcome.has_value()) {
            continue;
        }
        expectations.expect(static_cast<bool>(*outcome),
                            "full graph: Resolving -> Evaluating -> Identifying -> Analyzing "
                            "completes successfully");
        if (!*outcome) {
            continue;
        }
        expectations.expect((*outcome).attempt()->approvable() &&
                                (*outcome).attempt()->digest().has_value(),
                            "full graph: the completed attempt is approvable with a digest");
        if (run == 0) {
            firstDigest = (*outcome).attempt()->digest();
        } else {
            expectations.expect(firstDigest.has_value() &&
                                    (*outcome).attempt()->digest() == firstDigest,
                                "full graph: the digest is stable across two independent runs "
                                "over the identical fixture");
        }
    }
}

void testCancellationBeforeResolvingCompletes(Expectations& expectations) {
    TempDirectory directory;
    expectations.expect(directory.isValid(), "cancel: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "cancel: staged-artifact coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;

    auto begin = host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger,
                                                    requestFor(directory.path() / "cancelled.exr"));
    expectations.expect(static_cast<bool>(begin), "cancel: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    runner.requestCancellation();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value(), "cancel: the attempt reaches a terminal outcome");
    if (!outcome.has_value()) {
        return;
    }
    expectations.expect(!*outcome, "cancel: an immediately-cancelled attempt does not complete");
    const auto* failure = outcome->failure();
    expectations.expect(failure != nullptr && failure->cancelled(),
                        "cancel: the typed failure is marked cancelled");
}

void testResourceExhaustionIsTypedWithZeroLeak(Expectations& expectations) {
    TempDirectory directory;
    expectations.expect(directory.isValid(), "resource exhaustion: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(),
                        "resource exhaustion: staged-artifact coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger(/*concurrentAllowance=*/1);

    auto begin = host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger,
                                                    requestFor(directory.path() / "exhausted.exr"));
    expectations.expect(static_cast<bool>(begin),
                        "resource exhaustion: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value(),
                        "resource exhaustion: the attempt reaches a terminal outcome");
    if (!outcome.has_value()) {
        return;
    }
    expectations.expect(!*outcome,
                        "resource exhaustion: a 1-byte ledger cannot admit a real evaluated frame");
    const auto* failure = outcome->failure();
    expectations.expect(failure != nullptr &&
                            failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>() !=
                                nullptr &&
                            *failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>() ==
                                output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed,
                        "resource exhaustion: the failure is a typed ResourceReservationFailed at "
                        "the Analyzing stage");
    expectations.expect(ledger.chargedBytes() == 0,
                        "resource exhaustion: a refused attempt charges nothing (zero leak)");
}

} // namespace

int main() {
    Expectations expectations;
    testFullGraphProducesStableDigestAcrossTwoRuns(expectations);
    testCancellationBeforeResolvingCompletes(expectations);
    testResourceExhaustionIsTypedWithZeroLeak(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
