#pragma once

// Native execution helper for the bounded GPU coverage gate. It runs a real PreparedGpuScene
// through the production GpuSceneExecutor on a real device and compares the resident output against
// the CpuCompositionEvaluator oracle, including the actual descriptor window, origin, and pixel
// aspect. Nothing here fakes a dispatch or a readback: the counters are the executor's own.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_path_coverage.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace bloom::gpu_coverage_native {

enum class NativeOperationFamily : std::uint8_t {
    Solid,
    CoveredSolid,
    Translation,
    SourceOver,
    Blend,
    Affine,
    Upload,
    None,
};

struct NativeFixtureOutcome final {
    bool ran = false;
    bool passed = false;
    // Set when the executor refused the scene or could not reach the device: the route fell back to
    // a CPU whole-frame render and must never be counted as a GPU pass.
    bool cpuFallback = false;
    NativeOperationFamily family = NativeOperationFamily::None;
    std::uint64_t coldDispatches = 0;
    std::uint64_t familyDispatches = 0;
    std::uint64_t warmDispatches = 0;
    std::uint64_t warmCacheHits = 0;
    std::uint64_t readbacks = 0;
    // Set when the scene carries a PathRaster vector coverage command. The required evidence is a
    // real GpuPathCoverage compute dispatch, not a consumed host mask.
    bool coverageRequired = false;
    std::uint64_t coverageColdDispatches = 0;
    std::uint64_t coverageWarmDispatches = 0;
    // Set when the scene carries a real GPU OCIO ProcessEffect command. A colour-transform scene
    // that ran zero native OCIO dispatch is a CPU whole-frame render in disguise.
    bool ocioRequired = false;
    std::uint64_t ocioColdDispatches = 0;
    // Set when the request carried a region of interest and the prepared scene silently widened (or
    // otherwise failed to match) the requested process data window. An ROI request answered with a
    // whole-frame output is a missed GPU ROI, never a pass.
    bool roiRequired = false;
    bool roiDropped = false;
    std::string evidence;
};

// The single place that decides whether a native run actually proved a GPU dispatch. A scene that
// prepared but then ran with zero native dispatches for the required family, or that fell back to
// the CPU, is a MISSED GPU regardless of pixel parity. Cold must dispatch, the immediate warm
// rerun must dispatch nothing and hit the output cache, and the route must not read back a frame.
[[nodiscard]] inline std::string_view nativeMissedGpuReason(const NativeFixtureOutcome& outcome) {
    if (outcome.cpuFallback) {
        return "MISSED_GPU: CPU fallback with no native dispatch";
    }
    if (outcome.coldDispatches == 0 || outcome.familyDispatches == 0) {
        return "MISSED_GPU: zero actual native dispatch for the required family";
    }
    if (outcome.coverageRequired && outcome.coverageColdDispatches == 0) {
        return "MISSED_GPU: zero native GpuPathCoverage dispatch for a vector coverage scene";
    }
    if (outcome.coverageRequired && outcome.coverageWarmDispatches != 0) {
        return "MISSED_GPU: warm rerun dispatched native coverage work";
    }
    if (outcome.ocioRequired && outcome.ocioColdDispatches == 0) {
        return "MISSED_GPU: zero native OCIO effect dispatch for a colour-transform scene";
    }
    if (outcome.roiRequired && outcome.roiDropped) {
        return "MISSED_GPU: the request ROI was silently dropped";
    }
    if (outcome.warmDispatches != 0) {
        return "MISSED_GPU: warm rerun dispatched native work";
    }
    if (outcome.warmCacheHits == 0) {
        return "MISSED_GPU: warm rerun missed the output cache";
    }
    if (outcome.readbacks != 0) {
        return "MISSED_GPU: the route performed a full-frame readback";
    }
    return {};
}

