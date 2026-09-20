// Positive control for the coverage-contract mutation proof. This TU must compile: the contract
// header is valid and the known alternatives are classified. The reason-checked negative probe
// (gpu_coverage_contract_negative_probe.cpp) then fails specifically on the unclassified synthetic
// alternative, so that failure is attributable to the missing specialization and nothing else.

#include <bloom/runtime/gpu_coverage_contract.hpp>

#include <string_view>
#include <vector>

namespace {

using namespace bloom::runtime;

static_assert(std::string_view{GpuOperationCoverageTraits<CompiledSolid>::entry().id} ==
              "operation.CompiledSolid");
static_assert(std::string_view{GpuImageEffectCoverageTraits<CstKernel>::entry().id} ==
              "effect.CstKernel");

[[maybe_unused]] void positiveContractProbe() {
    const std::vector<GpuCoverageEntry> operations = gpuOperationCoverage();
    const std::vector<GpuCoverageEntry> effects = gpuImageEffectCoverage();
    const std::vector<GpuFeatureCoverageEntry> features = gpuFeatureCoverage();
    (void)operations;
    (void)effects;
    (void)features;
}

} // namespace
