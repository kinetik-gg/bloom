#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace bloom::media::detail {
using ImageWorkClock = std::chrono::steady_clock;
inline void addImageWork(std::uint64_t& total, const std::uint64_t value) noexcept {
    const auto remaining = std::numeric_limits<std::uint64_t>::max() - total;
    total += value > remaining ? remaining : value;
}
inline std::uint64_t imageWorkNanoseconds(const ImageWorkClock::time_point start) noexcept {
    const auto elapsed =
        std::chrono::duration<long double, std::nano>(ImageWorkClock::now() - start).count();
    if (elapsed <= 0)
        return 0;
    if (elapsed >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(elapsed);
}
class ImageWorkTimer final {
  public:
    explicit ImageWorkTimer(std::uint64_t* destination) noexcept
        : destination_(destination), start_(ImageWorkClock::now()) {}
    ~ImageWorkTimer() {
        if (destination_)
            addImageWork(*destination_, imageWorkNanoseconds(start_));
    }

  private:
    std::uint64_t* destination_;
    ImageWorkClock::time_point start_;
};
} // namespace bloom::media::detail
