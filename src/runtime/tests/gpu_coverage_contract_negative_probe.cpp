// Mutation-proof probe (expected to FAIL to compile). It instantiates the coverage collector for a
// synthetic operation alternative that has no GpuOperationCoverageTraits specialization, exactly as
// adding a new CompiledOperation alternative without classifying it would. The positive
// bloom_runtime_gpu_coverage_contract test proves this header compiles, so this target's failure is
// attributable to the unclassified alternative and nothing else. Registered with WILL_FAIL.

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <vector>

namespace {

struct SyntheticUnclassifiedOperation final {};

void proveUnclassifiedAlternativeFailsToCompile() {
    std::vector<bloom::runtime::GpuCoverageEntry> out;
    bloom::runtime::detail::collectGpuOperationCoverage<SyntheticUnclassifiedOperation>(out);
}

} // namespace
