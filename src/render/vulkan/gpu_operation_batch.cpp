#include "gpu_operation_batch_private.hpp"

#include <cstring>
#include <utility>

namespace bloom::render::operation_batch_detail {
bool State::owner() const noexcept {
    return control && control->owner == std::this_thread::get_id() &&
           control->ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken();
}
void State::fail(const GpuOperationBatchCode code, const char* message) noexcept {
    state = GpuOperationBatchState::Failure;
    diagnostic.code = code;
    try {
        diagnostic.message = message;
    } catch (...) {
        diagnostic.message.clear();
    }
}
void State::clear(const bool restorePrefix) noexcept {
    if (submitted)
        return;
    // Reset recorded references before backing destruction. This is called only after proof, or
    // before any submission; an unknown fence never reaches this release path.
    if (command != VK_NULL_HANDLE)
        static_cast<void>(control->device.getDispatcher()->vkResetCommandBuffer(command, 0));
    for (auto& job : jobs) {
        if (restorePrefix && job.image) {
            auto token = std::move(job.image->storageToken);
            job.image.reset();
            static_cast<void>(
                token.releaseFailedAttemptAfterBackingDestroyed(control->processingPhase));
        } else {
            job.image.reset();
        }
        for (auto& metadata : job.metadata) {
            if (restorePrefix)
                metadata.releaseFailedAttempt();
            else
                metadata.release();
        }
        job.sourcePin.reset();
        job.destinationPin.reset();
        job.source = nullptr;
        job.destination = nullptr;
    }
    count = 0;
}
State::~State() {
    clear(false);
    const auto* api = control->device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*control->device);
    if (fence != VK_NULL_HANDLE)
        api->vkDestroyFence(device, fence, nullptr);
    if (commandPool != VK_NULL_HANDLE)
        api->vkDestroyCommandPool(device, commandPool, nullptr);
    if (descriptors != VK_NULL_HANDLE)
        api->vkDestroyDescriptorPool(device, descriptors, nullptr);
    pipelines = {};
    if (ownsSlot)
        control->operationBatchOccupied.store(false, std::memory_order_release);
    if (orphaned)
        orphanCount.fetch_sub(1, std::memory_order_acq_rel);
}
bool State::submit() {
    const auto* api = control->device.getDispatcher();
    const VkSubmitInfo submitInfo{
        VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command, 0, nullptr};
    const auto result =
        api->vkQueueSubmit(static_cast<VkQueue>(*control->computeQueue), 1, &submitInfo, fence);
    if (result != VK_SUCCESS) {
        cancelled = true;
        lost = result == VK_ERROR_DEVICE_LOST;
        // A failed submission with no device-loss proof cannot authorize serial retry or backing
        // destruction. Preserve the exact owner until an actual fence or loss proves retirement.
        submitted = !lost;
        fail(lost ? GpuOperationBatchCode::DeviceLost : GpuOperationBatchCode::DeviceUnavailable,
             "native batch submission failed");
        return false;
    }
    submitted = true;
    state = GpuOperationBatchState::Pending;
    statistics.submissions = 1;
    statistics.dispatches = count;
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    if (gpu_scene_executor_fault::peek() ==
        gpu_scene_executor_fault::PollFault::UnknownSubmitAfterAcceptance) {
        static_cast<void>(gpu_scene_executor_fault::take());
        cancelled = true;
        fail(GpuOperationBatchCode::Unproven,
             "injected uncertain submit after genuine queue acceptance");
        return false;
    }
#endif
    return true;
}
void State::poll() noexcept {
    if (!submitted)
        return;
    const auto* api = control->device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*control->device);
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    const auto injected = gpu_scene_executor_fault::take();
    if (injected == gpu_scene_executor_fault::PollFault::StallPending)
        return;
    if (injected == gpu_scene_executor_fault::PollFault::UnknownFence) {
        cancelled = true;
        fail(GpuOperationBatchCode::Unproven, "injected unknown batch fence; exact owner retained");
        return;
    }
    if (injected == gpu_scene_executor_fault::PollFault::DeviceLost) {
        const auto proof = api->vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000ULL);
        if (proof != VK_SUCCESS) {
            cancelled = true;
            fail(GpuOperationBatchCode::Unproven, "injected batch loss could not prove retirement");
            return;
        }
        fault = PollFault::LostAfterProof;
    }
