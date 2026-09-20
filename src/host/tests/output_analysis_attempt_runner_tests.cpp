#include <bloom/host/output_analysis_attempt_runner.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>

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
constexpr auto kWidthParameterId = document::ParameterId::fromRaw(0x1105);
constexpr auto kHeightParameterId = document::ParameterId::fromRaw(0x1106);
constexpr auto kRevision = document::Revision::fromRaw(0x1006);
constexpr auto kLayerNodeId = document::NodeId::fromRaw(0x1007);
constexpr auto kLayerId = document::LayerId::fromRaw(0x1008);
constexpr auto kStackNodeId = document::NodeId::fromRaw(0x1009);
constexpr auto kSlotId = document::LayerSlotId::fromRaw(0x100a);
constexpr auto kPositionParameterId = document::ParameterId::fromRaw(0x100b);
constexpr auto kOpacityParameterId = document::ParameterId::fromRaw(0x100c);
constexpr auto kAnchorParameterId = document::ParameterId::fromRaw(0x100d);
constexpr auto kScaleParameterId = document::ParameterId::fromRaw(0x100e);
constexpr auto kRotationParameterId = document::ParameterId::fromRaw(0x100f);
constexpr auto kBlendModeParameterId = document::ParameterId::fromRaw(0x1010);

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
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNodeId,
                               {kColorParameterId, bloom::core::Color4d{0.25, 0.5, 0.75, 1.0}},
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

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions
gpuOptions(const bool enabled, std::filesystem::path loader = {}) {
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = enabled;
    options.loaderPath = std::move(loader);
    return options;
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

// Exercises the ACTUAL export host path (beginOutputAnalysisAttemptV1 -> Resolving -> Evaluating ->
// Identifying -> Analyzing) with the genuine GPU final-render bridge attached. The loader is read
// from BLOOM_TEST_VULKAN_LOADER so no machine-specific path is embedded in source; an absent loader
// or device prints an explicit NOTE and is not a failure. When `requireDevice` is set the same
// absence is a hard failure, so a native qualification run can demand a real device. The runtime
// fixture bloom.runtime.gpu_process_frame is the authority for per-op native dispatch counters;
// this host test is the authority for the counters/provenance retained by the REAL output attempt.
void testGpuEvaluatorHostPath(Expectations& expectations, const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(false,
                                "gpu host path: --require-device needs BLOOM_TEST_VULKAN_LOADER "
                                "to name an absolute loader");
            return;
        }
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping the GPU export host path\n";
        return;
    }
    runtime::TaskScheduler scheduler;
    auto provider =
        host::GpuExportProvider::create(gpuOptions(true, std::filesystem::path(loader)));
    provider->prepare(scheduler);
    // Test convenience only: wait for the provider's off-thread bootstrap to terminal. Production
    // never polls; the attempt runner defers its evaluation stage instead.
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(), "gpu host path: provider bootstrap terminals");
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "gpu host path: a device is required");
        const auto handle = provider->evaluator();
        std::cout << "NOTE: no compatible Vulkan device; skipping the GPU export host path: "
                  << (handle != nullptr ? handle->availabilityDiagnostic().message : std::string{})
                  << '\n';
        return;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "gpu host path: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "gpu host path: coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;

    auto request = requestFor(directory.path() / "gpu-attempt.exr");
    request.gpuProvider = provider;
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
    expectations.expect(static_cast<bool>(begin),
                        "gpu host path: begin submits the Resolving task");
    if (!begin) {
        return;
    }
    auto runner = std::move(begin).takeHandle();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value(), "gpu host path: attempt reaches a terminal outcome");
    if (!outcome.has_value()) {
        return;
    }
    expectations.expect(static_cast<bool>(*outcome),
                        "gpu host path: the GPU-evaluated attempt completes");
    if (*outcome) {
        expectations.expect((*outcome).attempt()->approvable() &&
                                (*outcome).attempt()->digest().has_value(),
                            "gpu host path: the attempt is approvable with a digest");
        // Native provenance/counters on the real output attempt: the frame came back from the
        // prepared-GPU scene executor with GpuResident provenance, performed real dispatches, and
        // did exactly one final RGBA32F readback.
        expectations.expect((*outcome).attempt()->frame()->identity().provider ==
                                runtime::EvaluationProvider::GpuResident,
                            "gpu host path: the retained frame has GpuResident provenance");
        const auto& provenance = (*outcome).gpuProvenance();
        expectations.expect(provenance.has_value() && provenance->gpuEvaluated(),
                            "gpu host path: the attempt records a GPU evaluation");
        if (provenance.has_value()) {
            expectations.expect(provenance->counters.nativeDispatches > 0,
                                "gpu host path: the attempt records real native dispatches");
            expectations.expect(provenance->counters.readbacks == 1,
                                "gpu host path: the attempt records exactly one final readback");
        }
    }
    static_cast<void>(provider->shutdownAndWait(std::chrono::seconds(10)));
}

