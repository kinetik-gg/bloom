#pragma once

// Native execution helper for the bounded GPU coverage gate. It runs a real PreparedGpuScene
// through the production GpuSceneExecutor on a real device and compares the resident output against
// the CpuCompositionEvaluator oracle, including the actual descriptor window, origin, and pixel
// aspect. Nothing here fakes a dispatch or a readback: the counters are the executor's own.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
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
#include <span>
#include <string>
#include <thread>

namespace bloom::gpu_coverage_native {

enum class NativeOperationFamily : std::uint8_t {
    Solid,
    CoveredSolid,
    Translation,
    SourceOver,
    Upload,
    None,
};

struct NativeFixtureOutcome final {
    bool ran = false;
    bool passed = false;
    NativeOperationFamily family = NativeOperationFamily::None;
    std::uint64_t coldDispatches = 0;
    std::uint64_t familyDispatches = 0;
    std::uint64_t warmDispatches = 0;
    std::uint64_t warmCacheHits = 0;
    std::uint64_t readbacks = 0;
    std::string evidence;
};

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
            !close(actual[i].alpha(), expected[i].alpha())) {
            return false;
        }
    }
    return true;
}

// The family the prepared scene actually requires, so the native counter that must fire is tied to
// the required node rather than any dispatch.
[[nodiscard]] inline NativeOperationFamily
requiredFamily(const bloom::runtime::PreparedGpuScene& scene) {
    bool hasSolid = false;
    bool hasCoverage = false;
    bool hasTranslation = false;
    bool hasMerge = false;
    for (const auto& command : scene.commands()) {
        hasSolid = hasSolid || std::holds_alternative<bloom::runtime::GpuSceneSolidCommand>(command);
        hasCoverage =
            hasCoverage ||
            std::holds_alternative<bloom::runtime::GpuSceneCoverageSolidCommand>(command);
        hasTranslation =
            hasTranslation ||
            std::holds_alternative<bloom::runtime::GpuSceneTranslationCommand>(command);
        hasMerge =
            hasMerge || std::holds_alternative<bloom::runtime::GpuSceneMergeCommand>(command);
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

[[nodiscard]] inline std::uint64_t familyCount(
    const bloom::runtime::GpuSceneExecutorCounters& counters,
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

    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = oracle.evaluate(plan, oracleRequest, {});
    if (frame.frame() == nullptr) {
        outcome.evidence = "CPU oracle did not evaluate";
        return outcome;
    }
    const auto& cpuImage = frame.frame()->processImage();
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
        if (const auto diagnostic = executor.begin(scene, kSceneBudget);
            diagnostic.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
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
            outcome.evidence = "native/CPU pixel parity failed";
            drain();
            return false;
        }
        const auto after = executor.counters();
        const auto dispatches = after.dispatches - before.dispatches;
        if (warm) {
            outcome.warmDispatches = dispatches;
            outcome.warmCacheHits = after.outputCacheHits - before.outputCacheHits;
        } else {
            outcome.coldDispatches = dispatches;
            outcome.familyDispatches =
                familyCount(after, outcome.family) - familyCount(before, outcome.family);
        }
        (void)executor.takeImage();
        return true;
    };

    if (!runOnce(false) || !runOnce(true)) {
        return outcome;
    }
    outcome.readbacks = executor.counters().readbacks;

    const bool providerTruth = outcome.coldDispatches > 0 && outcome.familyDispatches > 0 &&
                               outcome.warmDispatches == 0 && outcome.warmCacheHits > 0 &&
                               outcome.readbacks == 0;
    if (!providerTruth) {
        outcome.evidence = "cold/family dispatch, warm-zero cache, or provenance not met";
        return outcome;
    }
    outcome.passed = true;
    outcome.evidence = "native parity; family dispatches " + std::to_string(outcome.familyDispatches) +
                       ", cold " + std::to_string(outcome.coldDispatches) + ", warm " +
                       std::to_string(outcome.warmDispatches) + ", warm cache hits " +
                       std::to_string(outcome.warmCacheHits);
    return outcome;
}

} // namespace bloom::gpu_coverage_native
