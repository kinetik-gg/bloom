#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <memory>

namespace bloom::runtime {

class CpuCompositionEvaluator final {
  public:
    // `rowBands` is the bounded pool the per-row kernels are spread across (the scheduler owns one;
    // TaskContext::rowBandExecutor() is where a task body gets it). Null evaluates every row band
    // on the calling thread, in band order.
    //
    // The pixels are the same either way, to the bit: banding partitions rows, and every row kernel
    // writes only its own row. The evaluator therefore does NOT enter the pool into
    // ProcessFrameIdentity -- a frame evaluated in parallel and the same frame evaluated serially
    // are the same frame, and a cache that holds one may serve the other.
    [[nodiscard]] EvaluationResult evaluate(std::shared_ptr<const CompiledCompositionPlan> plan,
                                            const EvaluationRequest& request,
                                            const CancellationToken& cancellation,
                                            EvaluationProgressCallback progress = {},
                                            CpuRowBandExecutor* rowBands = nullptr) const;
};

} // namespace bloom::runtime
