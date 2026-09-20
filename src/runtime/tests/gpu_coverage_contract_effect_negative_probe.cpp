// Mutation-proof probe (expected to FAIL to compile). It instantiates the image-effect coverage
// collector for a synthetic ImageEffectKernel alternative that has no
// GpuImageEffectCoverageTraits specialization, exactly as adding a new kernel without classifying
// it would. The positive bloom_runtime_gpu_coverage_contract test proves the header compiles, so
// this target's failure is attributable to the unclassified kernel and nothing else.

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <vector>

namespace {

struct SyntheticUnclassifiedKernel final {};

void proveUnclassifiedKernelFailsToCompile() {
    std::vector<bloom::runtime::GpuCoverageEntry> out;
    bloom::runtime::detail::collectGpuEffectCoverage<SyntheticUnclassifiedKernel>(out);
}

} // namespace
