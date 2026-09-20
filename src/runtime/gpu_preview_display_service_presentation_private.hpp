#pragma once

// Owner-thread presentation generation of GpuPreviewDisplayService. This is the only place the
// service touches the real presentation machinery: it creates and owns exactly one
// GpuResidentFrameLeaseRegistry and one GpuPresentationCoordinator on the service's own device and
// its own service thread, with no second service, thread, device, or generic callback bus.
//
// The UI never sees a native object. The only thing published off the owner thread is the immutable
// shared GpuPresentationClient (a mailbox with no native state) and a copy of the coordinator's
// synchronized shutdown snapshot. All creation, pumping, and retirement happens on the owner
// thread; destruction order is coordinator -> registry -> device.

#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace bloom::runtime::detail {

struct PreviewDisplayServiceCore;

// The service's owner-thread presentation generation. Constructed only on the service thread after
// the device generation is Ready; `available` is false and `detail` explains why when the requested
// presentation capability is not present. Never holds a UI/native handle.
struct PreviewDisplayPresentation final {
    std::unique_ptr<GpuResidentFrameLeaseRegistry> registry;
    std::unique_ptr<GpuPresentationCoordinator> coordinator;
    std::shared_ptr<GpuPresentationClient> client;
    bool available = false;
    std::string detail;
    // Owner-thread shadow of the last values copied under the service state mutex, so an idle owner
    // pump does not relock and republish an identical snapshot. Never read off the owner thread.
    render::GpuPresentationAvailability publishedAvailability =
        render::GpuPresentationAvailability::NotRequested;
    std::string publishedDetail;
    std::shared_ptr<GpuPresentationClient> publishedClient;
    GpuPresentationShutdownStatus publishedShutdown;
    bool publishedOnce = false;
};

// Owner-thread. Creates the registry + coordinator for this service's device when presentation mode
// is enabled and the device presentation capability is genuinely Ready. Returns nullptr (never
// throws) when presentation is Disabled or Unavailable; `core->publishedPresentationAvailability`
// and detail are updated by publishServicePresentation().
[[nodiscard]] std::unique_ptr<PreviewDisplayPresentation>
createServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Copies the owner-thread availability/client into the core's stateMutex-protected published fields
// so status() can be read safely from the UI thread without touching owner entries.
void publishServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner-thread. Begins coordinator shutdown exactly once (idempotent). Safe to call every drain
// iteration; does nothing when there is no live presentation generation.
void beginServicePresentationShutdown(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner-thread. Drives one coordinator pump and the registry collection, then republishes the
// thread-safe shutdown snapshot. Must be called even when no preview task is outstanding.
void pumpServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner-thread. True while the presentation generation still has to be pumped before retirement can
// be proven (an in-flight retire, or the initial beginShutdown that has not run yet). False once
// every target is terminal or the coordinator's bounded drain budget has marked the rest unproven.
[[nodiscard]] bool
servicePresentationNeedsPump(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Owner-thread. Final bounded retirement: beginShutdown (if not already), keep pumping until the
// coordinator reports every target terminal or its own drain budget is exhausted, publish the
// actual snapshot, then destroy the coordinator before the registry. Unproven native targets are
// handed to the coordinator's process quarantine (which keeps the resident-image pins alive) and
// the registry leases are invalidated exactly once, only after the coordinator no longer holds any
// present pin.
void retireServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept;

// Test-only. Asks the owner thread to run one actual scheduler GPU task that produces a real
// resident display image (GpuSolid -> GpuResidentDisplay) and publishes it into the service's own
// registry. No public native handle and no fabricated image crosses this seam; the returned lease
// is the ordinary opaque token. `core` must have a live presentation generation.
struct PresentationTestLeaseResult final {
    GpuResidentFrameLease lease;
    bool ran = false;
    std::string diagnostic;
};

[[nodiscard]] bool
requestServicePresentationTestLease(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                    PresentationTestLeaseResult& out,
                                    std::chrono::milliseconds timeout, bool foreign = false);

} // namespace bloom::runtime::detail