// The explicit-off provider is truthful: no device native work is possible, the published
// evaluator reports unavailable, and the real output attempt still completes with zero GPU
// counters and CPU frame provenance. This is the desktop/headless fallback contract. The provider
// itself is dropped while the attempt is still in flight, proving the async attempt owns it.
void testGpuExportProviderDisabledFallback(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    auto provider = host::GpuExportProvider::create(gpuOptions(false));
    provider->prepare(scheduler);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(),
                        "provider-disabled: bootstrap reaches a terminal state");
    expectations.expect(provider->evaluator() != nullptr,
                        "provider-disabled: a CPU-unavailable evaluator is published");
    expectations.expect(!provider->deviceAvailable(),
                        "provider-disabled: no device is reported available");

    TempDirectory directory;
    expectations.expect(directory.isValid(), "provider-disabled: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "provider-disabled: coordinator is created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;
    auto request = requestFor(directory.path() / "disabled.exr");
    request.gpuProvider = provider;
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
    expectations.expect(static_cast<bool>(begin), "provider-disabled: begin submits the attempt");
    if (!begin) {
        return;
    }
    // The attempt now owns the provider; retire the local application handle immediately. Nothing
    // may dangle or fall back differently because the provider was retired mid-attempt.
    provider.reset();
    auto runner = std::move(begin).takeHandle();
    auto outcome = pumpUntilComplete(runner);
    expectations.expect(outcome.has_value() && static_cast<bool>(*outcome),
                        "provider-disabled: the CPU reference attempt completes");
    if (!outcome.has_value() || !*outcome) {
        return;
    }
    expectations.expect((*outcome).attempt()->frame()->identity().provider ==
                            runtime::EvaluationProvider::CpuReference,
                        "provider-disabled: the retained frame keeps CPU provenance");
    const auto& provenance = (*outcome).gpuProvenance();
    expectations.expect(
        provenance.has_value() && provenance->status == runtime::GpuProcessFrameStatus::Disabled &&
            provenance->counters.nativeDispatches == 0 && provenance->counters.readbacks == 0,
        "provider-disabled: provenance is Disabled with zero GPU work");
}

