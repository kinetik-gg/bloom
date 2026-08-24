#ifndef BLOOM_CORE_FLOATING_POINT_HPP
#define BLOOM_CORE_FLOATING_POINT_HPP

#include <cfenv>
#include <concepts>
#include <limits>

namespace bloom::core {

template <typename T>
concept ReferenceFloat = std::same_as<T, float> || std::same_as<T, double>;

// Bloom's CPU reference kernels require round-to-nearest and preserved IEEE-754 subnormal inputs
// and results. The volatile arithmetic probes reject per-thread FTZ/DAZ modes before authored
// arithmetic is evaluated.
template <ReferenceFloat T>
[[nodiscard]] bool supportsReferenceFloatingPointEnvironment() noexcept {
    if (std::fegetround() != FE_TONEAREST) {
        return false;
    }

    volatile const T minimumNormal = std::numeric_limits<T>::min();
    volatile const T half = T{0.5};
    volatile const T producedSubnormal = minimumNormal * half;
    volatile const T minimumSubnormal = std::numeric_limits<T>::denorm_min();
    volatile const T one = T{1};
    volatile const T consumedSubnormal = minimumSubnormal * one;
    return producedSubnormal != T{0} && consumedSubnormal != T{0};
}

static_assert(std::numeric_limits<float>::is_iec559,
              "Bloom Float32 reference kernels require IEEE-754 behavior");
static_assert(std::numeric_limits<double>::is_iec559,
              "Bloom Float64 reference kernels require IEEE-754 behavior");
static_assert(std::numeric_limits<float>::has_denorm == std::denorm_present,
              "Bloom Float32 reference kernels require subnormal support");
static_assert(std::numeric_limits<double>::has_denorm == std::denorm_present,
              "Bloom Float64 reference kernels require subnormal support");

} // namespace bloom::core

#endif // BLOOM_CORE_FLOATING_POINT_HPP
