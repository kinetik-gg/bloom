#pragma once

#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <memory>

namespace bloom::runtime {

class CpuCompositionEvaluator final {
  public:
    [[nodiscard]] EvaluationResult evaluate(std::shared_ptr<const CompiledCompositionPlan> plan,
                                            const EvaluationRequest& request,
                                            const CancellationToken& cancellation,
                                            EvaluationProgressCallback progress = {}) const;
};

} // namespace bloom::runtime