// Genuine asynchronous retirement against the real provider lifecycle. With a live device the
// provider is prepared, a real output attempt is started and deliberately interrupted by
// beginShutdown(), and the last attempt outcome handle is retained until after retirement
// completion is proven. beginShutdown() must return without blocking, and the UI-simulated pump
// must observe completion; only then is the evaluator released. The provider is destroyed before
// the scheduler (the ordering every route guarantees).
void testGpuExportProviderAsyncRetirement(Expectations& expectations, const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(
                false, "async-retirement: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        } else {
            std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping async GPU retirement\n";
        }
        return;
    }
    runtime::TaskScheduler scheduler;
    auto provider =
        host::GpuExportProvider::create(gpuOptions(true, std::filesystem::path(loader)));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + 30s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(), "async-retirement: bootstrap terminals");
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "async-retirement: a device is required");
        std::cout << "NOTE: no compatible Vulkan device; skipping async GPU retirement\n";
        return;
    }

    TempDirectory directory;
    expectations.expect(directory.isValid(), "async-retirement: temp directory is available");
    if (!directory.isValid()) {
        return;
    }
    auto artifacts = platform::StagedArtifactCoordinator::create({});
    expectations.expect(artifacts.succeeded(), "async-retirement: coordinator created");
    if (!artifacts) {
        return;
    }
    auto coordinator = std::move(artifacts).takeCoordinator();
    output::ExportResourceLedgerV1 ledger;
    auto request = requestFor(directory.path() / "async-retirement.exr");
    request.gpuProvider = provider;
    auto begin =
        host::beginOutputAnalysisAttemptV1(scheduler, coordinator, ledger, std::move(request));
    expectations.expect(static_cast<bool>(begin), "async-retirement: attempt begins");
    std::optional<host::OutputAnalysisAttemptOutcomeV1> outcome;
    if (begin) {
        auto runner = std::move(begin).takeHandle();
        // Deliberate mid-flight shutdown: signal the owner while the attempt may still be pending.
        const auto shutdownStarted = std::chrono::steady_clock::now();
        provider->beginShutdown();
        const auto shutdownElapsed = std::chrono::steady_clock::now() - shutdownStarted;
        expectations.expect(shutdownElapsed < std::chrono::seconds(1),
                            "async-retirement: beginShutdown never blocks the caller");
        outcome = pumpUntilComplete(runner);
        expectations.expect(outcome.has_value(),
                            "async-retirement: the interrupted attempt reaches a terminal outcome");
    }
    // The last attempt outcome handle is intentionally still alive here; retirement must complete
    // asynchronously without it being released.
    const auto retireDeadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < retireDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "async-retirement: retirement completes asynchronously");
    provider->collectRetired();
    outcome.reset(); // last attempt handle dropped only after completion proof
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(), "async-retirement: the scheduler quiesces");
}

// The root regression: admission is closed while the provider is still alive. Retirement completion
// is still proven asynchronously through the same non-blocking poll; the caller never blocks and
// never releases before completion proof. The assertion deliberately does not depend on which
// thread performs the eventual (trivial) release.
void testGpuExportProviderRetirementAfterAdmissionClosed(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->prepared() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->prepared(), "closed-admission: bootstrap terminals");
    expectations.expect(scheduler.isAccepting(), "closed-admission: admission starts open");
    scheduler.beginShutdown();
    expectations.expect(!scheduler.isAccepting(), "closed-admission: admission is closed");

    const auto shutdownStarted = std::chrono::steady_clock::now();
    provider->beginShutdown();
    expectations.expect(std::chrono::steady_clock::now() - shutdownStarted <
                            std::chrono::seconds(1),
                        "closed-admission: beginShutdown never blocks the caller");
    const auto retireDeadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < retireDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(
        provider->retirementComplete(),
        "closed-admission: retirement completes asynchronously after admission closed");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(),
                        "closed-admission: the scheduler quiesces after provider destruction");
}

// Shutting the provider down before its lazy bootstrap finishes must not create a device, must not
// join anything on the caller, and must leave the scheduler able to quiesce.
void testGpuExportProviderShutdownDuringBootstrap(Expectations& expectations) {
    runtime::TaskScheduler scheduler;
    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    provider->beginShutdown(); // shutdown while the bootstrap task is queued or running
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "shutdown-during-bootstrap: retirement completes");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(),
                        "shutdown-during-bootstrap: the scheduler quiesces, no join on caller");
}

// Deterministically holds the scheduler's single CPU worker so the provider's bootstrap stays
// queued. `started` proves the worker is genuinely occupied; `release` lets it finish. A single
// CPU worker is required: the default config derives a multi-worker pool, where the bootstrap would
// run on another worker instead of staying queued.
[[nodiscard]] runtime::TaskSchedulerConfig singleCpuWorkerConfig() {
    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 1;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return config;
}

struct CpuWorkerBlocker final {
    std::atomic_bool started{false};
    std::atomic_bool release{false};
    runtime::TaskHandle<void> handle;
};

