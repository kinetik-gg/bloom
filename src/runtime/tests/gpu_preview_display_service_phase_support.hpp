#pragma once

#include <cstdlib>
#include <utility>

#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

namespace bloom::runtime::detail {
// Real shared admission roots for the legacy service fixtures. Native startup reports the
// actual device; this sample supplies only the host constraint used before heavy CPU work.
inline void samplePreviewTestPolicy(GpuProcessingPhasePolicy& policy) {
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = ProcessMemoryStatus::Finite;
    sample.process.source = ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = GpuProcessingPhasePolicy::Clock::now();
    sample.process.domains.push_back({.path = "/fixture",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 0});
    sample.processSampleFresh = true;
    sample.sampleSerial = policy.snapshot().sampleSerial + 1;
    policy.applyHostSample(sample, 60 * gib, sample.process.sampledAt);
}
inline std::shared_ptr<GpuProcessingPhasePolicy> previewTestPolicy() {
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(4);
    samplePreviewTestPolicy(*policy);
    return policy;
}
inline document::Snapshot previewTestSnapshot(const CompiledCompositionPlan& plan) {
    document::Project project(plan.projectId(), "preview-service-fixture");
    const auto seed = document::makeNewProject("Fixture", "Main",
                                               core::RationalTime::fromInteger(4), plan.format());
    const bool added = project.addComposition(document::Composition(
        plan.compositionId(), "Main", core::RationalTime::fromInteger(4),
        seed.project.findComposition(seed.initialCompositionId)->graph(), plan.format()));
    if (!added)
        std::abort();
    document::Document document(std::move(project));
    return document.snapshot();
}
} // namespace bloom::runtime::detail
