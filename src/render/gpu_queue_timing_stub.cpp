#include <bloom/render/gpu_queue_timing.hpp>

#include <new>
#include <utility>

namespace bloom::render {
struct GpuQueueTiming::Impl final {};
GpuQueueTiming::GpuQueueTiming(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuQueueTiming::GpuQueueTiming(GpuQueueTiming&&) noexcept = default;
GpuQueueTiming& GpuQueueTiming::operator=(GpuQueueTiming&&) noexcept = default;
GpuQueueTiming::~GpuQueueTiming() = default;
void GpuQueueTiming::releaseImpl() noexcept { impl_.reset(); }
std::unique_ptr<GpuQueueTiming> GpuQueueTiming::create(GpuDevice&) noexcept {
    try {
        return std::unique_ptr<GpuQueueTiming>(new GpuQueueTiming(std::make_unique<Impl>()));
    } catch (...) {
        return {};
    }
}
GpuQueueTimingStatus GpuQueueTiming::begin() noexcept { return GpuQueueTimingStatus::Unsupported; }
void GpuQueueTiming::end() noexcept {}
void GpuQueueTiming::cancel() noexcept {}
GpuQueueTimingSample GpuQueueTiming::poll() noexcept { return sample(); }
GpuQueueTimingSample GpuQueueTiming::sample() const noexcept {
    return {GpuQueueTimingStatus::Unsupported};
}
bool GpuQueueTiming::hasUnretiredSubmission() const noexcept { return false; }
} // namespace bloom::render
