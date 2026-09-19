#pragma once

// Read-only acceleration status for the Preferences window's Performance page.
//
// This is a presentation seam over already-published state, not a GPU dependency the UI owns. The
// GPU backend lives in `src/render`/`src/runtime` behind a Qt- and Vulkan-free surface
// (docs/architecture/gpu-backend.md). bloom_ui already links bloom_runtime for the viewer
// bootstrap, so a provider here may read the runtime's cached service status directly; it must not
// create a second service, device, or thread. When no service is wired the default provider reports
// the CPU reference truth and the Settings page does not change.
//
// Boundary: a provider answers synchronously on the UI thread from already-published state. It must
// never probe or create a device, compile a shader, or wait on GPU work -- those are the GPU
// service's jobs and must not run on the UI thread. `accelerationStatusFromServiceStatus()` is a
// pure mapping over one such already-published snapshot; it performs no device or thread work.

#include <bloom/runtime/gpu_preview_display_service.hpp>

#include <QString>
#include <QStringList>

namespace bloom::ui {

struct AccelerationStatus final {
    // The backend actually offered by this generation: "CPU reference" today, "Vulkan" once the
    // service reports Ready. Ready is a capability fact, not a claim that work is executing.
    QString backend;
    // The device lifecycle state in the service's own vocabulary ("Unavailable", "Initializing",
    // "Ready", "Stopping", "Stopped"). Human-readable; the settings page only displays it.
    QString deviceState;
    QString deviceName;
    QString driver;
    // Availability of the eligible resident preview route for this service generation: "Available
    // for eligible previews" or "CPU fallback". The service cannot know which operation a given
    // frame needs, so this is not a claim that any frame is (or any Viewer is) currently running on
    // the GPU; unsupported nodes/colours still fall back per frame.
    QString previewRoute;
    // Presentation capability of this service generation, never an activation claim: "Not
    // requested", "Unavailable", or "Ready". A non-null presentation client is a capability, not
    // evidence that a Viewer is actively presenting.
    QString presentationStatus;
    // Ordered, human-readable qualification lines, one per exposed operation/precision. Empty when
    // no genuine eligible qualification report has been published.
    QStringList operationStatus;
    // One sentence an artist can act on: what is running and why.
    QString summary;

    friend bool operator==(const AccelerationStatus&, const AccelerationStatus&) = default;
};

// The truthful CPU-reference status for a host that wires no GPU service. It never claims the GPU
// support is absent from the build, because absence of a wired service is not proof of that.
[[nodiscard]] AccelerationStatus cpuOnlyAccelerationStatus();

// Pure, device-free mapping over one already-published service status. It reads only fields the
// service publishes under its own lock (state, availability, diagnostic, counters, the immutable
// qualification reports, and the presentation generation) and performs no device call, wait, or
// thread work. Device identity, qualification, and the eligible pixel interval come only from a
// genuine qualification report; a missing report leaves the device text empty and the route on the
// CPU fallback.
[[nodiscard]] AccelerationStatus
accelerationStatusFromServiceStatus(const runtime::GpuPreviewDisplayServiceStatus& status);

class AccelerationStatusProvider {
  public:
    virtual ~AccelerationStatusProvider() = default;
    [[nodiscard]] virtual AccelerationStatus accelerationStatus() const = 0;
};

// Default provider: reports the CPU reference path. A host that has no GPU service keeps this.
class CpuOnlyAccelerationStatus final : public AccelerationStatusProvider {
  public:
    [[nodiscard]] AccelerationStatus accelerationStatus() const override;
};

// Caches the last mapped status for synchronous UI-thread reads. The application's existing status
// poll (which already runs until shutdown) hands it one `service.status()` read; it performs no
// subscription, probing, or device work of its own. Until the first refresh it reports the CPU
// reference truth. Not thread-safe: call it from the owner UI thread, like every other provider.
class CachedAccelerationStatusProvider final : public AccelerationStatusProvider {
  public:
    CachedAccelerationStatusProvider() = default;

    // Maps and caches one already-published status snapshot. Cheap and UI-thread-safe.
    void setServiceStatus(const runtime::GpuPreviewDisplayServiceStatus& status);

    [[nodiscard]] AccelerationStatus accelerationStatus() const override;

  private:
    AccelerationStatus cached_ = cpuOnlyAccelerationStatus();
};

} // namespace bloom::ui
