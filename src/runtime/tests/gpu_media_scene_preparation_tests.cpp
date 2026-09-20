#include "gpu_media_scene_preparation_test_support.hpp"

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

namespace bloom::runtime {

// Proof-only fixture seam: the builder's private, default-empty checkpoint callback is set only
// from the test translation unit (mirroring GpuSceneFixtureBuilder). Production defines neither
// this accessor nor a way to install one.
struct GpuSceneBuilderTestAccess final {
    static void setCheckpoint(CpuGpuSceneBuilder& builder, std::function<void()> checkpoint) {
        builder.checkpoint_ = std::move(checkpoint);
    }
};

} // namespace bloom::runtime

namespace {

#include "gpu_media_scene_preparation_test_support.ipp"

#include "gpu_media_scene_preparation_parity_tests.ipp"

#include "gpu_media_scene_preparation_cache_tests.ipp"

#include "gpu_media_scene_preparation_budget_tests.ipp"

#include "gpu_media_scene_preparation_cancellation_tests.ipp"

} // namespace

int main() {
    try {
        Expectations expectations;
        const CpuCompositionEvaluator evaluator;
        const auto fixture = makeFixture();
        evaluator.setAssetBaseDirectory(fixture.directory);

        testStillImageParity(expectations, evaluator, fixture);
        testProxyNonSquarePar(expectations, evaluator, fixture);
        testSequenceFrames(expectations, evaluator, fixture);
        testWarmReuseAndChangedSource(expectations, evaluator, fixture);
        testChangedFrameAndBypass(expectations, evaluator, fixture);
        testChangedColourInterpretation(expectations, evaluator, fixture);
        testSourceKeyExcludesIdsAndRevision(expectations, evaluator, fixture);
        testPerRequestStatisticsAreLocal(expectations, evaluator, fixture);
        testGestureCacheNeverTouchesDisk(expectations, evaluator, fixture);
        testAffineAndBlendMedia(expectations, evaluator, fixture);
        testBudgetRefusal(expectations, fixture);
        testLargeSourceUnderDefaultAllowance(expectations);
        testCapacityAwareProducerPolicy(expectations);

        {
            const auto cancelPlan =
                mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 3400);
            testPreCancelledPreparationPublishesNothing(expectations, cancelPlan);
        }
        testCancellationDuringPreparation(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU media scene preparation expectations failed\n";
            return 1;
        }
        if (std::getenv("BLOOM_GPU_MEDIA_PROOF") != nullptr) {
            // Recorded CPU work for the proof log: source-specific key construction and
            // decode/conversion counters. There is deliberately no GPU upload count -- the native
            // executor does not exist.
            const auto cold = GpuSceneMediaContext::fromEvaluator(evaluator);
            const CpuGpuSceneBuilder builder(nullptr, cold);
            const auto plan =
                mediaPlan(format(8, 8), fixture.asset, LayerValues{.position = {4.3, 3.1}}, 9900);
            const auto first = builder.build(plan, requestFor(*plan));
            const auto second = builder.build(plan, requestFor(*plan));
            if (first && second) {
                std::cerr << "PROOF counters: first imageSources="
                          << first.scene->mediaStatistics().imageSources
                          << " conversions=" << first.scene->mediaStatistics().imageConversions
                          << " keyConstructions="
                          << first.scene->mediaStatistics().uploadKeyConstructions
                          << " hits=" << first.scene->mediaStatistics().uploadCacheHits
                          << " misses=" << first.scene->mediaStatistics().uploadCacheMisses
                          << "; warm imageSources=" << second.scene->mediaStatistics().imageSources
                          << " conversions=" << second.scene->mediaStatistics().imageConversions
                          << " keyConstructions="
                          << second.scene->mediaStatistics().uploadKeyConstructions
                          << " hits=" << second.scene->mediaStatistics().uploadCacheHits
                          << " misses=" << second.scene->mediaStatistics().uploadCacheMisses
                          << "\n";
            }
        }
        std::cout << "PASS: CPU GPU media scene preparation\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
