#include <bloom/host/output_analysis_attempt_runner.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>

#include <chrono>
#include <new>
#include <type_traits>
#include <utility>

namespace bloom::host {

namespace {

// The PNG-only color half of the blocking stage, in the exact closed input vocabulary
// analyzePngRgba8SrgbV1 accepts. `display` is populated only when both fields below are the
// nominal Ready/Qualified pair.
struct ColorResolutionOutcomeV1 final {
    output::PngRgba8SrgbColorResolutionStateV1 colorResolution =
        output::PngRgba8SrgbColorResolutionStateV1::Ready;
    output::OutputAnalysisAdapterStateV1 adapter = output::OutputAnalysisAdapterStateV1::Qualified;
    output::OutputAnalysisAttemptDisplayProductsV1 display;
    // The already-compiled, immutable GPU DisplayRgba8 command prepared on this blocking CPU stage
    // from the exact resolved display processor, when qualified tool paths are available. Null
    // keeps the unchanged CPU display path (the truthful fallback).
    std::shared_ptr<const runtime::PreparedGpuOcioCommand> gpuDisplayCommand;
};

// Everything the blocking stage hands the Cpu stage, behind ONE shared_ptr: runtime::
// TaskResultValue caps a task result at four pointers (task_scheduler.hpp), and the retained
// target plus the PNG display products exceed that on their own. Bundling them keeps the task
// result a single small handle while still transferring both products by value semantics.
struct ResolvedAttemptInputsV1 final {
    output::OutputAnalysisAttemptTargetV1 target;
    ColorResolutionOutcomeV1 color;
};

struct ResolvingOutcomeV1 final {
    bool succeeded = false;
    // Distinguishes an allocation/internal failure of the PNG color stage (which fails the attempt
    // AT ColorPreparing) from a target-preflight failure (which fails it at Resolving). A merely
    // non-Ready color resolution is neither: it travels in `resolved->color` to the analyzer.
    bool colorStageFailed = false;
    platform::StagedArtifactError error = platform::StagedArtifactError::None;
    std::shared_ptr<const ResolvedAttemptInputsV1> resolved = nullptr;
};
static_assert(runtime::TaskResultValue<ResolvingOutcomeV1>);

// Maps the C2 in-process registry's own closed outcome set onto the analyzer's closed PNG
// color-resolution input states, one to one, per frame-output.md's "The five ocio.* codes map
// one-to-one from the corresponding typed color-resolution failures". `Ready` is handled by the
// caller (it is the only outcome that carries a resolved product to build a processor from).
//
// `LocatorKindRequiresHelper` names a real, planned locator kind that this in-process registry
// never resolves; it is unreachable for the built-in URI this code always passes, and it maps to
// `MissingResource` (`ocio.resource-missing`) -- the honest "the configuration resource this build
// can reach does not cover that locator" state -- rather than being collapsed into `Missing`.
// `UnsupportedVersion` (`ocio.version-unsupported`) has no producer at all in version 1: the
// built-in payload's version is frozen with the binary, so nothing can present a newer one.
[[nodiscard]] output::PngRgba8SrgbColorResolutionStateV1
mapRegistryOutcome(const color::OcioBuiltInRegistryOutcome outcome) noexcept {
    switch (outcome) {
    case color::OcioBuiltInRegistryOutcome::Ready:
        break; // unreachable here; the caller only maps a non-Ready outcome.
    case color::OcioBuiltInRegistryOutcome::Missing:
        return output::PngRgba8SrgbColorResolutionStateV1::Missing;
    case color::OcioBuiltInRegistryOutcome::Changed:
        return output::PngRgba8SrgbColorResolutionStateV1::Changed;
    case color::OcioBuiltInRegistryOutcome::Invalid:
        return output::PngRgba8SrgbColorResolutionStateV1::Invalid;
    case color::OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper:
        return output::PngRgba8SrgbColorResolutionStateV1::MissingResource;
    }
    return output::PngRgba8SrgbColorResolutionStateV1::Missing;
}

// Binds a built or reused processor handle to the retained display-product triple. The identity is
// an ALIASING shared_ptr into the handle's own DisplayProcessorIdentityV1 member -- never an
// independently adopted copy -- so the exported identity can never be paired with a different
// processor (frame-output.md: "Recomputing pixel hashes or substituting an equivalent-looking
// frame or processor at approval or export is forbidden").
[[nodiscard]] std::optional<output::OutputAnalysisAttemptDisplayProductsV1>
retainDisplayProducts(std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle) {
    core::Sha256Digest expectedRevision;
    if (const auto view = handle->identity().borrowedView(); view.has_value()) {
        expectedRevision = view->expectedOcioRevision();
    } else {
        return std::nullopt;
    }
    std::shared_ptr<const color::DisplayProcessorIdentityV1> identity(handle, &handle->identity());
    return output::OutputAnalysisAttemptDisplayProductsV1{.processor = std::move(handle),
                                                          .identity = std::move(identity),
                                                          .expectedOcioRevision = expectedRevision};
}

// Prepares the immutable GPU DisplayRgba8 command from the exact resolved display processor, on
// this blocking CPU stage. Unavailable qualified tools or a preparation failure return null: the
// request then keeps the unchanged CPU display path. Never throws.
[[nodiscard]] std::shared_ptr<const runtime::PreparedGpuOcioCommand>
prepareGpuDisplayCommand(GpuExportProvider* const gpuProvider,
                         const color::ResolvedBloomNeutralConfig& config,
                         const runtime::GpuOcioCommandGeometry geometry,
                         const runtime::GpuOcioCancellation& cancel) noexcept {
    try {
        if (gpuProvider == nullptr || !gpuProvider->gpuDisplayPreparationAvailable()) {
            return nullptr;
        }
        auto prepared = gpuProvider->prepareGpuDisplayCommand(config, config.displayName(),
                                                              config.viewName(), geometry, cancel);
        return prepared.hasValue() ? prepared.command : nullptr;
    } catch (...) {
        return nullptr;
    }
}

// The attempt graph's step 4. Never throws; a genuine allocation/internal failure is signalled by
// returning nullopt so the caller can fail the attempt AT the ColorPreparing stage, while every
// modelled configuration/adapter state returns a populated outcome that the analyzer turns into a
// truthful, non-approvable report.
[[nodiscard]] std::optional<ColorResolutionOutcomeV1>
resolvePngDisplayProducts(runtime::QualifiedDisplayProcessorProvider* const provider,
                          GpuExportProvider* const gpuProvider,
                          const runtime::EvaluationColorIntent& intent,
                          const runtime::GpuOcioCommandGeometry geometry,
                          const runtime::GpuOcioCancellation& cancel) noexcept {
    try {
        const bool neutralWorkingSpace =
            intent.workingColorSpaceId == runtime::kLinearRec709SceneColorSpaceId &&
            (intent.ocioConfigRevision == core::Sha256Digest{} ||
             intent.ocioConfigRevision == color::kBloomNeutralV1ConfigDigest);
        const auto expectedRevision = intent.ocioConfigRevision == core::Sha256Digest{}
                                          ? color::kBloomNeutralV1ConfigDigest
                                          : intent.ocioConfigRevision;
        if (neutralWorkingSpace && provider != nullptr) {
            const auto snapshot = provider->snapshot();
            if (snapshot.readiness == runtime::QualifiedDisplayProcessorReadiness::Ready &&
                snapshot.handle != nullptr) {
                auto products = retainDisplayProducts(snapshot.handle);
                if (!products.has_value()) {
                    return std::nullopt;
                }
                // Prepare the GPU command from an independent resolution of the same exact config
                // (the ready CPU handle does not expose its config). A failure here is silent: the
                // CPU display path stays the truthful fallback.
                std::shared_ptr<const runtime::PreparedGpuOcioCommand> gpuCommand;
                auto gpuResolution = color::resolveOcioBuiltIn(
                    color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                    expectedRevision, intent.workingColorSpaceId);
                if (gpuResolution.ready()) {
                    if (auto gpuConfig = std::move(gpuResolution).takeResolved();
                        gpuConfig.has_value()) {
                        gpuCommand =
                            prepareGpuDisplayCommand(gpuProvider, *gpuConfig, geometry, cancel);
                    }
                }
                return ColorResolutionOutcomeV1{
                    .colorResolution = output::PngRgba8SrgbColorResolutionStateV1::Ready,
                    .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                    .display = std::move(*products),
                    .gpuDisplayCommand = std::move(gpuCommand)};
            }
        }

        const auto locator =
            neutralWorkingSpace ? color::kBloomNeutralV1ConfigUri : color::kAcesCgV1ConfigUri;
        auto resolution =
            color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn, locator,
                                      expectedRevision, intent.workingColorSpaceId);
        if (!resolution.ready()) {
            return ColorResolutionOutcomeV1{
                .colorResolution = mapRegistryOutcome(resolution.outcome()),
                .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                .display = {},
                .gpuDisplayCommand = {}};
        }
        auto resolved = std::move(resolution).takeResolved();
        if (!resolved.has_value()) {
            return std::nullopt;
        }

        std::shared_ptr<const runtime::PreparedGpuOcioCommand> gpuCommand =
            neutralWorkingSpace ? prepareGpuDisplayCommand(gpuProvider, *resolved, geometry, cancel)
                                : nullptr;

        // frame-output.md: "A resolved PNG configuration whose helper, processor, or execution
        // provider cannot run is an adapter-execution failure: Color keeps its Ready nominal tuple
        // while External Dependencies uses adapter.unavailable."
        auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
        auto handleValue = std::move(built).takeHandle();
        if (!handleValue.has_value()) {
            return ColorResolutionOutcomeV1{
                .colorResolution = output::PngRgba8SrgbColorResolutionStateV1::Ready,
                .adapter = output::OutputAnalysisAdapterStateV1::Unavailable,
                .display = {},
                .gpuDisplayCommand = {}};
        }
        auto handle = std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
            std::move(*handleValue));
        auto products = retainDisplayProducts(std::move(handle));
        if (!products.has_value()) {
            return std::nullopt;
        }
        return ColorResolutionOutcomeV1{.colorResolution =
                                            output::PngRgba8SrgbColorResolutionStateV1::Ready,
                                        .adapter = output::OutputAnalysisAdapterStateV1::Qualified,
                                        .display = std::move(*products),
                                        .gpuDisplayCommand = std::move(gpuCommand)};
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

enum class BuildFailureKindV1 : std::uint8_t {
    None,
    Evaluation,
    Identity,
    Analyzer,
    AttemptBuild,
};

// The successful Evaluating/Identifying/Analyzing product, heap-shared as one handle so the task
// result stays within TaskResultValue's 4-pointer cap even though the retained native provenance
// carries the full counter set.
struct BuildProductV1 final {
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt;
    OutputAnalysisAttemptGpuProvenanceV1 gpuProvenance;
};

struct BuildOutcomeV1 final {
    bool succeeded = false;
    BuildFailureKindV1 failureKind = BuildFailureKindV1::None;
    std::uint8_t rawCode = 0;
    std::shared_ptr<const BuildProductV1> product = nullptr;
};
static_assert(runtime::TaskResultValue<BuildOutcomeV1>);

[[nodiscard]] OutputAnalysisAttemptFailureV1
translateBuildFailure(const BuildOutcomeV1& outcome) noexcept {
    switch (outcome.failureKind) {
    case BuildFailureKindV1::Evaluation:
        return {OutputAnalysisAttemptStageV1::Evaluating, false,
                static_cast<runtime::EvaluationDiagnosticCode>(outcome.rawCode)};
    case BuildFailureKindV1::Identity:
        return {OutputAnalysisAttemptStageV1::Identifying, false,
                static_cast<output::ProcessFrameSemanticIdentityErrorCode>(outcome.rawCode)};
    case BuildFailureKindV1::Analyzer:
        return {OutputAnalysisAttemptStageV1::Analyzing, false,
                static_cast<output::OutputAnalysisAnalyzerErrorCodeV1>(outcome.rawCode)};
    case BuildFailureKindV1::AttemptBuild:
        return {OutputAnalysisAttemptStageV1::Analyzing, false,
                static_cast<output::OutputAnalysisAttemptErrorCodeV1>(outcome.rawCode)};
    case BuildFailureKindV1::None:
        break;
    }
    return {OutputAnalysisAttemptStageV1::Analyzing, false, std::monostate{}};
}

[[nodiscard]] runtime::TaskOwner attemptOwner(const runtime::TaskOwner requested) noexcept {
    return requested.isValid() ? requested
                               : runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                    .id = runtime::TaskOwnerId::fromRaw(1)};
}

} // namespace

