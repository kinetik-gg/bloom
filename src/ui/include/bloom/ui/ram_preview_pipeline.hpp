#pragma once

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bloom::ui {

using PreviewPreparationResultHandle = runtime::PreviewPreparationResultHandle;

// The bounded, interface-thread-only bookkeeping behind the RAM preview fill's TWO-deep pipeline.
//
// A RAM preview must keep two frames preparing at once so that the CPU preparation of the next
// frame overlaps the display stage of the previous one. That overlap is only honest if the bound,
// the identity of each in-flight frame, and cancellation all live in one place: a range rebase
// must never submit a second copy of a frame that is already preparing, and a cancel/display/
// resolution change must detach every frame, not just the newest.
//
// This class owns exactly that. It performs no I/O, takes no lock, and touches the scheduler only
// to cancel a TaskHandle it was handed. It deliberately knows nothing about the range cursor, the
// session's provenance, or the cache: those stay in RamPreviewController, which is also where the
// "is this result still current?" decision is made. Keeping the bound here means it can be
// exercised with real scheduler handles and no UI at all.
class RamPreviewPipeline final {
  public:
    // Two frames in flight, never three. The GPU preview display service admits a 512 MiB
    // request-owned reservation per request against a 1 GiB scheduler capacity, so two is also the
    // exact number the existing service can run a stage for at once.
    static constexpr std::size_t kMaxInFlight = 2;

    struct InFlight final {
        std::uint64_t frameIndex = 0;
        runtime::PreviewRequestIdentity identity;
        runtime::TaskHandle<PreviewPreparationResultHandle> handle;
    };

    // One in-flight frame whose task has reached a terminal mailbox, moved out for the controller
    // to validate and insert. `result` is the scheduler's own TaskResult, never reinterpreted here.
    struct Ready final {
        std::uint64_t frameIndex = 0;
        runtime::PreviewRequestIdentity identity;
        runtime::TaskResult<PreviewPreparationResultHandle> result;
    };

    RamPreviewPipeline() = default;
    RamPreviewPipeline(const RamPreviewPipeline&) = delete;
    RamPreviewPipeline& operator=(const RamPreviewPipeline&) = delete;

    [[nodiscard]] bool hasCapacity() const noexcept { return slots_.size() < kMaxInFlight; }
    [[nodiscard]] std::size_t inFlightCount() const noexcept { return slots_.size(); }
    [[nodiscard]] bool empty() const noexcept { return slots_.empty(); }
    // The high-water mark since construction, so a test can prove the bound held even after the
    // frames have drained.
    [[nodiscard]] std::uint32_t peakInFlight() const noexcept { return peakInFlight_; }

    // True when a frame for exactly this pixels-identity (everything but requestGeneration) is
    // already preparing. A range rebase consults this before it re-scans so it can never submit a
    // duplicate of a frame still in flight.
    [[nodiscard]] bool isInFlight(const PreviewFrameCacheKey& key) const noexcept;

    // Adds an accepted submission. Caller must have checked hasCapacity().
    void add(InFlight slot);

    // Removes and returns every slot whose task has a result waiting, in submission order, so an
    // out-of-order completion is still reported in frame order. The taken slots no longer count
    // against the bound.
    [[nodiscard]] std::vector<Ready> takeReady();

    // Cancels and detaches every in-flight frame. Called by cancel(), beginShutdown(), every
    // invalidation that ends a run, and the bounded budget stop; after it returns, no frame this
    // pipeline owned can still be running.
    void cancelAllAndDetach() noexcept;

  private:
    std::vector<InFlight> slots_;
    std::uint32_t peakInFlight_ = 0;
};

} // namespace bloom::ui
