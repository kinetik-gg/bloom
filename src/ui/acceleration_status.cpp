#include <bloom/ui/acceleration_status.hpp>

namespace bloom::ui {

AccelerationStatus cpuOnlyAccelerationStatus() {
    AccelerationStatus status;
    status.gpuBuildEnabled = false;
    status.backend = QStringLiteral("CPU reference");
    status.deviceState = QStringLiteral("Unavailable");
    status.summary = QStringLiteral(
        "GPU acceleration is not built into this version. The CPU reference evaluator is active "
        "and deterministic output is unaffected.");
    return status;
}

AccelerationStatus CpuOnlyAccelerationStatus::accelerationStatus() const {
    return cpuOnlyAccelerationStatus();
}

} // namespace bloom::ui