// CPU-only negative fixtures for the acceptance predicate itself. They construct the outcomes the
// native gate must reject -- a prepared scene that ran with zero actual dispatch, and an executor
// CPU fallback -- and prove the predicate classifies each as MISSED_GPU while a genuine dispatch is
// accepted. No GPU is fabricated; only the rejection logic is exercised.
[[nodiscard]] inline bool nativeNegativeFixturesDetectMissedGpu(std::string& evidence) {
    const auto rejects = [](const NativeFixtureOutcome& outcome) {
        return !nativeMissedGpuReason(outcome).empty();
    };
    NativeFixtureOutcome zeroDispatch;
    zeroDispatch.ran = true;
    zeroDispatch.warmCacheHits = 1;
    NativeFixtureOutcome cpuFallback;
    cpuFallback.ran = true;
    cpuFallback.cpuFallback = true;
    NativeFixtureOutcome genuine;
    genuine.ran = true;
    genuine.coldDispatches = 3;
    genuine.familyDispatches = 3;
    genuine.warmCacheHits = 1;
    // A vector coverage scene that reports a covered-solid fill but no GpuPathCoverage dispatch is
    // a host-mask pass in disguise and must be rejected.
    NativeFixtureOutcome coverageMissing;
    coverageMissing.ran = true;
    coverageMissing.coldDispatches = 3;
    coverageMissing.familyDispatches = 3;
    coverageMissing.warmCacheHits = 1;
    coverageMissing.coverageRequired = true;
    // A colour-transform scene that reports a generic dispatch but no OCIO effect dispatch is a CPU
    // whole-frame render in disguise and must be rejected.
    NativeFixtureOutcome ocioMissing;
    ocioMissing.ran = true;
    ocioMissing.coldDispatches = 3;
    ocioMissing.familyDispatches = 3;
    ocioMissing.warmCacheHits = 1;
    ocioMissing.ocioRequired = true;
    // An ROI request whose prepared scene dropped the ROI and produced a whole-frame output is a
    // missed GPU ROI even when the pixels happen to match a full-frame render.
    NativeFixtureOutcome roiDropped;
    roiDropped.ran = true;
    roiDropped.coldDispatches = 3;
    roiDropped.familyDispatches = 3;
    roiDropped.warmCacheHits = 1;
    roiDropped.roiRequired = true;
    roiDropped.roiDropped = true;
    // An ROI that the unchanged CPU oracle legitimately clips (a request region that reaches past
    // the resolved process window) is NOT a dropped ROI: the prepared output matches the oracle's
    // clipped data window, so the gate must accept it. This is the no-false-positive control for
    // the dropped-ROI rejection above.
    NativeFixtureOutcome roiCroppedToOracle;
    roiCroppedToOracle.ran = true;
    roiCroppedToOracle.coldDispatches = 3;
    roiCroppedToOracle.familyDispatches = 3;
    roiCroppedToOracle.warmCacheHits = 1;
    roiCroppedToOracle.roiRequired = true;
    roiCroppedToOracle.roiDropped = false;
    if (!rejects(zeroDispatch) || !rejects(cpuFallback) || !rejects(coverageMissing) ||
        !rejects(ocioMissing) || !rejects(roiDropped) || rejects(genuine) ||
        rejects(roiCroppedToOracle)) {
        evidence = "missed-GPU detection failed for zero-dispatch, CPU-fallback, missing "
                   "coverage-dispatch, missing OCIO-dispatch, or dropped-ROI fixtures, or rejected "
                   "a legitimately oracle-clipped ROI";
        return false;
    }
    evidence = "zero-dispatch, CPU-fallback, missing coverage-dispatch, missing OCIO-dispatch, and "
               "dropped-ROI fixtures are MISSED_GPU; a real dispatch and an oracle-clipped ROI are "
               "not";
    return true;
}

[[nodiscard]] inline bool pixelsClose(const std::span<const bloom::render::Rgba32f> actual,
                                      const std::span<const bloom::render::Rgba32f> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    const auto close = [](const float a, const float e) {
        const float tolerance = std::max(2.0e-6F, 2.0e-6F * std::fabs(e));
        return std::fabs(a - e) <= tolerance;
    };
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!close(actual[i].red(), expected[i].red()) ||
            !close(actual[i].green(), expected[i].green()) ||
            !close(actual[i].blue(), expected[i].blue()) ||
            actual[i].alpha() != expected[i].alpha()) {
            return false;
        }
    }
    return true;
}

