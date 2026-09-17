#pragma once

#include <compare>
#include <cstddef>

namespace bloom::runtime {

// A stable, plan-local address for one image operation. Kept in its own small header because the
// value graph's post-image bounds readout refers to image operations while the plan header owns the
// operation definitions themselves.
class OperationIndex final {
  public:
    [[nodiscard]] static constexpr OperationIndex fromRaw(const std::size_t value) noexcept {
        return OperationIndex(value);
    }

    [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }
    friend constexpr auto operator<=>(const OperationIndex&,
                                      const OperationIndex&) noexcept = default;

  private:
    explicit constexpr OperationIndex(const std::size_t value) noexcept : value_(value) {}

    std::size_t value_ = 0;
};

} // namespace bloom::runtime