[[nodiscard]] std::shared_ptr<CpuWorkerBlocker> occupyCpuWorker(runtime::TaskScheduler& scheduler) {
    auto blocker = std::make_shared<CpuWorkerBlocker>();
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest(
            "hold the CPU worker",
            {.kind = runtime::TaskOwnerKind::Export, .id = runtime::TaskOwnerId::fromRaw(7)},
            runtime::TaskPriority::Interactive, runtime::TaskExecutor::Cpu),
        [blocker](runtime::TaskContext&) -> runtime::TaskResult<void> {
            blocker->started.store(true, std::memory_order_release);
            while (!blocker->release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
            return runtime::TaskResult<void>::succeeded();
        });
    if (submission.accepted()) {
        blocker->handle = std::move(submission.handle);
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!blocker->started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return blocker;
}

// A scheduled bootstrap that is still queued (the CPU worker is deliberately occupied) is NOT
// complete, and beginShutdown() -- a mere signal -- must not make it complete either. Completion
// arrives only when the bootstrap task genuinely runs and terminalizes.
void testGpuExportProviderBootstrapInFlightIsNotComplete(Expectations& expectations) {
    runtime::TaskScheduler scheduler(singleCpuWorkerConfig());
    auto blocker = occupyCpuWorker(scheduler);
    expectations.expect(blocker->started.load(std::memory_order_acquire),
                        "bootstrap-in-flight: the CPU worker is genuinely occupied");

    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler); // bootstrap is accepted but stays queued behind the blocker
    expectations.expect(!provider->retirementComplete(),
                        "bootstrap-in-flight: a queued bootstrap is not complete");
    provider->beginShutdown();
    expectations.expect(!provider->retirementComplete(),
                        "bootstrap-in-flight: beginShutdown (a signal) is not completion proof");

    blocker->release.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "bootstrap-in-flight: retirement completes once the task terminalizes");
    expectations.expect(provider->evaluator() == nullptr,
                        "bootstrap-in-flight: a shutdown-racing bootstrap publishes no device");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(), "bootstrap-in-flight: the scheduler quiesces");
}

// A bootstrap queued when the scheduler closes admission is cancelled before its lambda ever runs.
// The provider must observe that owned task's terminal state and report completion, without ever
// constructing an evaluator and without waiting on anything the scheduler might never run.
void testGpuExportProviderQueuedBootstrapCancellationCompletes(Expectations& expectations) {
    runtime::TaskScheduler scheduler(singleCpuWorkerConfig());
    auto blocker = occupyCpuWorker(scheduler);
    expectations.expect(blocker->started.load(std::memory_order_acquire),
                        "queued-cancel: the CPU worker is genuinely occupied");

    auto provider = host::GpuExportProvider::create(
        gpuOptions(true, "/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    expectations.expect(!provider->retirementComplete(),
                        "queued-cancel: the queued bootstrap is not yet complete");

    scheduler.beginShutdown(); // cancels the queued bootstrap before its lambda can run
    blocker->release.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!provider->retirementComplete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(provider->retirementComplete(),
                        "queued-cancel: the cancelled bootstrap terminalizes retirement");
    expectations.expect(provider->evaluator() == nullptr,
                        "queued-cancel: no device was ever constructed");
    provider->collectRetired();
    provider.reset();
    const auto quiesceDeadline = std::chrono::steady_clock::now() + 15s;
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < quiesceDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    expectations.expect(scheduler.isQuiescent(), "queued-cancel: the scheduler quiesces");
}

} // namespace

int main(const int argc, char** argv) {
    Expectations expectations;
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            requireDevice = true;
        }
    }
    testFullGraphProducesStableDigestAcrossTwoRuns(expectations);
    testCancellationBeforeResolvingCompletes(expectations);
    testResourceExhaustionIsTypedWithZeroLeak(expectations);
    testGpuExportProviderDisabledFallback(expectations);
    testGpuExportProviderAsyncRetirement(expectations, requireDevice);
    testGpuExportProviderRetirementAfterAdmissionClosed(expectations);
    testGpuExportProviderShutdownDuringBootstrap(expectations);
    testGpuExportProviderBootstrapInFlightIsNotComplete(expectations);
    testGpuExportProviderQueuedBootstrapCancellationCompletes(expectations);
    testGpuEvaluatorHostPath(expectations, requireDevice);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}