// The first component that breaks the strict gate, for an actionable failure message. Empty when
// the spans match exactly (the same predicate pixelsClose() applies).
[[nodiscard]] inline std::string
pixelParityDetail(const std::span<const bloom::render::Rgba32f> actual,
                  const std::span<const bloom::render::Rgba32f> expected) {
    if (actual.size() != expected.size()) {
        return " (size " + std::to_string(actual.size()) + " vs " +
               std::to_string(expected.size()) + ")";
    }
    const auto close = [](const float a, const float e) {
        const float tolerance = std::max(2.0e-6F, 2.0e-6F * std::fabs(e));
        return std::fabs(a - e) <= tolerance;
    };
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].alpha() != expected[i].alpha()) {
            return " (alpha at " + std::to_string(i) + ": " + std::to_string(actual[i].alpha()) +
                   " vs " + std::to_string(expected[i].alpha()) + ")";
        }
        if (!close(actual[i].red(), expected[i].red())) {
            return " (red at " + std::to_string(i) + ": " + std::to_string(actual[i].red()) +
                   " vs " + std::to_string(expected[i].red()) + ")";
        }
        if (!close(actual[i].green(), expected[i].green())) {
            return " (green at " + std::to_string(i) + ": " + std::to_string(actual[i].green()) +
                   " vs " + std::to_string(expected[i].green()) + ")";
        }
        if (!close(actual[i].blue(), expected[i].blue())) {
            return " (blue at " + std::to_string(i) + ": " + std::to_string(actual[i].blue()) +
                   " vs " + std::to_string(expected[i].blue()) + ")";
        }
    }
    return {};
}

// The family the prepared scene actually requires, so the native counter that must fire is tied to
// the required node rather than any dispatch.
[[nodiscard]] inline NativeOperationFamily
requiredFamily(const bloom::runtime::PreparedGpuScene& scene) {
    bool hasSolid = false;
    bool hasCoverage = false;
    bool hasTranslation = false;
    bool hasMerge = false;
    bool hasBlend = false;
    bool hasAffine = false;
    for (const auto& command : scene.commands()) {
        hasSolid =
            hasSolid || std::holds_alternative<bloom::runtime::GpuSceneSolidCommand>(command);
        hasCoverage = hasCoverage ||
                      std::holds_alternative<bloom::runtime::GpuSceneCoverageSolidCommand>(command);
        hasTranslation =
            hasTranslation ||
            std::holds_alternative<bloom::runtime::GpuSceneTranslationCommand>(command);
        hasMerge =
            hasMerge || std::holds_alternative<bloom::runtime::GpuSceneMergeCommand>(command);
        hasBlend =
            hasBlend || std::holds_alternative<bloom::runtime::GpuSceneBlendCommand>(command);
        hasAffine =
            hasAffine || std::holds_alternative<bloom::runtime::GpuSceneAffineCommand>(command);
    }
    // The distinguishing family first: an explicit BlendV1 fold or a GpuAffine placement is what a
    // blend/affine fixture must actually dispatch, even when its inputs also emit coverage.
    if (hasBlend) {
        return NativeOperationFamily::Blend;
    }
    if (hasAffine) {
        return NativeOperationFamily::Affine;
    }
    if (hasCoverage) {
        return NativeOperationFamily::CoveredSolid;
    }
    if (hasTranslation) {
        return NativeOperationFamily::Translation;
    }
    if (hasSolid) {
        return NativeOperationFamily::Solid;
    }
    if (hasMerge) {
        return NativeOperationFamily::SourceOver;
    }
    return NativeOperationFamily::None;
}

[[nodiscard]] inline std::uint64_t
familyCount(const bloom::runtime::GpuSceneExecutorCounters& counters,
            const NativeOperationFamily family) {
    switch (family) {
    case NativeOperationFamily::Solid:
        return counters.solidDispatches;
    case NativeOperationFamily::CoveredSolid:
        return counters.coveredSolidDispatches;
    case NativeOperationFamily::Translation:
        return counters.translationDispatches;
    case NativeOperationFamily::SourceOver:
        return counters.sourceOverDispatches;
    case NativeOperationFamily::Blend:
        return counters.blendDispatches;
    case NativeOperationFamily::Affine:
        return counters.affineDispatches;
    case NativeOperationFamily::Upload:
        return counters.uploads;
    case NativeOperationFamily::None:
        return counters.dispatches;
    }
    return counters.dispatches;
}