#endif
    const auto result = fault == PollFault::Pending   ? VK_NOT_READY
                        : fault == PollFault::Unknown ? VK_ERROR_UNKNOWN
                                                      : api->vkGetFenceStatus(device, fence);
    if (result == VK_NOT_READY)
        return;
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        cancelled = true;
        fail(GpuOperationBatchCode::Unproven,
             "batch fence retirement is unknown; exact owner retained");
        return;
    }
    submitted = false;
    lost = result == VK_ERROR_DEVICE_LOST || fault == PollFault::LostAfterProof;
    if (lost) {
        fail(GpuOperationBatchCode::DeviceLost, "device lost after batch retirement proof");
        clear(false);
        return;
    }
    if (cancelled) {
        fail(GpuOperationBatchCode::Cancelled, "batch cancelled after fence retirement proof");
        clear(false);
        return;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& status = jobs[index].metadata[2];
        if (!status.armed)
            continue;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(control->allocator, status.allocation, &info);
        std::uint32_t flag = 0;
        if (info.pMappedData == nullptr ||
            composite_detail::mappedMemoryVisibility(true, control->allocator, status.allocation,
                                                     sizeof(flag)) != VK_SUCCESS) {
            fail(GpuOperationBatchCode::StatusRejected,
                 "batch status visibility failed after retirement");
            clear(false);
            return;
        }
        std::memcpy(&flag, info.pMappedData, sizeof(flag));
        if (flag != 0 || fault == PollFault::StatusAfterProof) {
            fail(GpuOperationBatchCode::StatusRejected, "batch kernel status rejected all outputs");
            clear(false);
            return;
        }
    }
    state = GpuOperationBatchState::Ready;
}
void retire(State* state) noexcept {
    if (state->owner()) {
        state->cancelled = true;
        if (state->submitted && state->fault == PollFault::None) {
            const VkDevice device = static_cast<VkDevice>(*state->control->device);
            const auto proof = state->control->device.getDispatcher()->vkWaitForFences(
                device, 1, &state->fence, VK_TRUE, 2'000'000'000ULL);
            if (proof == VK_SUCCESS || proof == VK_ERROR_DEVICE_LOST) {
                // Bounded teardown discards pixels and already has real positive proof. Poll
                // fault controls cannot erase that evidence or retain a healthy retired owner.
                state->submitted = false;
                state->lost = proof == VK_ERROR_DEVICE_LOST;
                state->clear(false);
            }
        }
        state->poll();
        if (!state->submitted) {
            delete state;
            return;
        }
    }
    state->orphaned = true;
    orphanCount.fetch_add(1, std::memory_order_acq_rel);
    state->control->retiredOperationBatch.store(state, std::memory_order_release);
}
void drainRetired(vulkan_detail::DeviceAllocatorState& control) noexcept {
    auto* const state = control.retiredOperationBatch.load(std::memory_order_acquire);
    if (!state || !state->owner())
        return;
    state->cancelled = true;
    state->poll();
    if (!state->submitted) {
        control.retiredOperationBatch.store(nullptr, std::memory_order_release);
        delete state;
    }
}
} // namespace bloom::render::operation_batch_detail

