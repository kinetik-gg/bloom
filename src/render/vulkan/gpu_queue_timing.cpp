#include "gpu_queue_timing_private.hpp"

#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace bloom::render::queue_timing_detail {
namespace {
void retire(State* state) noexcept {
    if (state->owner()) {
        state->close(true);
        state->poll();
        if (!state->pending()) {
            delete state;
            return;
        }
    }
    // Final foreign release never calls Vulkan. Keep this exact owner and generation alive; its
    // slot remains unavailable until a later owner-thread create/drain proves fence retirement.
    state->control->retiredQueueTiming.store(state, std::memory_order_release);
}
} // namespace
bool State::owner() const noexcept {
    return control && control->owner == std::this_thread::get_id() &&
           control->ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken();
}
State::~State() {
    const auto* api = control->device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*control->device);
    for (const VkFence fence : fences)
        if (fence != VK_NULL_HANDLE)
            api->vkDestroyFence(device, fence, nullptr);
    if (pool != VK_NULL_HANDLE)
        api->vkDestroyCommandPool(device, pool, nullptr);
    if (queries != VK_NULL_HANDLE)
        api->vkDestroyQueryPool(device, queries, nullptr);
    if (ownsSlot)
        control->queueTimingOccupied.store(false, std::memory_order_release);
}
bool State::initialize() {
    if (!control->calibratedTimestampsEnabled) {
        result.status = GpuQueueTimingStatus::Unsupported;
        return false;
    }
    const auto families = control->physicalDevice.getQueueFamilyProperties();
    if (control->computeQueueFamily >= families.size())
        return false;
    result.timestampValidBits = families[control->computeQueueFamily].timestampValidBits;
    result.timestampPeriodNanoseconds =
        static_cast<double>(control->physicalDevice.getProperties().limits.timestampPeriod);
    if (result.timestampValidBits < 36 || result.timestampValidBits > 64 ||
        !std::isfinite(result.timestampPeriodNanoseconds) ||
        result.timestampPeriodNanoseconds <= 0) {
        result.status = GpuQueueTimingStatus::Unsupported;
        return false;
    }
    const auto* api = control->device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*control->device);
    const VkQueryPoolCreateInfo queryInfo{
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, 2, 0};
    const VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                           VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                           control->computeQueueFamily};
    ++control->nativeQueueTimingBackingAttempts;
    if (api->vkCreateQueryPool(device, &queryInfo, nullptr, &queries) != VK_SUCCESS ||
        api->vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        return false;
    const VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    if (api->vkAllocateCommandBuffers(device, &allocation, commands.data()) != VK_SUCCESS)
        return false;
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    for (auto& fence : fences)
        if (api->vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
            return false;
    return true;
}
bool State::submit(const std::size_t endpoint) noexcept {
    const auto* api = control->device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*control->device);
    const VkCommandBuffer command = commands[endpoint];
    VkResult status = api->vkResetCommandBuffer(command, 0);
    if (status == VK_SUCCESS)
        status = api->vkResetFences(device, 1, &fences[endpoint]);
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    if (status == VK_SUCCESS)
        status = api->vkBeginCommandBuffer(command, &begin);
    if (status == VK_SUCCESS) {
        if (endpoint == 0)
            api->vkCmdResetQueryPool(command, queries, 0, 2);
        api->vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries,
                                 static_cast<std::uint32_t>(endpoint));
        // Execution dependency keeps subsequent production work after the opening marker. The
        // closing BOTTOM_OF_PIPE marker includes all preceding work in queue submission order.
        if (endpoint == 0)
            api->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                                      0, nullptr);
        status = api->vkEndCommandBuffer(command);
    }
    if (status == VK_SUCCESS) {
        const VkSubmitInfo submitInfo{
            VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command, 0, nullptr};
        status = api->vkQueueSubmit(static_cast<VkQueue>(*control->computeQueue), 1, &submitInfo,
                                    fences[endpoint]);
    }
    if (status == VK_SUCCESS) {
        if (endpoint == 0)
            submittedStart = true;
        else
            submittedEnd = true;
        return true;
    }
    lost = status == VK_ERROR_DEVICE_LOST;
    result.status = GpuQueueTimingStatus::Failed;
    return false;
}
void State::close(const bool discard) noexcept {
    cancelled = cancelled || discard;
    if (submittedStart && !submittedEnd && !lost) {
        if (submit(1))
            result.status = GpuQueueTimingStatus::Pending;
    }
}
void State::poll() noexcept {
    if (!pending() || (!submittedEnd && result.status == GpuQueueTimingStatus::Active))
        return;
    const auto* api = control->device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*control->device);
    // End submission follows start on the same queue. An end fence proves both the query reset
    // and both timestamp writes completed, so old availability can never masquerade as a result.
    const VkFence fence = submittedEnd ? fences[1] : fences[0];
    const VkResult status = lost                                      ? VK_ERROR_DEVICE_LOST
                            : pollFaultForTests == PollFault::Pending ? VK_NOT_READY
                            : pollFaultForTests == PollFault::Unknown
                                ? VK_ERROR_UNKNOWN
                                : api->vkGetFenceStatus(device, fence);
    if (status == VK_NOT_READY)
        return;
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        result.status = GpuQueueTimingStatus::Failed;
        cancelled = true;
        if (const auto delivery = receipt.lock())
            delivery->result = result;
        return; // Unknown completion retains every native object and the bounded owner slot.
    }
    lost = lost || status == VK_ERROR_DEVICE_LOST;
    submittedStart = false;
    const bool complete = std::exchange(submittedEnd, false);
    if (lost || !complete) {
        result.status = GpuQueueTimingStatus::Failed;
    } else if (cancelled) {
        result.status = GpuQueueTimingStatus::Cancelled;
    } else {
        std::array<std::uint64_t, 4> values{};
        const auto query = api->vkGetQueryPoolResults(
            device, queries, 0, 2, sizeof(values), values.data(), 2 * sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const auto hostElapsed = std::chrono::steady_clock::now() - started;
        const long double wrapNanoseconds =
            std::ldexp(1.0L, static_cast<int>(result.timestampValidBits)) *
            static_cast<long double>(result.timestampPeriodNanoseconds);
        // The host clock is ONLY a wrap-ambiguity rejection bound, never a GPU time substitute.
        const bool ambiguous =
            std::chrono::duration<long double, std::nano>(hostElapsed).count() >= wrapNanoseconds;
        const auto mask = result.timestampValidBits == 64
                              ? std::numeric_limits<std::uint64_t>::max()
                              : (std::uint64_t{1} << result.timestampValidBits) - 1;
        const long double elapsed = static_cast<long double>((values[2] - values[0]) & mask) *
                                    static_cast<long double>(result.timestampPeriodNanoseconds);
        if (query != VK_SUCCESS || values[1] == 0 || values[3] == 0 || ambiguous ||
            !std::isfinite(elapsed) || elapsed >= std::numeric_limits<std::uint64_t>::max()) {
            result.status = GpuQueueTimingStatus::Failed;
        } else {
            result.nanoseconds = static_cast<std::uint64_t>(elapsed);
            result.status = GpuQueueTimingStatus::Available;
        }
    }
    active = 0;
    if (const auto delivery = receipt.lock())
        delivery->result = result;
}
void drainRetired(vulkan_detail::DeviceAllocatorState& control) noexcept {
    auto* const retired = control.retiredQueueTiming.load(std::memory_order_acquire);
    if (!retired || !retired->owner())
        return;
    retired->close(true);
    retired->poll();
    if (!retired->pending()) {
        control.retiredQueueTiming.store(nullptr, std::memory_order_release);
        delete retired;
    }
}
} // namespace bloom::render::queue_timing_detail

