#pragma once

#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <chrono>
#include <thread>

namespace bloom::ui::test {
// Explicit host-only fixture constraint. The service reports the actual native device separately.
inline void refreshPreviewProcessingPhase(runtime::GpuProcessingPhasePolicy& policy) {
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    runtime::MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = runtime::ProcessMemoryStatus::Finite;
    sample.process.source = runtime::ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    sample.process.domains.push_back({.path = "/fixture",
                                      .kind = runtime::ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 0});
    sample.processSampleFresh = true;
    sample.sampleSerial = policy.snapshot().sampleSerial + 1;
    policy.applyHostSample(sample, 60 * gib, sample.process.sampledAt);
}
inline std::shared_ptr<runtime::GpuProcessingPhasePolicy> previewProcessingPhase() {
    auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(4);
    refreshPreviewProcessingPhase(*policy);
    return policy;
}
// Long retirement fixtures still require a fresh host delivery for their later CPU fallback.
inline std::jthread refreshPreviewProcessingPhaseWhileRunning(
    const std::shared_ptr<runtime::GpuProcessingPhasePolicy>& policy) {
    return std::jthread([policy](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            for (int index = 0; index < 100 && !stop.stop_requested(); ++index)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!stop.stop_requested())
                refreshPreviewProcessingPhase(*policy);
        }
    });
}
} // namespace bloom::ui::test
