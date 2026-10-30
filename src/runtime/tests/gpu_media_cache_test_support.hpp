#pragma once

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

namespace {
// Existing small semantic fixtures intentionally exercise the fixed standalone legacy arm.
// Production omitted/rooted retention is qualified by the host source-reuse fixture; an unbound
// fromEvaluator context itself remains uncached.
[[nodiscard]] inline bloom::runtime::GpuSceneMediaContext
legacyPreparedMediaContext(const bloom::runtime::CpuCompositionEvaluator& evaluator) {
    auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    context.preparedUploadCache = std::make_shared<bloom::runtime::GpuPreparedUploadCache>(
        bloom::runtime::kDefaultPreparedUploadCacheBytes);
    // Deterministic per-build statistics: read-ahead is exercised by its own fixture.
    context.sequenceReadAhead.reset();
    return context;
}

} // namespace