OutputAnalysisAttemptOutcomeV1 OutputAnalysisAttemptOutcomeV1::completed(
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt,
    std::optional<OutputAnalysisAttemptGpuProvenanceV1> gpuProvenance) noexcept {
    OutputAnalysisAttemptOutcomeV1 outcome;
    outcome.attempt_ = std::move(attempt);
    outcome.gpuProvenance_ = std::move(gpuProvenance);
    return outcome;
}

OutputAnalysisAttemptOutcomeV1
OutputAnalysisAttemptOutcomeV1::failure(OutputAnalysisAttemptFailureV1 failureValue) noexcept {
    OutputAnalysisAttemptOutcomeV1 outcome;
    outcome.failure_.emplace(failureValue); // trivially copyable; std::move() would be a no-op
    return outcome;
}

namespace detail {

struct OutputAnalysisAttemptRunnerState final {
    runtime::TaskScheduler* scheduler = nullptr;
    platform::StagedArtifactCoordinator* artifacts = nullptr;
    output::ExportResourceLedgerV1* ledger = nullptr;
    OutputAnalysisAttemptRequestV1 request;
    OutputAnalysisAttemptStageV1 stage = OutputAnalysisAttemptStageV1::Resolving;
    runtime::TaskHandle<ResolvingOutcomeV1> resolvingHandle;
    std::optional<runtime::TaskHandle<BuildOutcomeV1>> buildHandle;
    // Set once Resolving succeeds and cleared when the Cpu evaluation task is submitted. While it
    // is set, the runner may be deferring on the GPU provider's lazy bootstrap.
    std::shared_ptr<const ResolvedAttemptInputsV1> resolvedInputs;
    std::optional<std::chrono::steady_clock::time_point> gpuGateDeadline;
    bool completed = false;
};

// Bounded deferral for the GPU provider's lazy bootstrap. The gate is owner-driven: tryComplete()
// simply reports "not yet" while the provider boots and the authoring surface's existing poll
// loop re-drives it. A provider that never reaches a terminal state cannot wedge an export
// forever, so after this bound the evaluation proceeds and the truthful CPU fallback runs.
constexpr auto kGpuBootstrapDeferralLimit = std::chrono::seconds(5);

} // namespace detail

