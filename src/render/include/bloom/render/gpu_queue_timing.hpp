#pragma once

#include <cstdint>
#include <memory>

namespace bloom::render {
class GpuDevice;

enum class GpuQueueTimingStatus : std::uint8_t {
    NotRequested,
    Unsupported,
    Idle,
    Active,
    Pending,
    Available,
    Busy,
    Cancelled,
    Failed,
    WrongThread,
};

// Genuine device-clock interval between two ordered markers on the production compute queue.
// The interval includes queue idle time and host polling/submission gaps between those markers;
// it is never shader-only time. nanoseconds is populated ONLY for Available. Opaque driver query,
// command-pool and fence memory is bounded to one owner per device and has no VMA byte estimate.
struct GpuQueueTimingSample final {
    GpuQueueTimingStatus status = GpuQueueTimingStatus::NotRequested;
    std::uint64_t nanoseconds = 0;
    std::uint32_t timestampValidBits = 0;
    double timestampPeriodNanoseconds = 0;
};

// Optional, nonblocking owner-thread diagnostics. Facades on the same device share one serial
// query owner: a second span returns Busy until the first retires. No result is needed to publish
// pixels. Unsupported/failed timing never changes production rendering admission or execution.
class GpuQueueTiming final {
  public:
    [[nodiscard]] static std::unique_ptr<GpuQueueTiming> create(GpuDevice& device) noexcept;
    GpuQueueTiming(const GpuQueueTiming&) = delete;
    GpuQueueTiming& operator=(const GpuQueueTiming&) = delete;
    GpuQueueTiming(GpuQueueTiming&&) noexcept;
    GpuQueueTiming& operator=(GpuQueueTiming&&) noexcept;
    ~GpuQueueTiming();

    [[nodiscard]] GpuQueueTimingStatus begin() noexcept;
    void end() noexcept;
    void cancel() noexcept;
    [[nodiscard]] GpuQueueTimingSample poll() noexcept;
    [[nodiscard]] GpuQueueTimingSample sample() const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;

  private:
    struct Impl;
    explicit GpuQueueTiming(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace bloom::render
