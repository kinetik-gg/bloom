#pragma once

// Read-only acceleration status for the Settings window's Performance page.
//
// This is deliberately a seam, not a GPU dependency. The GPU backend lives in `src/render` behind a
// Qt- and Vulkan-free surface (docs/architecture/gpu-backend.md) and is not part of this build, so
// the default provider reports the truth here: the CPU reference path is active and no GPU
// acceleration is built in. When the GPU lane lands, it supplies a provider that reads the cached
// immutable capability report; the Settings page does not change.
//
// Boundary: a provider answers synchronously on the UI thread from already-published state. It must
// never probe or create a device, compile a shader, or wait on GPU work -- those are the GPU
// service's jobs and must not run on the UI thread.

#include <QString>
#include <QStringList>

namespace bloom::ui {

struct AccelerationStatus final {
    // True only when this build compiled GPU support and a device reached at least the bootstrap
    // baseline. False is the CPU-fallback truth, not an error.
    bool gpuBuildEnabled = false;
    // The backend actually evaluating work: "CPU reference" today, "Vulkan" once qualified.
    QString backend;
    // The device lifecycle state, in the GPU backend's own vocabulary ("Unavailable", "Ready",
    // "Lost", ...). Human-readable; the settings page only displays it.
    QString deviceState;
    QString deviceName;
    QString driver;
    // Ordered, human-readable qualification lines, one per exposed operation/precision. Empty when
    // no GPU support is built.
    QStringList operationStatus;
    // One sentence an artist can act on: what is running and why.
    QString summary;
};

// The truthful status for a build without GPU support.
[[nodiscard]] AccelerationStatus cpuOnlyAccelerationStatus();

class AccelerationStatusProvider {
  public:
    virtual ~AccelerationStatusProvider() = default;
    [[nodiscard]] virtual AccelerationStatus accelerationStatus() const = 0;
};

// Default provider: reports the CPU reference path. `apps/bloom` installs this today and will swap
// in a capability-report-backed provider when the GPU backend ships.
class CpuOnlyAccelerationStatus final : public AccelerationStatusProvider {
  public:
    [[nodiscard]] AccelerationStatus accelerationStatus() const override;
};

} // namespace bloom::ui
