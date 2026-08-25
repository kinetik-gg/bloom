#include <bloom/runtime/task_gpu_executor.hpp>

#include "task_scheduler_internal.hpp"

#include <utility>

namespace bloom::runtime {

GpuExecutorLease::GpuExecutorLease(std::weak_ptr<detail::SchedulerState> scheduler,
                                   const GpuServiceGeneration generation,
                                   const std::uint64_t attachmentId) noexcept
    : scheduler_(std::move(scheduler)), generation_(generation), attachmentId_(attachmentId) {}

GpuExecutorLease::GpuExecutorLease(GpuExecutorLease&& other) noexcept
    : scheduler_(std::move(other.scheduler_)), generation_(other.generation_),
      attachmentId_(std::exchange(other.attachmentId_, 0)) {
    other.generation_ = {};
}

GpuExecutorLease& GpuExecutorLease::operator=(GpuExecutorLease&& other) noexcept {
    if (this != &other) {
        detach();
        scheduler_ = std::move(other.scheduler_);
        generation_ = other.generation_;
        attachmentId_ = std::exchange(other.attachmentId_, 0);
        other.generation_ = {};
    }
    return *this;
}

GpuExecutorLease::~GpuExecutorLease() { detach(); }

bool GpuExecutorLease::isValid() const noexcept {
    if (attachmentId_ == 0 || !generation_.isValid()) {
        return false;
    }
    const auto scheduler = scheduler_.lock();
    return scheduler != nullptr && scheduler->gpuAttachmentActive(generation_, attachmentId_);
}

GpuServiceGeneration GpuExecutorLease::generation() const noexcept { return generation_; }

GpuDispatchStatus GpuExecutorLease::dispatchOne() noexcept {
    if (const auto scheduler = scheduler_.lock()) {
        return scheduler->dispatchGpu(generation_, attachmentId_);
    }
    return GpuDispatchStatus::ExecutorUnavailable;
}

bool GpuExecutorLease::reportDeviceLost(TaskDiagnostic diagnostic) noexcept {
    if (const auto scheduler = scheduler_.lock()) {
        const bool reported =
            scheduler->reportGpuDeviceLost(generation_, attachmentId_, std::move(diagnostic));
        if (reported) {
            attachmentId_ = 0;
            generation_ = {};
            scheduler_.reset();
        }
        return reported;
    }
    return false;
}

bool GpuExecutorLease::forceShutdownFallback() noexcept {
    if (const auto scheduler = scheduler_.lock()) {
        const bool forced = scheduler->forceGpuShutdown(generation_, attachmentId_);
        if (forced) {
            attachmentId_ = 0;
            generation_ = {};
            scheduler_.reset();
        }
        return forced;
    }
    return false;
}

void GpuExecutorLease::detach() noexcept {
    if (attachmentId_ != 0) {
        if (const auto scheduler = scheduler_.lock()) {
            scheduler->detachGpu(generation_, attachmentId_);
        }
    }
    attachmentId_ = 0;
    generation_ = {};
    scheduler_.reset();
}

} // namespace bloom::runtime
