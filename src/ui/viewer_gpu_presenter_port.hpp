#pragma once

// Private seam for ViewerGpuPresenter. It names the exact subset of the existing runtime
// GpuPresentationClient the adapter uses, so the CPU-only adapter test can substitute a scripted
// fake without a device or a coordinator. The production implementation is a strict pass-through;
// it adds no policy, no thread, and no state.

#include <bloom/render/gpu_present_image.hpp>
#include <bloom/render/gpu_presentation_target.hpp>
#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>

#include <cstdint>
#include <memory>

namespace bloom::ui {

class ViewerGpuPort {
  public:
    ViewerGpuPort() = default;
    ViewerGpuPort(const ViewerGpuPort&) = delete;
    ViewerGpuPort& operator=(const ViewerGpuPort&) = delete;
    virtual ~ViewerGpuPort() = default;

    [[nodiscard]] virtual render::GpuBorrowedInstanceView instanceView() const = 0;
    [[nodiscard]] virtual runtime::GpuPresentationPortResult
    attach(const render::GpuBorrowedSurface& surface, std::uint32_t width,
           std::uint32_t height) = 0;
    [[nodiscard]] virtual runtime::GpuPresentationPortResult
    update(runtime::GpuPresentationTargetId target, std::uint64_t sequence,
           runtime::GpuPresentationUpdate update) = 0;
    [[nodiscard]] virtual runtime::GpuPresentationPortResult
    resize(runtime::GpuPresentationTargetId target, std::uint64_t sequence, std::uint32_t width,
           std::uint32_t height) = 0;
    [[nodiscard]] virtual runtime::GpuPresentationPortResult
    retire(runtime::GpuPresentationTargetId target, std::uint64_t sequence) = 0;
    // Bounded terminal acknowledgement. Called by the adapter ONLY for a proven-terminal record
    // (Retired, or Rejected with surfaceSafeToDestroy). The runtime refuses a live/unproven target
    // with NotTerminal and keeps its record and surface ownership.
    [[nodiscard]] virtual runtime::GpuPresentationPortResult
    forget(runtime::GpuPresentationTargetId target) = 0;
    [[nodiscard]] virtual runtime::GpuPresentationTargetSnapshot
    status(runtime::GpuPresentationTargetId target) const = 0;
    [[nodiscard]] virtual bool ownerAlive() const noexcept = 0;
};

// Wraps the existing shared runtime client. The adapter keeps the shared_ptr alive for its
// lifetime.
[[nodiscard]] std::shared_ptr<ViewerGpuPort>
makeRuntimeViewerGpuPort(std::shared_ptr<runtime::GpuPresentationClient> client);

} // namespace bloom::ui