OutputAnalysisAttemptRunnerV1::OutputAnalysisAttemptRunnerV1(
    OutputAnalysisAttemptRunnerV1&&) noexcept = default;
OutputAnalysisAttemptRunnerV1&
OutputAnalysisAttemptRunnerV1::operator=(OutputAnalysisAttemptRunnerV1&&) noexcept = default;

OutputAnalysisAttemptRunnerV1::OutputAnalysisAttemptRunnerV1(
    std::unique_ptr<detail::OutputAnalysisAttemptRunnerState> state) noexcept
    : state_(std::move(state)) {}

OutputAnalysisAttemptRunnerV1::~OutputAnalysisAttemptRunnerV1() {
    if (state_ && !state_->completed) {
        requestCancellation();
    }
}

bool OutputAnalysisAttemptRunnerV1::isReady() const noexcept {
    if (!state_ || state_->completed || state_->scheduler == nullptr) {
        return false;
    }
    const auto id =
        state_->buildHandle.has_value() ? state_->buildHandle->id() : state_->resolvingHandle.id();
    const auto snapshot = state_->scheduler->snapshot(id);
    return snapshot.has_value() && runtime::isTerminal(snapshot->state);
}

void OutputAnalysisAttemptRunnerV1::requestCancellation() noexcept {
    if (!state_) {
        return;
    }
    if (state_->buildHandle.has_value()) {
        state_->buildHandle->cancel();
    } else {
        state_->resolvingHandle.cancel();
    }
}