// Runs one prepared fixture on a fresh device-bound GpuSceneCache (so a previous fixture's cached
// source/output key can never satisfy this fixture's cold run). Cold must dispatch the required
// family; the immediate warm re-run must dispatch nothing and hit the output cache.
[[nodiscard]] inline NativeFixtureOutcome
runNativeFixture(bloom::render::GpuDevice& device,
                 const bloom::runtime::CpuCompositionEvaluator& oracle,
                 const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
                 const bloom::runtime::EvaluationRequest& request,
                 const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene) {
    constexpr std::uint64_t kSceneBudget = 1U << 28U;
    constexpr auto kDeadline = std::chrono::steady_clock::duration{std::chrono::seconds{10}};
    NativeFixtureOutcome outcome;
    outcome.ran = true;
    outcome.family = requiredFamily(*scene);
    for (const auto& command : scene->commands()) {
        const auto* covered = std::get_if<bloom::runtime::GpuSceneCoverageSolidCommand>(&command);
        if (covered != nullptr && covered->geometry != nullptr) {
            outcome.coverageRequired = true;
        }
        if (std::holds_alternative<bloom::runtime::GpuSceneOcioEffectCommand>(command)) {
            outcome.ocioRequired = true;
        }
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = oracle.evaluate(plan, oracleRequest, {});
    if (frame.frame() == nullptr) {
        outcome.evidence = "CPU oracle did not evaluate";
        return outcome;
    }
    const auto& cpuImage = frame.frame()->processImage();
    // The expectation is the UNCHANGED CPU oracle's process descriptor, never the raw request
    // region. The oracle resolves an ROI into the process data window (clipped to the resolved
    // process window), so a legitimate clipped ROI is not a dropped ROI; a prepared scene that
    // widened the ROI to the whole frame no longer matches the oracle data window and is.
    outcome.roiRequired = request.roi.has_value();
    outcome.roiDropped = outcome.roiRequired && scene->outputDescriptor().dataWindow() !=
                                                    cpuImage.descriptor()->dataWindow();
    if (outcome.roiDropped) {
        outcome.evidence = "MISSED_GPU: the request ROI was silently dropped by the prepared scene";
        return outcome;
    }
    if (scene->outputDescriptor() != *cpuImage.descriptor()) {
        outcome.evidence = "prepared output descriptor differs from the CPU oracle";
        return outcome;
    }

    auto cacheResult = bloom::runtime::GpuSceneCache::create(device);
    if (!cacheResult) {
        outcome.evidence = "scene cache could not be created for this fixture";
        return outcome;
    }
    auto executorResult = bloom::runtime::GpuSceneExecutor::create(device, *cacheResult.cache);
    if (!executorResult) {
        outcome.evidence = "executor create failed: " + executorResult.diagnostic.message;
        return outcome;
    }
    auto& executor = *executorResult.executor;

    const auto pollToReady = [&]() -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto poll = executor.poll();
            if (poll == bloom::runtime::GpuSceneExecutorPollResult::Ready) {
                return true;
            }
            if (poll == bloom::runtime::GpuSceneExecutorPollResult::Failure ||
                poll == bloom::runtime::GpuSceneExecutorPollResult::WrongThread) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    const auto drain = [&]() {
        executor.cancel();
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (executor.hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline) {
            (void)executor.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    const auto runOnce = [&](const bool warm) -> bool {
        const auto before = executor.counters();
        const auto coverageBefore = bloom::render::GpuPathCoverage::nativeDispatchCount();
        if (const auto diagnostic = executor.begin(scene, kSceneBudget);
            diagnostic.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
            outcome.cpuFallback =
                diagnostic.code == bloom::runtime::GpuSceneExecutorDiagnosticCode::Unsupported ||
                diagnostic.code ==
                    bloom::runtime::GpuSceneExecutorDiagnosticCode::DeviceUnavailable;
            outcome.evidence = "executor begin refused: " + diagnostic.message;
            return false;
        }
        if (!pollToReady()) {
            outcome.evidence = executor.state() == bloom::runtime::GpuSceneExecutorJobState::Failure
                                   ? "executor poll failed: " + executor.diagnostic().message
                                   : "executor did not reach Ready before the deadline";
            drain();
            return false;
        }
        const auto* image = executor.image();
        if (image == nullptr) {
            outcome.evidence = "executor published no resident image";
            drain();
            return false;
        }
        if (!image->dataWindow().has_value() ||
            *image->dataWindow() != scene->outputDescriptor().dataWindow() ||
            !image->displayWindow().has_value() ||
            *image->displayWindow() != scene->outputDescriptor().displayWindow() ||
            image->pixelAspect() != scene->outputDescriptor().pixelAspect()) {
            outcome.evidence = "native image window/origin/PAR differs from the scene";
            drain();
            return false;
        }
        const auto readback = bloom::render::readbackResidentImage(*image, kSceneBudget);
        if (!readback) {
            outcome.evidence = "test readback failed";
            drain();
            return false;
        }
        if (readback.pixels.size() != cpuImage.pixels().size() ||
            !pixelsClose(readback.pixels, cpuImage.pixels())) {
            outcome.evidence = "native/CPU pixel parity failed" +
                               pixelParityDetail(readback.pixels, cpuImage.pixels());
            drain();
            return false;
        }
        const auto after = executor.counters();
        const auto dispatches = after.dispatches - before.dispatches;
        const auto coverageDispatches =
            bloom::render::GpuPathCoverage::nativeDispatchCount() - coverageBefore;
        const auto ocioDispatches = after.ocioEffectDispatches - before.ocioEffectDispatches;
        if (warm) {
            outcome.warmDispatches = dispatches;
            outcome.warmCacheHits = after.outputCacheHits - before.outputCacheHits;
            outcome.coverageWarmDispatches = coverageDispatches;
        } else {
            outcome.coldDispatches = dispatches;
            outcome.familyDispatches =
                familyCount(after, outcome.family) - familyCount(before, outcome.family);
            outcome.coverageColdDispatches = coverageDispatches;
            outcome.ocioColdDispatches = ocioDispatches;
        }
        (void)executor.takeImage();
        return true;
    };

    if (!runOnce(false) || !runOnce(true)) {
        return outcome;
    }
    outcome.readbacks = executor.counters().readbacks;

    if (const auto reason = nativeMissedGpuReason(outcome); !reason.empty()) {
        outcome.evidence = std::string{reason};
        return outcome;
    }
    outcome.passed = true;
    outcome.evidence = "native parity; family dispatches " +
                       std::to_string(outcome.familyDispatches) + ", cold " +
                       std::to_string(outcome.coldDispatches) + ", warm " +
                       std::to_string(outcome.warmDispatches) + ", warm cache hits " +
                       std::to_string(outcome.warmCacheHits) + ", coverage cold " +
                       std::to_string(outcome.coverageColdDispatches) + ", coverage warm " +
                       std::to_string(outcome.coverageWarmDispatches) + ", ocio cold " +
                       std::to_string(outcome.ocioColdDispatches);
    return outcome;
}

// Native nested-composition proof for the coverage gate. The production builder splices a real
// child plan's commands into the parent scene, so cold must dispatch genuine child commands, the
// immediate warm rerun must dispatch nothing new and hit the cache, and after a single-branch child
// edit the untouched branch's content-addressed key must still be cached while only the changed
// subtree re-dispatches. Pixels and descriptors are checked against the unchanged CPU oracle.
[[nodiscard]] inline bool runNestedBranchReuse(
    bloom::render::GpuDevice& device, const bloom::runtime::CpuCompositionEvaluator& oracle,
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& planA,
    const bloom::runtime::EvaluationRequest& requestA,
    const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& sceneA,
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& planB,
    const bloom::runtime::EvaluationRequest& requestB,
    const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& sceneB, std::string& evidence) {
    constexpr std::uint64_t kSceneBudget = 1U << 28U;
    constexpr auto kDeadline = std::chrono::steady_clock::duration{std::chrono::seconds{10}};
    auto cacheResult = bloom::runtime::GpuSceneCache::create(device);
    if (!cacheResult) {
        evidence = "nested branch reuse: scene cache could not be created";
        return false;
    }
    auto executorResult = bloom::runtime::GpuSceneExecutor::create(device, *cacheResult.cache);
    if (!executorResult) {
        evidence = "nested branch reuse: executor create failed";
        return false;
    }
    auto& executor = *executorResult.executor;

    struct RunResult final {
        bool ok = false;
        std::vector<bloom::render::Rgba32f> pixels;
        bloom::runtime::GpuSceneExecutorCounters counters;
        std::optional<bloom::render::ImageWindow> dataWindow;
        std::optional<bloom::render::ImageWindow> displayWindow;
        bloom::core::PixelAspectRatio pixelAspect = bloom::core::PixelAspectRatio::square();
        std::string why;
    };
    const auto run = [&](const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene) {
        RunResult result;
        if (const auto diagnostic = executor.begin(scene, kSceneBudget);
            diagnostic.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
            result.why = "begin refused: " + diagnostic.message;
            return result;
        }
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto poll = executor.poll();
            if (poll == bloom::runtime::GpuSceneExecutorPollResult::Ready) {
                break;
            }
            if (poll == bloom::runtime::GpuSceneExecutorPollResult::Failure ||
                poll == bloom::runtime::GpuSceneExecutorPollResult::WrongThread) {
                result.why = "poll failed: " + executor.diagnostic().message;
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto* image = executor.image();
        if (image == nullptr) {
            result.why = "no resident image";
            return result;
        }
        result.dataWindow = image->dataWindow();
        result.displayWindow = image->displayWindow();
        result.pixelAspect = image->pixelAspect();
        const auto readback = bloom::render::readbackResidentImage(*image, kSceneBudget);
        if (!readback) {
            result.why = "readback failed";
            return result;
        }
        result.pixels = readback.pixels;
        result.counters = executor.counters();
        (void)executor.takeImage();
        result.ok = true;
        return result;
    };

    const auto evaluate =
        [&](const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
            const bloom::runtime::EvaluationRequest& request) {
            auto oracleRequest = request;
            oracleRequest.bypassOperationCache = true;
            return oracle.evaluate(plan, oracleRequest, {});
        };
    const auto frameA = evaluate(planA, requestA);
    if (frameA.frame() == nullptr) {
        evidence = "nested branch reuse: CPU oracle A did not evaluate";
        return false;
    }
    const auto& oracleA = frameA.frame()->processImage();

    const auto cold = run(sceneA);
    if (!cold.ok || cold.counters.dispatches == 0 || cold.counters.commandsExecuted == 0 ||
        cold.counters.readbacks != 0 || !cold.dataWindow.has_value() ||
        *cold.dataWindow != oracleA.descriptor()->dataWindow() || !cold.displayWindow.has_value() ||
        *cold.displayWindow != oracleA.descriptor()->displayWindow() ||
        cold.pixelAspect != oracleA.descriptor()->pixelAspect() ||
        !pixelsClose(cold.pixels, oracleA.pixels())) {
        evidence =
            "nested branch reuse: cold run failed: " +
            (cold.why.empty() ? std::string{"dispatch/parity/descriptor mismatch"} : cold.why);
        return false;
    }

    const auto warm = run(sceneA);
    if (!warm.ok || warm.counters.dispatches != cold.counters.dispatches ||
        warm.counters.commandCacheHits <= cold.counters.commandCacheHits) {
        evidence = "nested branch reuse: warm run did not reuse the cache with zero dispatch";
        return false;
    }

    // The first child solid is the branch that does not change between A and B.
    std::optional<std::string> branchKey;
    for (const auto& command : sceneA->commands()) {
        if (const auto* solid = std::get_if<bloom::runtime::GpuSceneSolidCommand>(&command);
            solid != nullptr && solid->sourceOperation.value() == 0) {
            branchKey = solid->semanticKey;
            break;
        }
    }
    if (!branchKey.has_value()) {
        evidence = "nested branch reuse: no unrelated child branch key was found";
        return false;
    }

    const auto frameB = evaluate(planB, requestB);
    if (frameB.frame() == nullptr) {
        evidence = "nested branch reuse: CPU oracle B did not evaluate";
        return false;
    }
    const auto& oracleB = frameB.frame()->processImage();
    const auto changed = run(sceneB);
    if (!changed.ok || changed.counters.dispatches <= warm.counters.dispatches ||
        changed.counters.dispatches - warm.counters.dispatches >= cold.counters.dispatches ||
        cacheResult.cache->find(*branchKey) == nullptr ||
        !pixelsClose(changed.pixels, oracleB.pixels())) {
        evidence = "nested branch reuse: changed-branch dispatch or branch reuse failed";
        return false;
    }

    evidence = "nested branch reuse: cold dispatches " + std::to_string(cold.counters.dispatches) +
               ", warm reuses the cache, changed subtree re-dispatches " +
               std::to_string(changed.counters.dispatches - warm.counters.dispatches) +
               ", untouched branch cached";
    return true;
}

} // namespace bloom::gpu_coverage_native
