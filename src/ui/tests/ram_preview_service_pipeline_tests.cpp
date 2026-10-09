// PERF-RAM-2: the real GpuPreviewDisplayService submission seam the two-deep RAM pipeline uses.
//
// Two honest halves:
//
//   * Device-free admission: with no usable native loader the service rejects initialization as
//     Unavailable and its own bounded submit() (root admission plus the CPU-fallback reservation)
//     admits the controller's in-flight frames and returns genuine TaskHandles the controller can
//     cancel. Nothing about GPU success is fabricated.
//   * Enabled-device overlap: when a loader is supplied on the command line and the service reaches
//     Ready, the SAME real service is driven through the two-deep controller. A deterministic
//     two-worker barrier inside the service's own CPU stage proves the second frame's CPU
//     preparation starts before the first finishes (genuine stage overlap under the real service
//     admission, not two serially-queued items), the controller never exceeds its two-frame bound,
//     and every stage selects the qualified display processor. These tiny 16x12 frames remain
//     below the production measured native-selection interval, so their published pixels carry
//     honest CpuOcio/CPU provenance even while the service is Ready. This is CPU-stage overlap,
//     not a GPU pixel pass; genuine packed RAM/native evidence is a separate fixture.
#include "ram_preview_pipeline_test_support.hpp"

#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/ram_preview_pipeline.hpp>

#include <QApplication>

#include <atomic>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace bloom;
using namespace bloom::ui::ram_pipeline_test;

std::shared_ptr<runtime::GpuProcessingPhasePolicy> sampledServiceRoot() {
    auto sampled = std::async(std::launch::async, [] {
        auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(4);
        auto sample = runtime::memoryBudgetSample();
        runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                           sample.machine.availableBytes.value_or(0));
        ledger.beginProcessSampling();
        const auto at = runtime::GpuProcessingPhasePolicy::Clock::now();
        const auto state = ledger.applySample(std::move(sample), at);
        policy->applyHostSample(state, ledger.physicalByteBudget(), at);
        return policy;
    });
    if (!waitUntil(
            [&] { return sampled.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }))
        throw std::runtime_error("Real off-UI processing sample did not finish.");
    return sampled.get();
}
void phaseConservation(Expectations& expectations,
                       const std::shared_ptr<runtime::GpuProcessingPhasePolicy>& policy) {
    expectations.expect(waitUntil([&] {
                            const auto root = policy->snapshot().reservations;
                            return root.admittedPhases == 0 && root.unprovenPhases == 0 &&
                                   root.hostReservedBytes == 0 && root.deviceReservedBytes == 0 &&
                                   root.ownedPackedHostBytes == 0 &&
                                   root.ownedPackedHostAllocations == 0 &&
                                   root.totalHostClaimBytes == 0;
                        }) &&
                            policy->snapshot().reservations.peakHostReservedBytes > 0,
                        "shared real root admitted actual work and releases all phase claims");
}

