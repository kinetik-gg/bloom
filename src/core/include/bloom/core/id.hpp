#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>

namespace bloom::core {

template <typename Tag> class Id final {
  public:
    using Value = std::uint64_t;

    constexpr Id() noexcept = default;

    [[nodiscard]] static constexpr Id fromRaw(Value value) noexcept { return Id(value); }

    [[nodiscard]] constexpr Value value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return isValid(); }

    friend constexpr auto operator<=>(const Id&, const Id&) noexcept = default;

  private:
    explicit constexpr Id(Value value) noexcept : value_(value) {}

    Value value_ = 0;
};

template <typename T>
concept TypedId = requires(const T id) {
    { id.value() } -> std::same_as<std::uint64_t>;
    { id.isValid() } -> std::same_as<bool>;
};

} // namespace bloom::core

namespace std {

template <typename Tag> struct hash<bloom::core::Id<Tag>> {
    [[nodiscard]] std::size_t operator()(const bloom::core::Id<Tag> id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

} // namespace std
