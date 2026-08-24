#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::project {

enum class CanonicalBase64Error : std::uint8_t {
    None,
    SizeOverflow,
    InvalidEncodedLength,
    InvalidAlphabet,
    InvalidPadding,
    NonZeroTailBits,
    OutputSizeMismatch,
};

class [[nodiscard]] CanonicalBase64SizeResult final {
  public:
    [[nodiscard]] static constexpr CanonicalBase64SizeResult
    success(const std::size_t size) noexcept {
        return CanonicalBase64SizeResult(size);
    }

    [[nodiscard]] static constexpr CanonicalBase64SizeResult
    failure(const CanonicalBase64Error error) noexcept {
        return CanonicalBase64SizeResult(error);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept { return size_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const std::size_t* value() const& noexcept {
        return size_.has_value() ? &*size_ : nullptr;
    }
    [[nodiscard]] constexpr const std::size_t* value() const&& = delete;
    [[nodiscard]] constexpr CanonicalBase64Error error() const noexcept { return error_; }

  private:
    constexpr explicit CanonicalBase64SizeResult(const std::size_t size) noexcept : size_(size) {}
    constexpr explicit CanonicalBase64SizeResult(const CanonicalBase64Error error) noexcept
        : error_(error) {}

    std::optional<std::size_t> size_;
    CanonicalBase64Error error_ = CanonicalBase64Error::None;
};

class [[nodiscard]] CanonicalBase64WriteResult final {
  public:
    [[nodiscard]] static constexpr CanonicalBase64WriteResult
    success(const std::size_t requiredSize) noexcept {
        return CanonicalBase64WriteResult(CanonicalBase64Error::None, requiredSize, requiredSize);
    }

    [[nodiscard]] static constexpr CanonicalBase64WriteResult
    failure(const CanonicalBase64Error error,
            const std::optional<std::size_t> requiredSize = std::nullopt) noexcept {
        return CanonicalBase64WriteResult(error, requiredSize, 0);
    }

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error_ == CanonicalBase64Error::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr CanonicalBase64Error error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::optional<std::size_t> requiredSize() const noexcept {
        return requiredSize_;
    }
    [[nodiscard]] constexpr std::size_t bytesWritten() const noexcept { return bytesWritten_; }

  private:
    constexpr CanonicalBase64WriteResult(const CanonicalBase64Error error,
                                         const std::optional<std::size_t> requiredSize,
                                         const std::size_t bytesWritten) noexcept
        : requiredSize_(requiredSize), bytesWritten_(bytesWritten), error_(error) {}

    std::optional<std::size_t> requiredSize_;
    std::size_t bytesWritten_ = 0;
    CanonicalBase64Error error_ = CanonicalBase64Error::None;
};

[[nodiscard]] CanonicalBase64SizeResult
canonicalBase64EncodedSize(std::size_t decodedSize) noexcept;

// Validates the complete canonical spelling before returning its exact decoded size.
[[nodiscard]] CanonicalBase64SizeResult
canonicalBase64DecodedSize(std::string_view encoded) noexcept;

// Destination spans must have exactly the preflight size. Every failure leaves the destination
// untouched and reports zero bytes written.
[[nodiscard]] CanonicalBase64WriteResult encodeCanonicalBase64(std::span<const std::byte> decoded,
                                                               std::span<char> encoded) noexcept;
[[nodiscard]] CanonicalBase64WriteResult
decodeCanonicalBase64(std::string_view encoded, std::span<std::byte> decoded) noexcept;

static_assert(std::is_trivially_copyable_v<CanonicalBase64SizeResult>);
static_assert(std::is_trivially_copyable_v<CanonicalBase64WriteResult>);

} // namespace bloom::project