std::optional<OutputAnalysisAttemptOutcomeV1> OutputAnalysisAttemptRunnerV1::tryComplete() {
    if (!state_ || state_->completed) {
        return std::nullopt;
    }

    if (!state_->buildHandle.has_value()) {
        if (state_->resolvedInputs == nullptr) {
            // Still waiting on / just reached the Resolving stage.
            auto taken = state_->resolvingHandle.tryTakeResult();
            if (!taken.has_value()) {
                return std::nullopt;
            }
            if (taken->state() == runtime::TaskState::Cancelled) {
                state_->completed = true;
                return OutputAnalysisAttemptOutcomeV1::failure(
                    {OutputAnalysisAttemptStageV1::Resolving, true, std::monostate{}});
            }
            if (taken->state() != runtime::TaskState::Succeeded || !taken->value().has_value() ||
                !taken->value()->succeeded) {
                state_->completed = true;
                if (taken->value().has_value() && taken->value()->colorStageFailed) {
                    return OutputAnalysisAttemptOutcomeV1::failure(
                        {OutputAnalysisAttemptStageV1::ColorPreparing, false, std::monostate{}});
                }
                const auto error = taken->value().has_value() ? taken->value()->error
                                                              : platform::StagedArtifactError::None;
                return OutputAnalysisAttemptOutcomeV1::failure(
                    {OutputAnalysisAttemptStageV1::Resolving, false, error});
            }
            // Resolving succeeded: retain its typed result (the target, and for PNG the retained
            // display products) until the evaluation stage is actually submitted -- possibly after
            // a bounded GPU-bootstrap deferral -- then consume it directly into the next task's
            // closure, never a wait/get/join.
            state_->resolvedInputs = taken->value()->resolved;
            if (state_->resolvedInputs == nullptr) {
                state_->completed = true;
                return OutputAnalysisAttemptOutcomeV1::failure(
                    {OutputAnalysisAttemptStageV1::Resolving, false, std::monostate{}});
            }
        }

        // GPU bootstrap gate: defer the evaluation stage until the provider's lazy bootstrap is
        // terminal, so the first real export on a supported device is genuinely GPU instead of a
        // CPU fallback that raced the bootstrap. This is owner-driven -- tryComplete() simply
        // reports "not yet" and the authoring surface's existing poll loop re-drives it -- so no
        // worker is blocked and no CPU task is submitted while waiting. The bound keeps a wedged
        // provider from stalling an export forever.
        const auto& provider = state_->request.gpuProvider;
        if (provider != nullptr && !provider->prepared()) {
            if (!state_->gpuGateDeadline.has_value()) {
                state_->gpuGateDeadline =
                    std::chrono::steady_clock::now() + detail::kGpuBootstrapDeferralLimit;
            }
            if (std::chrono::steady_clock::now() < *state_->gpuGateDeadline) {
                return std::nullopt;
            }
        }

        auto resolved = std::move(state_->resolvedInputs);
        state_->resolvedInputs.reset();
        state_->gpuGateDeadline.reset();
        const auto preset = state_->request.preset;
        auto* ledger = state_->ledger;
        auto gpuProvider = state_->request.gpuProvider; // shared ownership travels with the task
        // The command the combined readback will run. The canonical command was prepared on the
        // Resolving CPU task from the EXACT resolved config/working space/display/view and the exact
        // data-window geometry. `request.outputColorCommand` is an explicit test seam and is
        // accepted ONLY when it byte-identifies that canonical command; any other command (wrong
        // transform, stale config revision, or a command prepared for a different frame) is
        // refused, and the attempt falls back honestly to the CPU reference/display path instead of
        // laundering foreign pixels under the canonical display identity.
        const auto& canonicalCommand = resolved->color.gpuDisplayCommand;
        const auto& requestedCommand = state_->request.outputColorCommand;
        const bool refusedRequestedCommand =
            requestedCommand != nullptr &&
            (canonicalCommand == nullptr ||
             requestedCommand->identity() != canonicalCommand->identity());
        const bool forceCpuFallback = refusedRequestedCommand;
        auto outputColorCommand = refusedRequestedCommand
                                      ? nullptr
                                      : (requestedCommand != nullptr ? requestedCommand
                                                                     : canonicalCommand);
        auto plan = state_->request.plan;
        auto evaluation = state_->request.evaluation;

        runtime::TaskRequest cpuRequest(
            "Analyze a frame export attempt", attemptOwner(state_->request.owner),
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);
        auto submission = state_->scheduler->submit<BuildOutcomeV1>(
            std::move(cpuRequest),
            [plan = std::move(plan), evaluation, resolved, preset, ledger,
             gpuProvider = std::move(gpuProvider), forceCpuFallback,
             outputColorCommand = std::move(outputColorCommand)](
                runtime::TaskContext& context) -> runtime::TaskResult<BuildOutcomeV1> {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress(
                    {.phase = "Evaluating", .subphase = "", .completed = 0, .total = std::nullopt});
                // GPU final-render bridge: when an available device and a prepared-GPU-subset scene
                // exist, evaluate through the genuine native scene executor and read the final
                // RGBA32F back exactly once. An unavailable device, a disabled provider, or a
                // scene outside the subset falls through to the CPU reference evaluator on the
                // same snapshot/identity. The typed native provenance/counters are retained for
                // diagnostics; they never alter the analysis or the approval digest. A refused
                // requested command forces the full CPU reference path: the identity arm must never
                // silently substitute for the display transform the caller asked to run.
                OutputAnalysisAttemptGpuProvenanceV1 gpuProvenance;
                output::OutputAnalysisAttemptGpuDisplayV1 gpuDisplay;
                std::shared_ptr<const runtime::ProcessFrame> evaluatedFrame;
                if (gpuProvider != nullptr && !forceCpuFallback) {
                    auto evaluator = gpuProvider->evaluator();
                    if (evaluator != nullptr) {
                        auto gpuOutcome = evaluator->evaluate(
                            plan, evaluation, context.cancellation(), {}, outputColorCommand);
                        gpuProvenance.status = gpuOutcome.status;
                        gpuProvenance.counters = gpuOutcome.counters;
                        gpuProvenance.deviceOwnershipEpoch = gpuOutcome.deviceOwnershipEpoch;
                        gpuProvenance.encodedArm = gpuOutcome.encodedArm;
                        gpuProvenance.readbackSubmissions =
                            gpuOutcome.outputColorCounters.readbackSubmissions;
                        gpuProvenance.transferredPayloads =
                            gpuOutcome.outputColorCounters.transferredPayloads;
                        gpuProvenance.processPayloadBytes =
                            gpuOutcome.outputColorCounters.processPayloadBytes;
                        gpuProvenance.encodedPayloadBytes =
                            gpuOutcome.outputColorCounters.encodedPayloadBytes;
                        gpuProvenance.outputCommandIdentity = gpuOutcome.outputCommandIdentity;
                        if (gpuOutcome.status == runtime::GpuProcessFrameStatus::Evaluated) {
                            evaluatedFrame = gpuOutcome.frame;
                            // Retain the verified display payload, bound to the exact process
                            // geometry and the SAME canonical display identity the attempt retains.
                            const auto* descriptor =
                                gpuOutcome.frame != nullptr
                                    ? gpuOutcome.frame->processImage().descriptor()
                                    : nullptr;
                            if (gpuOutcome.encodedArm == runtime::GpuOutputColorArm::DisplayRgba8 &&
                                !gpuOutcome.encodedDisplayRgba8.empty() && descriptor != nullptr &&
                                resolved->color.display.identity != nullptr) {
                                const auto extent = descriptor->dataWindow().extent();
                                if (static_cast<std::size_t>(extent.width()) * extent.height() ==
                                    gpuOutcome.encodedDisplayRgba8.size()) {
                                    gpuDisplay.pixels = std::move(gpuOutcome.encodedDisplayRgba8);
                                    gpuDisplay.width = extent.width();
                                    gpuDisplay.height = extent.height();
                                    gpuDisplay.commandIdentity = gpuOutcome.outputCommandIdentity;
                                    gpuDisplay.displayIdentity = resolved->color.display.identity;
                                }
                            }
                        }
                    } else {
                        gpuProvenance.status = runtime::GpuProcessFrameStatus::DeviceUnavailable;
                    }
                }
                if (evaluatedFrame == nullptr) {
                    const runtime::CpuCompositionEvaluator evaluator;
                    auto evalResult = evaluator.evaluate(plan, evaluation, context.cancellation());
                    if (evalResult.status() == runtime::EvaluationStatus::Cancelled) {
                        return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                    }
                    if (evalResult.status() != runtime::EvaluationStatus::Evaluated ||
                        evalResult.frame() == nullptr) {
                        const auto code = evalResult.diagnostics().empty()
                                              ? runtime::EvaluationDiagnosticCode::InternalInvariant
                                              : evalResult.diagnostics().front().code;
                        return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                            {.succeeded = false,
                             .failureKind = BuildFailureKindV1::Evaluation,
                             .rawCode = static_cast<std::uint8_t>(code)});
                    }
                    evaluatedFrame = evalResult.frame();
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress({.phase = "Identifying",
                                        .subphase = "",
                                        .completed = 0,
                                        .total = std::nullopt});
                const output::ProcessFrameSemanticIdentityV1Preparer identityPreparer;
                auto identityResult =
                    identityPreparer.prepare(evaluatedFrame, context.cancellation());
                if (identityResult.status() ==
                    output::ProcessFrameSemanticIdentityPreparationStatus::Cancelled) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }
                if (identityResult.status() !=
                        output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
                    identityResult.identity() == nullptr) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::Identity,
                         .rawCode = static_cast<std::uint8_t>(identityResult.error())});
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress(
                    {.phase = "Analyzing", .subphase = "", .completed = 0, .total = std::nullopt});
                const output::OutputAnalysisProcessSourceV1 processSource{
                    .state = output::OutputAnalysisProcessSourceStateV1::Ready,
                    .readyIdentity = identityResult.identity(),
                    .missingDescriptor = std::nullopt};
                // The preset-specific analyzer entry points -- never a preset enum plus a union of
                // optional fields (frame-output.md: "There is no public entry point that accepts a
                // preset enum plus a union of optional fields"). The PNG input's expected OCIO
                // revision is the persisted Bloom Neutral v1 expectedRevision, required whether or
                // not resolution succeeded; when resolution DID succeed it byte-equals the revision
                // embedded in the retained canonical DisplayProcessorIdentity, which the digest
                // stage independently re-checks.
                output::OutputAnalysisAnalyzerResultV1 analyzed =
                    preset == output::OutputPresetV1::PngRgba8SrgbV1
                        ? output::analyzePngRgba8SrgbV1(
                              {.process = processSource,
                               .expectedOcioRevision =
                                   evaluation.colorIntent.ocioConfigRevision == core::Sha256Digest{}
                                       ? color::kBloomNeutralV1ConfigDigest
                                       : evaluation.colorIntent.ocioConfigRevision,
                               .colorResolution = resolved->color.colorResolution,
                               .adapter = resolved->color.adapter})
                    : preset == output::OutputPresetV1::TiffRgba16SrgbV1
                        ? output::analyzeTiffRgba16SrgbV1(
                              {.process = processSource,
                               .adapter = output::outputPresetAvailabilityV1(
                                              output::OutputPresetV1::TiffRgba16SrgbV1)
                                                  .available
                                              ? output::OutputAnalysisAdapterStateV1::Qualified
                                              : output::OutputAnalysisAdapterStateV1::Unavailable})
                        : output::analyzeFlatExrRgba32fLinRec709SceneV1({.process = processSource});
                if (!analyzed.hasReport()) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::Analyzer,
                         .rawCode = static_cast<std::uint8_t>(analyzed.error())});
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                // Bind the retained encoded display product to the exact process-pixel identity of
                // the frame it was transferred with; buildOutputAnalysisAttemptV1 re-validates the
                // pairing, so a cross-frame payload can never be retained.
                if (gpuDisplay.isPresent()) {
                    gpuDisplay.processPixelDigest = identityResult.identity()->processPixelDigest();
                }

                auto buildResult = output::buildOutputAnalysisAttemptV1(
                    {.frame = evaluatedFrame,
                     .processIdentity = identityResult.identity(),
                     .report = analyzed.report(),
                     .target = resolved->target,
                     .display = resolved->color.display,
                     .gpuDisplay = std::move(gpuDisplay)},
                    *ledger);
                if (!buildResult) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::AttemptBuild,
                         .rawCode = static_cast<std::uint8_t>(buildResult.error())});
                }
                return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                    {.succeeded = true,
                     .product = std::make_shared<const BuildProductV1>(
                         BuildProductV1{buildResult.attempt(), gpuProvenance})});
            });

        if (!submission.accepted()) {
            state_->completed = true;
            return OutputAnalysisAttemptOutcomeV1::failure(
                {OutputAnalysisAttemptStageV1::Evaluating, false, submission.status});
        }
        state_->buildHandle.emplace(std::move(submission.handle));
        return std::nullopt;
    }

    auto taken = state_->buildHandle->tryTakeResult();
    if (!taken.has_value()) {
        return std::nullopt;
    }
    state_->completed = true;
    if (taken->state() == runtime::TaskState::Cancelled) {
        return OutputAnalysisAttemptOutcomeV1::failure(
            {OutputAnalysisAttemptStageV1::Evaluating, true, std::monostate{}});
    }
    const auto& product = taken->value()->product;
    if (taken->state() != runtime::TaskState::Succeeded || !taken->value().has_value() ||
        !taken->value()->succeeded || product == nullptr || product->attempt == nullptr) {
        if (taken->value().has_value()) {
            return OutputAnalysisAttemptOutcomeV1::failure(translateBuildFailure(*taken->value()));
        }
        return OutputAnalysisAttemptOutcomeV1::failure(
            {OutputAnalysisAttemptStageV1::Analyzing, false, std::monostate{}});
    }
    return OutputAnalysisAttemptOutcomeV1::completed(product->attempt, product->gpuProvenance);
}

