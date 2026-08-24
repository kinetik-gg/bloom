#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <bit>
#include <limits>

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U,
};

[[nodiscard]] constexpr std::uint32_t choose(const std::uint32_t x, const std::uint32_t y,
                                             const std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr std::uint32_t majority(const std::uint32_t x, const std::uint32_t y,
                                               const std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] constexpr std::uint32_t bigSigma0(const std::uint32_t value) noexcept {
    return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
}

[[nodiscard]] constexpr std::uint32_t bigSigma1(const std::uint32_t value) noexcept {
    return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
}

[[nodiscard]] constexpr std::uint32_t smallSigma0(const std::uint32_t value) noexcept {
    return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3U);
}

[[nodiscard]] constexpr std::uint32_t smallSigma1(const std::uint32_t value) noexcept {
    return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10U);
}

[[nodiscard]] std::uint32_t loadBigEndian32(const std::byte* bytes) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]));
}

void storeBigEndian32(const std::uint32_t value, std::uint8_t* destination) noexcept {
    destination[0] = static_cast<std::uint8_t>(value >> 24U);
    destination[1] = static_cast<std::uint8_t>(value >> 16U);
    destination[2] = static_cast<std::uint8_t>(value >> 8U);
    destination[3] = static_cast<std::uint8_t>(value);
}

void storeBigEndian64(const std::uint64_t value, std::byte* destination) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned>((7 - index) * 8);
        destination[index] = static_cast<std::byte>(value >> shift);
    }
}

[[nodiscard]] std::optional<std::uint8_t> decodeLowercaseHex(const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    return std::nullopt;
}

} // namespace

namespace bloom::core {

std::optional<Sha256Digest> Sha256Digest::fromLowercaseHex(const std::string_view text) noexcept {
    if (text.size() != kSha256HexCharacters) {
        return std::nullopt;
    }

    Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = decodeLowercaseHex(text[index * 2]);
        const auto low = decodeLowercaseHex(text[index * 2 + 1]);
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;
        }
        bytes[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return Sha256Digest(bytes);
}

Sha256Digest::LowercaseHex Sha256Digest::toLowercaseHex() const noexcept {
    constexpr std::string_view digits = "0123456789abcdef";
    LowercaseHex result{};
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        result[index * 2] = digits[bytes_[index] >> 4U];
        result[index * 2 + 1] = digits[bytes_[index] & 0x0FU];
    }
    return result;
}

bool Sha256Hasher::update(const std::span<const std::byte> bytes) noexcept {
    constexpr auto maximumMessageBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
    if (bytes.size() > maximumMessageBytes - totalBytes_) {
        return false;
    }

    totalBytes_ += bytes.size();
    auto remaining = bytes;
    if (bufferedByteCount_ != 0) {
        const auto copyCount =
            std::min(remaining.size(), bufferedBytes_.size() - bufferedByteCount_);
        std::copy_n(remaining.begin(), copyCount, bufferedBytes_.begin() + bufferedByteCount_);
        bufferedByteCount_ += copyCount;
        remaining = remaining.subspan(copyCount);
        if (bufferedByteCount_ != bufferedBytes_.size()) {
            return true;
        }
        compress(bufferedBytes_);
        bufferedByteCount_ = 0;
    }

    while (remaining.size() >= bufferedBytes_.size()) {
        std::array<std::byte, 64> block{};
        std::copy_n(remaining.begin(), block.size(), block.begin());
        compress(block);
        remaining = remaining.subspan(block.size());
    }
    std::copy(remaining.begin(), remaining.end(), bufferedBytes_.begin());
    bufferedByteCount_ = remaining.size();
    return true;
}

Sha256Digest Sha256Hasher::finalize() const noexcept {
    auto final = *this;
    final.bufferedBytes_[final.bufferedByteCount_++] = std::byte{0x80};
    if (final.bufferedByteCount_ > 56) {
        std::fill(final.bufferedBytes_.begin() + final.bufferedByteCount_,
                  final.bufferedBytes_.end(), std::byte{0});
        final.compress(final.bufferedBytes_);
        final.bufferedByteCount_ = 0;
    }
    std::fill(final.bufferedBytes_.begin() + final.bufferedByteCount_,
              final.bufferedBytes_.begin() + 56, std::byte{0});
    storeBigEndian64(final.totalBytes_ * 8U, final.bufferedBytes_.data() + 56);
    final.compress(final.bufferedBytes_);

    Sha256Digest::Bytes digestBytes{};
    for (std::size_t index = 0; index < final.state_.size(); ++index) {
        storeBigEndian32(final.state_[index], digestBytes.data() + index * 4);
    }
    return Sha256Digest::fromBytes(digestBytes);
}

std::optional<Sha256Digest> Sha256Hasher::hash(const std::span<const std::byte> bytes) noexcept {
    Sha256Hasher hasher;
    if (!hasher.update(bytes)) {
        return std::nullopt;
    }
    return hasher.finalize();
}

void Sha256Hasher::compress(const std::array<std::byte, 64>& block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
        schedule[index] = loadBigEndian32(block.data() + index * 4);
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
        schedule[index] = smallSigma1(schedule[index - 2]) + schedule[index - 7] +
                          smallSigma0(schedule[index - 15]) + schedule[index - 16];
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const auto first =
            h + bigSigma1(e) + choose(e, f, g) + kRoundConstants[index] + schedule[index];
        const auto second = bigSigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

} // namespace bloom::core