namespace bloom::render {
struct GpuQueueTiming::Impl final {
    std::shared_ptr<queue_timing_detail::State> state;
    std::shared_ptr<queue_timing_detail::Receipt> receipt =
        std::make_shared<queue_timing_detail::Receipt>();
    std::uint64_t serial = 0;
};
GpuQueueTiming::GpuQueueTiming(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuQueueTiming::GpuQueueTiming(GpuQueueTiming&&) noexcept = default;
GpuQueueTiming& GpuQueueTiming::operator=(GpuQueueTiming&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuQueueTiming::~GpuQueueTiming() { releaseImpl(); }
void GpuQueueTiming::releaseImpl() noexcept {
    if (impl_ && impl_->state && impl_->state->owner()) {
        cancel();
        static_cast<void>(poll());
    }
    impl_.reset();
}
std::unique_ptr<GpuQueueTiming> GpuQueueTiming::create(GpuDevice& device) noexcept {
    try {
        auto impl = std::make_unique<Impl>();
        if (device.isOwnerThread() && device.state() == GpuDeviceState::Ready) {
            auto control = GpuRendererAccess::state(device);
            queue_timing_detail::drainRetired(*control);
            impl->state = control->queueTiming.lock();
            if (!impl->state) {
                auto owned = std::make_unique<queue_timing_detail::State>();
                owned->control = std::move(control);
                bool available = false;
                if (owned->control->queueTimingOccupied.compare_exchange_strong(
                        available, true, std::memory_order_acq_rel)) {
                    owned->ownsSlot = true;
                    auto state = std::shared_ptr<queue_timing_detail::State>(
                        owned.release(), queue_timing_detail::retire);
                    if (state->initialize()) {
                        state->control->queueTiming = state;
                        impl->state = std::move(state);
                    } else {
                        impl->receipt->result = state->result;
                        if (impl->receipt->result.status != GpuQueueTimingStatus::Unsupported)
                            impl->receipt->result.status = GpuQueueTimingStatus::Failed;
                    }
                } else {
                    impl->receipt->result.status = GpuQueueTimingStatus::Busy;
                }
            }
            if (impl->state) {
                impl->receipt->result = impl->state->result;
                impl->receipt->result.status = GpuQueueTimingStatus::Idle;
                impl->receipt->result.nanoseconds = 0;
            }
        } else if (!device.isOwnerThread()) {
            impl->receipt->result.status = GpuQueueTimingStatus::WrongThread;
        }
        return std::unique_ptr<GpuQueueTiming>(new GpuQueueTiming(std::move(impl)));
    } catch (...) {
        return {};
    }
}
GpuQueueTimingStatus GpuQueueTiming::begin() noexcept {
    if (!impl_ || !impl_->state)
        return sample().status;
    auto& state = *impl_->state;
    if (!state.owner())
        return GpuQueueTimingStatus::WrongThread;
    state.poll();
    if (state.active != 0 || state.pending()) {
        impl_->receipt->result = {GpuQueueTimingStatus::Busy};
        return impl_->receipt->result.status;
    }
    if (state.lost || state.control->imageOwnerClosed) {
        impl_->receipt->result = {GpuQueueTimingStatus::Failed};
        return impl_->receipt->result.status;
    }
    state.result.status = GpuQueueTimingStatus::Active;
    state.result.nanoseconds = 0;
    state.cancelled = false;
    state.started = std::chrono::steady_clock::now();
    impl_->serial = ++state.serial;
    state.active = impl_->serial;
    state.receipt = impl_->receipt;
    if (!state.submit(0))
        state.active = 0;
    impl_->receipt->result = state.result;
    return impl_->receipt->result.status;
}
void GpuQueueTiming::end() noexcept {
    if (impl_ && impl_->state && impl_->state->owner() && impl_->serial != 0 &&
        impl_->state->active == impl_->serial)
        impl_->state->close(false);
}
void GpuQueueTiming::cancel() noexcept {
    if (impl_ && impl_->state && impl_->state->owner() && impl_->serial != 0 &&
        impl_->state->active == impl_->serial)
        impl_->state->close(true);
}
GpuQueueTimingSample GpuQueueTiming::poll() noexcept {
    if (!impl_ || !impl_->state)
        return sample();
    if (!impl_->state->owner())
        return {GpuQueueTimingStatus::WrongThread};
    auto& state = *impl_->state;
    if (impl_->serial != 0 && (state.active == impl_->serial || state.serial == impl_->serial)) {
        state.poll();
        impl_->receipt->result = state.result;
    }
    return impl_->receipt->result;
}
GpuQueueTimingSample GpuQueueTiming::sample() const noexcept {
    return impl_ ? impl_->receipt->result : GpuQueueTimingSample{};
}
bool GpuQueueTiming::hasUnretiredSubmission() const noexcept {
    return impl_ && impl_->state && impl_->serial != 0 && impl_->state->active == impl_->serial &&
           impl_->state->pending();
}
} // namespace bloom::render