OutputAnalysisAttemptRunnerResultV1::OutputAnalysisAttemptRunnerResultV1(
    const runtime::TaskSubmissionStatus status) noexcept
    : submissionFailure_(status) {}

OutputAnalysisAttemptRunnerResultV1::OutputAnalysisAttemptRunnerResultV1(
    OutputAnalysisAttemptRunnerV1 handle) noexcept
    : handle_(std::move(handle)) {}

OutputAnalysisAttemptRunnerV1 OutputAnalysisAttemptRunnerResultV1::takeHandle() && noexcept {
    if (!handle_.has_value()) {
        std::terminate();
    }
    return std::move(*handle_);
}

OutputAnalysisAttemptRunnerResultV1 beginOutputAnalysisAttemptV1(
    runtime::TaskScheduler& scheduler, platform::StagedArtifactCoordinator& artifacts,
    output::ExportResourceLedgerV1& ledger, OutputAnalysisAttemptRequestV1 request) {
    const auto targetPath = request.targetPath;
    const auto overwritePolicy = request.overwritePolicy;
    // Direct aggregate-brace construction (never T's own implicit default constructor, which
    // std::make_unique<T>() would call with zero arguments): OutputAnalysisAttemptRequestV1 has no
    // default constructor of its own (runtime::EvaluationRequest's OperationIndex member has none
    // either), so `request` -- the one field with no usable default -- must be supplied inline
    // here rather than default-constructed then assigned.
    auto state = std::unique_ptr<detail::OutputAnalysisAttemptRunnerState>(
        new detail::OutputAnalysisAttemptRunnerState{
            .scheduler = &scheduler,
            .artifacts = &artifacts,
            .ledger = &ledger,
            .request = std::move(request),
            .stage = OutputAnalysisAttemptStageV1::Resolving,
            .resolvingHandle = {},
            .buildHandle = std::nullopt,
            .resolvedInputs = nullptr,
            .gpuGateDeadline = std::nullopt,
            .completed = false,
        });

    const auto preset = state->request.preset;
    auto* const displayProvider = state->request.displayProcessorProvider;
    auto* const gpuProvider = state->request.gpuProvider.get();
    const auto colorIntent = state->request.evaluation.colorIntent;
    const runtime::GpuOcioCommandGeometry colorGeometry{
        .width = state->request.plan != nullptr ? state->request.plan->format().width() : 0,
        .height = state->request.plan != nullptr ? state->request.plan->format().height() : 0};

    runtime::TaskRequest resolvingRequest(
        "Resolve an export target", attemptOwner(state->request.owner),
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);
    auto submission = scheduler.submit<ResolvingOutcomeV1>(
        std::move(resolvingRequest),
        [&artifacts, targetPath, overwritePolicy, preset, displayProvider, gpuProvider, colorIntent,
         colorGeometry](runtime::TaskContext& context) -> runtime::TaskResult<ResolvingOutcomeV1> {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<ResolvingOutcomeV1>::cancelled();
            }
            context.reportProgress(
                {.phase = "Resolving", .subphase = "", .completed = 0, .total = std::nullopt});
            auto preflightResult = artifacts.preflight({.targetPath = targetPath,
                                                        .overwritePolicy = overwritePolicy,
                                                        .expectedTarget = std::nullopt});
            if (!preflightResult) {
                return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                    {.succeeded = false, .error = preflightResult.error()});
            }
            auto target = std::move(preflightResult).takeTarget();
            const auto targetKey = target.targetKey();
            const auto observation = target.observation();
            // `target` goes out of scope here: the live platform active-target admission it holds
            // is released immediately rather than kept open for the duration of a pending artist
            // decision (see this file's own header-level rationale comment).
            ResolvedAttemptInputsV1 inputs{.target = {.targetKey = targetKey,
                                                      .observation = observation,
                                                      .targetPath = targetPath,
                                                      .overwritePolicy = overwritePolicy},
                                           .color = {}};

            // EXR has no display product and therefore never enters ColorPreparing at all.
            if (preset == output::OutputPresetV1::PngRgba8SrgbV1) {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<ResolvingOutcomeV1>::cancelled();
                }
                context.reportProgress({.phase = "ColorPreparing",
                                        .subphase = "",
                                        .completed = 0,
                                        .total = std::nullopt});
                auto color = resolvePngDisplayProducts(
                    displayProvider, gpuProvider, colorIntent, colorGeometry,
                    [&context] { return context.isCancellationRequested(); });
                if (!color.has_value()) {
                    return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                        {.succeeded = false, .colorStageFailed = true});
                }
                inputs.color = std::move(*color);
            }

            std::shared_ptr<const ResolvedAttemptInputsV1> retained;
            try {
                retained = std::make_shared<const ResolvedAttemptInputsV1>(std::move(inputs));
            } catch (const std::bad_alloc&) {
                return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                    {.succeeded = false,
                     .error = platform::StagedArtifactError::ResourceUnavailable});
            }
            return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                {.succeeded = true, .resolved = std::move(retained)});
        });

    if (!submission.accepted()) {
        return OutputAnalysisAttemptRunnerResultV1(submission.status);
    }
    state->resolvingHandle = std::move(submission.handle);
    return OutputAnalysisAttemptRunnerResultV1(OutputAnalysisAttemptRunnerV1(std::move(state)));
}

} // namespace bloom::host