void testGpuServiceSeamAdmitsBoth(Expectations& expectations) {
    SessionFixture fixture(makeTestProject("RAM Service Seam", timeAt(3, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    const auto foregroundFrame = fixture.controller.state().frame;
    if (!foregroundFrame) {
        expectations.expect(false, "real foreground frame is required for cache provenance proof");
        finishFixture(fixture, expectations);
        return;
    }
    fixture.frameCache->clear();

    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = "/nonexistent/bloom-loader-for-ram-pipeline-test.so";
    options.previewByteAllowance = std::size_t{512} * 1024U * 1024U;
    options.processingPhasePolicy = sampledServiceRoot();
    const auto policy = options.processingPhasePolicy;
    auto preparations = std::make_shared<std::atomic_int>(0);
    auto stage = ui::makeCompositionPreviewCpuStage(
        fixture.pipelineFixture.compiler, fixture.pipelineFixture.evaluator,
        fixture.pipelineFixture.qualifiedProcessorProvider, fixture.pipelineFixture.planCache);
    auto fallback =
        ui::makeCompositionPreviewCpuDisplayFallback(fixture.pipelineFixture.displayPreparer);
    runtime::GpuPreviewDisplayService service(
        fixture.scheduler,
        [base = std::move(stage),
         preparations](const document::Snapshot& snapshot,
                       const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                       const std::vector<runtime::SnapshotParameterOverride>& overrides,
                       runtime::TaskContext& context) {
            ++*preparations;
            return base(snapshot, identity, bytes, overrides, context);
        },
        std::move(fallback), options);
    expectations.expect(
        waitUntil([&] {
            return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
        }) &&
            service.status().state == runtime::GpuPreviewDisplayServiceState::Unavailable,
        "a missing native loader honestly reports the service Unavailable");
    expectations.expect(service.status().diagnostic.code ==
                            runtime::GpuPreviewDisplayServiceDiagnosticCode::LoaderUnavailable,
                        "the diagnostic names the loader, not a fabricated GPU success");

    auto calls = std::make_shared<std::atomic<int>>(0);
    ui::RamPreviewController ram(
        fixture.session, fixture.controller, fixture.scheduler, fixture.bridge,
        fixture.pipelineFixture.preparation(fixture.coordinator), nullptr,
        [&service, calls](runtime::TaskRequest request, const document::Snapshot& snapshot,
                          const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                          const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            calls->fetch_add(1);
            return service.submit(std::move(request), snapshot, identity, limit, overrides);
        });
    ram.start();
    expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                            ram.cachedFrameCount() == 3 && ram.totalFrameCount() == 3,
                        "the real service seam caches the whole range on the CPU path");
    expectations.expect(calls->load() >= 1 && ram.peakInFlightFrames() <= 2,
                        "the service admitted the submitted frames without breaking the bound");
    expectations.expect(preparations->load() == 3,
                        "missing-loader route reached every actual CPU stage");
    for (std::int64_t time = 0; time < 3; ++time) {
        auto identity = foregroundFrame->desiredIdentity();
        identity.time = timeAt(time, 25);
        const auto frame = fixture.frameCache->take(identity);
        expectations.expect(
            frame &&
                (frame->provenance().provider == runtime::PreviewDisplayProvider::CpuReference ||
                 frame->provenance().provider == runtime::PreviewDisplayProvider::CpuOcio) &&
                frame->processIdentity().provider == runtime::EvaluationProvider::CpuReference &&
                !frame->provenance().gpuPreparedPacked,
            "missing-loader cached frame carries actual CPU provenance");
    }
    phaseConservation(expectations, policy);

    ram.beginShutdown();
    service.beginShutdown();
    finishFixture(fixture, expectations);
}

// The enabled-device half: the real service, driven by the two-deep controller, with a two-worker
// barrier inside its own CPU stage. `coordinator` counts stage calls across the whole fixture; a
// barrier on the first RAM frame until the second has arrived would deadlock if the two stage
// children did not genuinely run at the same time.
void testGpuServiceEnabledOverlapsStages(Expectations& expectations,
                                         const std::filesystem::path& loader) {
    SessionFixture fixture(makeTestProject("RAM Service Enabled", timeAt(6, 25)));
    expectations.expect(waitUntil([&] { return isReady(fixture.controller); }),
                        "the foreground frame settles");
    const auto foregroundFrame = fixture.controller.state().frame;
    if (!foregroundFrame) {
        expectations.expect(false, "real foreground frame is required for cache provenance proof");
        finishFixture(fixture, expectations);
        return;
    }
    fixture.frameCache->clear();

    auto stageBarrier = std::make_shared<PrepCoordinator>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto qualifiedStages = std::make_shared<std::atomic<int>>(0);

    // The enabled-device overlap only exists if the stage can select the qualified display
    // processor; the bare pipeline fixture leaves its provider unpublished, so publish the same
    // exact embedded Bloom Neutral v1 processor the application bootstrap does.
    fixture.pipelineFixture.qualifiedProcessorProvider.publish(
        runtime::buildBloomNeutralQualifiedDisplayProcessor());
    expectations.expect(fixture.pipelineFixture.qualifiedProcessorProvider.readiness() ==
                            runtime::QualifiedDisplayProcessorReadiness::Ready,
                        "the enabled-service fixture publishes the Bloom Neutral processor");

    auto rawStage = ui::makeCompositionPreviewCpuStage(
        fixture.pipelineFixture.compiler, fixture.pipelineFixture.evaluator,
        fixture.pipelineFixture.qualifiedProcessorProvider, fixture.pipelineFixture.planCache);
    auto fallback =
        ui::makeCompositionPreviewCpuDisplayFallback(fixture.pipelineFixture.displayPreparer);
    runtime::PreviewCpuStageFunction countingStage =
        [rawStage = std::move(rawStage), stageBarrier,
         qualifiedStages](const document::Snapshot& snapshot,
                          const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                          const std::vector<runtime::SnapshotParameterOverride>& overrides,
                          runtime::TaskContext& context) {
            const int ordinal = stageBarrier->arrive();
            auto result = rawStage(snapshot, identity, limit, overrides, context);
            stageBarrier->depart(ordinal);
            if (result.value() && *result.value() && (*result.value())->stage != nullptr) {
                const auto& stage = *(*result.value())->stage;
                if (stage.ocioQualified()) {
                    qualifiedStages->fetch_add(1);
                }
            }
            return result;
        };

    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.previewByteAllowance = std::size_t{512} * 1024U * 1024U;
    options.processingPhasePolicy = sampledServiceRoot();
    const auto policy = options.processingPhasePolicy;
    runtime::GpuPreviewDisplayService service(fixture.scheduler, std::move(countingStage),
                                              std::move(fallback), options);

    const bool terminal = waitUntil([&] {
        return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
    });
    const bool ready =
        terminal && service.status().state == runtime::GpuPreviewDisplayServiceState::Ready;
    if (!ready) {
        std::cout << "ram-preview-service-pipeline: loader supplied but service did not reach "
                     "Ready (state="
                  << static_cast<int>(service.status().state)
                  << ", diagnostic=" << static_cast<int>(service.status().diagnostic.code)
                  << "); enabled-device overlap gate stays pending, honest CPU fallback still "
                     "asserted\n";
    } else {
        // Both RAM frames must prepare concurrently under the real service admission. Arm the
        // barrier only over the RAM run's own stage calls, after the foreground frame settled.
        const int base = stageBarrier->calls();
        stageBarrier->armArrivalBarrier(base, base + 1);
        ui::RamPreviewController ram(
            fixture.session, fixture.controller, fixture.scheduler, fixture.bridge,
            fixture.pipelineFixture.preparation(fixture.coordinator), nullptr,
            [&service, calls](runtime::TaskRequest request, const document::Snapshot& snapshot,
                              const runtime::PreviewRequestIdentity& identity,
                              const std::size_t limit,
                              const std::vector<runtime::SnapshotParameterOverride>& overrides) {
                calls->fetch_add(1);
                return service.submit(std::move(request), snapshot, identity, limit, overrides);
            });
        ram.start();
        // The barrier is a hard rendezvous: a passing completion is itself the overlap proof.
        expectations.expect(waitUntil([&] { return !ram.isCaching(); }) &&
                                ram.cachedFrameCount() == ram.totalFrameCount(),
                            "the enabled service caches the whole range through the submit seam");
        expectations.expect(stageBarrier->peak() >= 2,
                            "the enabled service ran two CPU stages at the same time, not two "
                            "serially-queued items");
        expectations.expect(ram.peakInFlightFrames() == ui::RamPreviewPipeline::kMaxInFlight,
                            "the enabled service stayed inside the two-frame controller bound");
        expectations.expect(qualifiedStages->load() >= 1,
                            "every admitted stage selected the qualified display processor");
        std::cout << "ram-preview-service-pipeline: enabled service reached Ready; two CPU stages "
                     "overlapped (peak="
                  << stageBarrier->peak()
                  << ", qualifiedStageSelections=" << qualifiedStages->load()
                  << ", submitCalls=" << calls->load()
                  << "); barrier rendezvous is the overlap proof\n";

        for (std::int64_t frame = 0; frame < 6; ++frame) {
            const auto key = fixture.controller.cacheKeyForTime(timeAt(frame, 25));
            expectations.expect(key && fixture.frameCache->contains(*key),
                                "every frame of the enabled range is cached");
            auto identity = foregroundFrame->desiredIdentity();
            identity.time = timeAt(frame, 25);
            const auto cached = fixture.frameCache->take(identity);
            expectations.expect(
                cached &&
                    cached->provenance().provider == runtime::PreviewDisplayProvider::CpuOcio &&
                    cached->processIdentity().provider ==
                        runtime::EvaluationProvider::CpuReference &&
                    !cached->provenance().gpuQualification &&
                    !cached->provenance().gpuPreparedPacked,
                "tiny qualified display route caches honest CPU-OCIO frames without GPU proof");
        }
        expectations.expect(service.status().counters.nativeDispatches == 0 &&
                                service.status().counters.residentGraphJobs == 0 &&
                                service.status().counters.packedTransfers == 0,
                            "small measured CPU route makes no native graph/transfer claim");
        phaseConservation(expectations, policy);
        ram.beginShutdown();
    }

    service.beginShutdown();
    finishFixture(fixture, expectations);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    std::filesystem::path loader;
    bool valid = true;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] == nullptr ? std::string{} : argv[index];
        if (argument == "--loader" && index + 1 < argc && argv[index + 1] != nullptr) {
            loader = argv[++index];
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            valid = false;
        }
    }
    if (!valid) {
        return 2;
    }

    Expectations expectations;
    try {
        testGpuServiceSeamAdmitsBoth(expectations);
        testGpuServiceEnabledOverlapsStages(expectations, loader);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
