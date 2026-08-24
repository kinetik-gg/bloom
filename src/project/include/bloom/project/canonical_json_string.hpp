#ifndef BLOOM_PROJECT_CANONICAL_JSON_STRING_HPP
#define BLOOM_PROJECT_CANONICAL_JSON_STRING_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::project {

enum class CanonicalJsonStringError : std::uint8_t {
    None,
    InvalidUtf8,
    SizeOverflow,
    OutputSizeMismatch,
};

class [[nodiscard]] CanonicalJsonStringSizeResult final {
  public:
    [[nodiscard]] static constexpr CanonicalJsonStringSizeResult
    success(const std::size_t size) noexcept {
        return CanonicalJsonStringSizeResult(size);
    }

    [[nodiscard]] static constexpr CanonicalJsonStringSizeResult
    failure(const CanonicalJsonStringError error) noexcept {
        return CanonicalJsonStringSizeResult(error);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept { return size_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const std::size_t* value() const& noexcept {
        return size_.has_value() ? &*size_ : nullptr;
    }
    [[nodiscard]] constexpr const std::size_t* value() const&& = delete;
    [[nodiscard]] constexpr CanonicalJsonStringError error() const noexcept { return error_; }

  private:
    constexpr explicit CanonicalJsonStringSizeResult(const std::size_t size) noexcept
        : size_(size) {}
    constexpr explicit CanonicalJsonStringSizeResult(const CanonicalJsonStringError error) noexcept
        : error_(error) {}

    std::optional<std::size_t> size_;
    CanonicalJsonStringError error_ = CanonicalJsonStringError::None;
};

class [[nodiscard]] CanonicalJsonStringWriteResult final {
  public:
    [[nodiscard]] static constexpr CanonicalJsonStringWriteResult
    success(const std::size_t requiredSize) noexcept {
        return CanonicalJsonStringWriteResult(CanonicalJsonStringError::None, requiredSize,
                                              requiredSize);
    }

    [[nodiscard]] static constexpr CanonicalJsonStringWriteResult
    failure(const CanonicalJsonStringError error,
            const std::optional<std::size_t> requiredSize = std::nullopt) noexcept {
        return CanonicalJsonStringWriteResult(error, requiredSize, 0);
    }

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error_ == CanonicalJsonStringError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr CanonicalJsonStringError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::optional<std::size_t> requiredSize() const noexcept {
        return requiredSize_;
    }
    [[nodiscard]] constexpr std::size_t bytesWritten() const noexcept { return bytesWritten_; }

  private:
    constexpr CanonicalJsonStringWriteResult(const CanonicalJsonStringError error,
                                             const std::optional<std::size_t> requiredSize,
                                             const std::size_t bytesWritten) noexcept
        : requiredSize_(requiredSize), bytesWritten_(bytesWritten), error_(error) {}

    std::optional<std::size_t> requiredSize_;
    std::size_t bytesWritten_ = 0;
    CanonicalJsonStringError error_ = CanonicalJsonStringError::None;
};

// Returns the exact byte count for a complete JSON string token, including surrounding quotes.
[[nodiscard]] CanonicalJsonStringSizeResult
canonicalJsonStringTokenSize(std::string_view value) noexcept;

// The destination must have exactly the preflight size. Every failure leaves it untouched and
// reports zero bytes written. Successful output has no trailing NUL byte.
[[nodiscard]] CanonicalJsonStringWriteResult
encodeCanonicalJsonStringToken(std::string_view value, std::span<char> output) noexcept;

static_assert(std::is_trivially_copyable_v<CanonicalJsonStringSizeResult>);
static_assert(std::is_trivially_copyable_v<CanonicalJsonStringWriteResult>);

} // namespace bloom::project

#endif // BLOOM_PROJECT_CANONICAL_JSON_STRING_HPP