namespace bloom::render {
struct GpuOperationBatch::Impl final {
    std::unique_ptr<operation_batch_detail::State, decltype(&operation_batch_detail::retire)> state{
        nullptr, operation_batch_detail::retire};
    GpuOperationBatchBudgets budgets;
    GpuOperationBatchDiagnostic unavailable{GpuOperationBatchCode::Busy,
                                            "the bounded native batch owner is occupied"};
};
GpuOperationBatch::GpuOperationBatch(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
GpuOperationBatch::GpuOperationBatch(GpuOperationBatch&&) noexcept = default;
GpuOperationBatch& GpuOperationBatch::operator=(GpuOperationBatch&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuOperationBatch::~GpuOperationBatch() { releaseImpl(); }
void GpuOperationBatch::releaseImpl() noexcept { impl_.reset(); }
std::unique_ptr<GpuOperationBatch>
GpuOperationBatch::create(GpuDevice& device, const GpuOperationBatchBudgets& budgets) noexcept {
    try {
        auto impl = std::make_unique<Impl>();
        impl->budgets = budgets;
        if (device.isOwnerThread() && device.state() == GpuDeviceState::Ready) {
            auto control = GpuRendererAccess::state(device);
            operation_batch_detail::drainRetired(*control);
            auto state = std::make_unique<operation_batch_detail::State>();
            state->control = control;
            bool occupied = false;
            if (control->operationBatchOccupied.compare_exchange_strong(
                    occupied, true, std::memory_order_acq_rel)) {
                state->ownsSlot = true;
                state->expectedGeneration = state->control->generation;
                impl->state.reset(state.release());
            }
        } else {
            impl->unavailable = {device.isOwnerThread() ? GpuOperationBatchCode::DeviceUnavailable
                                                        : GpuOperationBatchCode::WrongThread,
                                 "native batch device is unavailable on this thread"};
        }
        return std::unique_ptr<GpuOperationBatch>(new GpuOperationBatch(std::move(impl)));
    } catch (...) {
        return {};
    }
}
GpuOperationBatchDiagnostic
GpuOperationBatch::begin(const std::span<const GpuBatchOperation> operations,
                         const std::uint64_t byteBudget) {
    if (!impl_ || !impl_->state)
        return diagnostic();
    auto& state = *impl_->state;
    if (!state.owner())
        return {GpuOperationBatchCode::WrongThread, "batch begin requires the device owner"};
    if (state.submitted || state.state == GpuOperationBatchState::Ready)
        return {GpuOperationBatchCode::Busy, "batch outputs or submission have not retired"};
    if (state.lost || state.control->imageOwnerClosed ||
        state.expectedGeneration != state.control->generation)
        return {GpuOperationBatchCode::DeviceLost, "batch generation is closed or lost"};
    state.clear(false);
    state.cancelled = false;
    state.state = GpuOperationBatchState::Idle;
    state.diagnostic = {};
    state.statistics = {};
    try {
        if (!state.initialize()) {
            if (state.diagnostic.code == GpuOperationBatchCode::None)
                state.fail(GpuOperationBatchCode::AllocationFailed,
                           "batch native resources could not initialize");
        } else if (state.prepare(operations, impl_->budgets, byteBudget) && state.record() &&
                   state.submit())
            return {};
        else if (state.diagnostic.code == GpuOperationBatchCode::None)
            state.fail(GpuOperationBatchCode::DeviceUnavailable, "batch command recording failed");
    } catch (...) {
        state.fail(GpuOperationBatchCode::AllocationFailed, "batch preparation allocation failed");
    }
    // Generic fallback is permitted only before any submission. Restoring consumed origin prefix
    // follows actual backing destruction, with no extra request grant and no phase substitution.
    if (!state.submitted)
        state.clear(!state.lost);
    return state.diagnostic;
}
GpuOperationBatchState GpuOperationBatch::poll() noexcept {
    if (!impl_ || !impl_->state)
        return GpuOperationBatchState::Failure;
    if (!impl_->state->owner())
        return GpuOperationBatchState::WrongThread;
    impl_->state->poll();
    return impl_->state->state;
}
GpuOperationBatchState GpuOperationBatch::state() const noexcept {
    if (impl_ && impl_->state && !impl_->state->owner())
        return GpuOperationBatchState::WrongThread;
    return impl_ && impl_->state ? impl_->state->state : GpuOperationBatchState::Failure;
}
GpuOperationBatchDiagnostic GpuOperationBatch::diagnostic() const {
    if (impl_ && impl_->state && !impl_->state->owner())
        return {GpuOperationBatchCode::WrongThread, "batch diagnostics require the device owner"};
    return impl_ && impl_->state ? impl_->state->diagnostic
           : impl_ ? impl_->unavailable
                   : GpuOperationBatchDiagnostic{GpuOperationBatchCode::DeviceUnavailable,
                                                 "no batch owner"};
}
GpuOperationBatchStatistics GpuOperationBatch::statistics() const noexcept {
    return impl_ && impl_->state && impl_->state->owner() ? impl_->state->statistics
                                                          : GpuOperationBatchStatistics{};
}
bool GpuOperationBatch::hasUnretiredSubmission() const noexcept {
    return impl_ && impl_->state && (!impl_->state->owner() || impl_->state->submitted);
}
bool GpuOperationBatch::teardownDrainIncomplete() noexcept {
    return operation_batch_detail::orphanCount.load(std::memory_order_acquire) != 0;
}
std::vector<GpuImage> GpuOperationBatch::takeImages() {
    std::vector<GpuImage> images;
    if (!impl_ || !impl_->state || !impl_->state->owner() ||
        impl_->state->state != GpuOperationBatchState::Ready)
        return images;
    auto& state = *impl_->state;
    images.reserve(state.count);
    for (std::uint32_t index = 0; index < state.count; ++index)
        images.push_back(makeGpuImage(std::move(state.jobs[index].image)));
    state.clear(false);
    state.state = GpuOperationBatchState::Idle;
    return images;
}
void GpuOperationBatch::cancel() noexcept {
    if (impl_ && impl_->state && impl_->state->owner()) {
        impl_->state->cancelled = true;
        if (!impl_->state->submitted) {
            impl_->state->clear(false);
            impl_->state->fail(GpuOperationBatchCode::Cancelled, "batch output cancelled");
        }
    }
}
} // namespace bloom::render
