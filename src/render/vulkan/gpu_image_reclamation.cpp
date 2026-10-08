#include "gpu_bounded_retirement.hpp"
#include "gpu_image_private.hpp"
#include "gpu_image_reclamation_private.hpp"
#include "gpu_oracle_readback_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <atomic>
#include <thread>

namespace bloom::render::image_detail {
namespace {
struct ProcessImageTag final {};
struct DisplayImageTag final {};
using Store = primitive_detail::VoidResidentSlotStore<kImageOwnerCapacity>;
Store& processImages() noexcept {
    return primitive_detail::immortalResidentSlotStore<kImageOwnerCapacity, ProcessImageTag>();
}
Store& displayImages() noexcept {
    return primitive_detail::immortalResidentSlotStore<kImageOwnerCapacity, DisplayImageTag>();
}
std::atomic<std::size_t> admissionLimit{0};
std::atomic<bool> pauseDestroy{false};
std::atomic<bool> destroyPaused{false};

template <typename Image> bool reserve(Image& image, Store& store) noexcept {
    if (image.state == nullptr || !image.onOwnerThread() || image.state->imageOwnerClosed) {
        return false;
    }
    if (image.reclamationSlot < kImageOwnerCapacity) {
        return true;
    }
    const auto limit = admissionLimit.load(std::memory_order_relaxed);
    if (!store.acquire(&image, image.reclamationSlot, limit == 0 ? kImageOwnerCapacity : limit)) {
        return false;
    }
    store.setImageMetadata(&image, image.reclamationSlot, image.state.get(), 0);
    return true;
}
template <typename Image>
void drain(Store& store, vulkan_detail::DeviceAllocatorState& state) noexcept {
    store.drainRetainedOrphans(
        [&state](void* raw) noexcept {
            const auto* image = static_cast<const Image*>(raw);
            return image->state.get() == &state && image->onOwnerThread();
        },
        [](void* raw) noexcept {
            // A wrapper's final release proves that all shared submission/presentation pins have
            // gone. The oracle's explicit unknown-submission flag is an additional fail-closed
            // guard; only its own exact proof may clear it (following oracle lifetime slice).
            return !static_cast<const Image*>(raw)->submissionUnretired;
        },
        [](void* raw) noexcept {
            auto* image = static_cast<Image*>(raw);
            if (pauseDestroy.load(std::memory_order_acquire)) {
                destroyPaused.store(true, std::memory_order_release);
                while (pauseDestroy.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            delete image;
        });
}
void collect(Store& store, const void* state, GpuImageReclamationStats& stats) noexcept {
    store.inspectImageMetadata(
        [&](const void* owner, std::uint64_t bytes, bool orphaned, bool closed) noexcept {
            if (state != nullptr && state != owner) {
                return;
            }
            if (orphaned) {
                ++stats.retiringImages;
                stats.retiringAllocationBytes += bytes;
                if (closed) {
                    ++stats.closedOwnerRetiringImages;
                    stats.closedOwnerRetiringAllocationBytes += bytes;
                }
            } else {
                ++stats.liveImages;
                stats.liveAllocationBytes += bytes;
            }
        });
}
} // namespace

bool reserveImage(GpuImageImpl& image) noexcept { return reserve(image, processImages()); }
bool reserveImage(GpuDisplayImageImpl& image) noexcept { return reserve(image, displayImages()); }
void registerImageBytes(GpuImageImpl& image, std::uint64_t bytes) noexcept {
    processImages().setImageMetadata(&image, image.reclamationSlot, image.state.get(), bytes);
}
void registerImageBytes(GpuDisplayImageImpl& image, std::uint64_t bytes) noexcept {
    displayImages().setImageMetadata(&image, image.reclamationSlot, image.state.get(), bytes);
}
void releaseImageSlot(GpuImageImpl& image) noexcept {
    processImages().release(&image, image.reclamationSlot);
    if (image.retirementMarked) {
        image.retirementMarked = false;
        image.state->pendingImageReclaims.fetch_sub(1, std::memory_order_release);
    }
    image.reclamationSlot = kImageOwnerCapacity;
}
void releaseImageSlot(GpuDisplayImageImpl& image) noexcept {
    displayImages().release(&image, image.reclamationSlot);
    if (image.retirementMarked) {
        image.retirementMarked = false;
        image.state->pendingImageReclaims.fetch_sub(1, std::memory_order_release);
    }
    image.reclamationSlot = kImageOwnerCapacity;
}
void orphanImage(GpuImageImpl& image) noexcept {
    if (!image.retirementMarked) {
        image.retirementMarked = true;
        image.state->pendingImageReclaims.fetch_add(1, std::memory_order_release);
    }
    processImages().orphan(&image, image.reclamationSlot);
}
void orphanImage(GpuDisplayImageImpl& image) noexcept {
    if (!image.retirementMarked) {
        image.retirementMarked = true;
        image.state->pendingImageReclaims.fetch_add(1, std::memory_order_release);
    }
    displayImages().orphan(&image, image.reclamationSlot);
}
void drainImages(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (state.owner != std::this_thread::get_id() || state.imageOwnerClosed ||
        state.ownerLifetimeToken != vulkan_detail::deviceOwnerLifetimeToken()) {
        return;
    }
    oracle_detail::drain(state);
    if (state.pendingImageReclaims.load(std::memory_order_acquire) == 0) {
        return;
    }
    state.imageReclamationScans.fetch_add(1, std::memory_order_relaxed);
    drain<GpuImageImpl>(processImages(), state);
    drain<GpuDisplayImageImpl>(displayImages(), state);
}
void closeImageOwner(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (state.owner != std::this_thread::get_id() ||
        state.ownerLifetimeToken != vulkan_detail::deviceOwnerLifetimeToken()) {
        return;
    }
    drainImages(state);
    state.imageOwnerClosed = true;
    processImages().closeImageOwner(&state);
    displayImages().closeImageOwner(&state);
}
GpuImageReclamationStats imageStats(const void* state) noexcept {
    GpuImageReclamationStats stats;
    stats.capacityPerFamily = kImageOwnerCapacity;
    collect(processImages(), state, stats);
    collect(displayImages(), state, stats);
    // Aggregate counters are process-wide; per-generation counts and bytes above are exact.
    stats.admissionRefusals = processImages().refusals() + displayImages().refusals();
    stats.ownerReclaims = processImages().retired() + displayImages().retired();
    return stats;
}
void setImageDestroyPauseForTests(bool pause) noexcept {
    destroyPaused.store(false, std::memory_order_release);
    pauseDestroy.store(pause, std::memory_order_release);
}
bool imageDestroyPausedForTests() noexcept { return destroyPaused.load(std::memory_order_acquire); }
void setImageAdmissionLimitForTests(std::size_t limit) noexcept {
    admissionLimit.store(limit, std::memory_order_relaxed);
}
} // namespace bloom::render::image_detail
