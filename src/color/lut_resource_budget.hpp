#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

namespace bloom::color::detail {
// Conservative byte reservations cover vectors, sealed copies, scratch and opaque handles.
// Each reservation is <= 64 MiB; a file request holds <= 193 MiB including its pixel slabs.
class LutReservation final {
  public:
    static std::shared_ptr<const LutReservation> acquire(const std::size_t bytes) {
        auto current = resident_.load();
        do {
            if (bytes > limit_ - current)
                return {};
        } while (!resident_.compare_exchange_weak(current, current + bytes));
        try {
            return std::shared_ptr<const LutReservation>(new LutReservation(bytes));
        } catch (...) {
            resident_.fetch_sub(bytes);
            throw;
        }
    }
    ~LutReservation() { resident_.fetch_sub(bytes_); }
    LutReservation(const LutReservation&) = delete;
    LutReservation& operator=(const LutReservation&) = delete;

  private:
    explicit LutReservation(const std::size_t bytes) : bytes_(bytes) {}
    const std::size_t bytes_;
    static constexpr std::size_t limit_ = std::size_t{768} * 1024U * 1024U;
    inline static std::atomic<std::size_t> resident_{0};
};
} // namespace bloom::color::detail
