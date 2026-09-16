#pragma once
#include <bit>
#include <bloom/core/sha256.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace bloom::runtime::detail {
// Process-local key encoding. No padding, locale, pointer identity or lossy float formatting.
class OperationKey final {
  public:
    template <typename T> void add(const T& value) {
        if constexpr (std::is_enum_v<T>)
            add(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (std::is_integral_v<T>) {
            const auto bits = static_cast<std::uint64_t>(value);
            for (unsigned shift = 0; shift < 64; shift += 8)
                bytes_.push_back(static_cast<char>((bits >> shift) & 255));
        } else if constexpr (std::is_same_v<T, double>)
            add(std::bit_cast<std::uint64_t>(value));
        else if constexpr (requires { value.value(); })
            add(value.value());
        else if constexpr (requires { value.numerator(); }) {
            add(value.numerator());
            add(value.denominator());
        } else if constexpr (requires { value.red; }) {
            add(value.red);
            add(value.green);
            add(value.blue);
            add(value.alpha);
        } else if constexpr (requires { value.x; }) {
            add(value.x);
            add(value.y);
            if constexpr (requires { value.z; })
                add(value.z);
        } else if constexpr (requires { value.anchors; }) {
            add(value.closed);
            add(value.anchors.size());
            for (const auto& anchor : value.anchors) {
                add(anchor.point);
                add(anchor.inHandle.has_value());
                if (anchor.inHandle)
                    add(*anchor.inHandle);
                add(anchor.outHandle.has_value());
                if (anchor.outHandle)
                    add(*anchor.outHandle);
            }
        } else if constexpr (std::is_same_v<T, std::string>) {
            add(value.size());
            bytes_ += value;
        } else {
            add(value.index());
            std::visit([this](const auto& item) { this->add(item); }, value);
        }
    }
    [[nodiscard]] const std::string& bytes() const { return bytes_; }
    [[nodiscard]] std::string digest() const {
        const auto hash = core::Sha256Hasher::hash(std::as_bytes(std::span(bytes_)));
        if (!hash)
            throw std::length_error("Operation key exceeds SHA-256 input limit");
        const auto hex = hash->toLowercaseHex();
        return {hex.begin(), hex.end()};
    }

  private:
    std::string bytes_;
};
} // namespace bloom::runtime::detail
