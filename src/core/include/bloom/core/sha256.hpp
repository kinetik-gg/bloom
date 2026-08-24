#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bloom::core {

inline constexpr std::size_t kSha256DigestBytes = 32;
inline constexpr std::size_t kSha256HexCharacters = kSha256DigestBytes * 2;

class Sha256Digest final {
  public:
    using Bytes = std::array<std::uint8_t, kSha256DigestBytes>;
    using LowercaseHex = std::array<char, kSha256HexCharacters>;

    constexpr Sha256Digest() noexcept = default;

    [[nodiscard]] static constexpr Sha256Digest fromBytes(const Bytes bytes) noexcept {
        return Sha256Digest(bytes);
    }
    [[nodiscard]] static std::optional<Sha256Digest>
    fromLowercaseHex(std::string_view text) noexcept;

    [[nodiscard]] constexpr std::span<const std::uint8_t, kSha256DigestBytes>
    bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] LowercaseHex toLowercaseHex() const noexcept;

    friend constexpr auto operator<=>(const Sha256Digest&, const Sha256Digest&) noexcept = default;

  private:
    explicit constexpr Sha256Digest(const Bytes bytes) noexcept : bytes_(bytes) {}

    Bytes bytes_{};
};

class Sha256Hasher final {
  public:
    constexpr Sha256Hasher() noexcept = default;

    // SHA-256's encoded bit length limits one message to fewer than 2^64 bits. An oversized update
    // is rejected before changing the hash state; the hasher remains usable.
    [[nodiscard]] bool update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] Sha256Digest finalize() const noexcept;

    [[nodiscard]] static std::optional<Sha256Digest>
    hash(std::span<const std::byte> bytes) noexcept;

  private:
    void compress(const std::array<std::byte, 64>& block) noexcept;

    std::array<std::uint32_t, 8> state_{
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
    };
    std::array<std::byte, 64> bufferedBytes_{};
    std::uint64_t totalBytes_ = 0;
    std::size_t bufferedByteCount_ = 0;
};

} // namespace bloom::core
