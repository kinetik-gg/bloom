#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64)
#define BLOOM_SHA256_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define BLOOM_SHA256_X86_TARGET
#else
#include <cpuid.h>
#define BLOOM_SHA256_X86_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#endif
#endif

#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(__ARM_FEATURE_SHA2)
#define BLOOM_SHA256_ARM 1
#include <arm_neon.h>
#endif

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

using State = std::array<std::uint32_t, 8>;

constexpr State kInitialState{
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
    0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

void compressPortable(State& state, const std::byte* data, std::size_t blocks) noexcept {
    for (; blocks != 0; --blocks, data += 64) {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16; ++index) {
            schedule[index] = loadBigEndian32(data + index * 4);
        }
        for (std::size_t index = 16; index < schedule.size(); ++index) {
            schedule[index] = smallSigma1(schedule[index - 2]) + schedule[index - 7] +
                              smallSigma0(schedule[index - 15]) + schedule[index - 16];
        }

        auto a = state[0];
        auto b = state[1];
        auto c = state[2];
        auto d = state[3];
        auto e = state[4];
        auto f = state[5];
        auto g = state[6];
        auto h = state[7];
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

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
}

#if defined(BLOOM_SHA256_X86)
BLOOM_SHA256_X86_TARGET inline __m128i load128(const void* source) noexcept {
    return _mm_loadu_si128(static_cast<const __m128i*>(source));
}
BLOOM_SHA256_X86_TARGET inline void store128(void* destination, const __m128i value) noexcept {
    _mm_storeu_si128(static_cast<__m128i*>(destination), value);
}

// Intel SHA extensions (SHA-NI). The state is kept as ABEF/CDGH lane pairs, the layout
// sha256rnds2 consumes; the message schedule rotates through four registers.
BLOOM_SHA256_X86_TARGET void compressX86(State& state, const std::byte* data,
                                         std::size_t blocks) noexcept {
    const __m128i byteSwap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i swapped = load128(state.data());
    __m128i state1 = load128(state.data() + 4);
    swapped = _mm_shuffle_epi32(swapped, 0xB1);
    state1 = _mm_shuffle_epi32(state1, 0x1B);
    __m128i state0 = _mm_alignr_epi8(swapped, state1, 8);
    state1 = _mm_blend_epi16(state1, swapped, 0xF0);
    for (; blocks != 0; --blocks, data += 64) {
        const __m128i savedAbef = state0;
        const __m128i savedCdgh = state1;
        std::array<__m128i, 4> words{};
        for (int group = 0; group < 16; ++group) {
            auto& current = words[static_cast<std::size_t>(group & 3)];
            if (group < 4) {
                current = _mm_shuffle_epi8(load128(data + group * 16), byteSwap);
            }
            __m128i message = _mm_add_epi32(
                current, load128(kRoundConstants.data() + static_cast<std::size_t>(group) * 4));
            state1 = _mm_sha256rnds2_epu32(state1, state0, message);
            if (group >= 3 && group < 15) {
                auto& next = words[static_cast<std::size_t>((group + 1) & 3)];
                const auto& previous = words[static_cast<std::size_t>((group + 3) & 3)];
                next = _mm_add_epi32(next, _mm_alignr_epi8(current, previous, 4));
                next = _mm_sha256msg2_epu32(next, current);
            }
            message = _mm_shuffle_epi32(message, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, message);
            if (group >= 1 && group < 13) {
                auto& previous = words[static_cast<std::size_t>((group + 3) & 3)];
                previous = _mm_sha256msg1_epu32(previous, current);
            }
        }
        state0 = _mm_add_epi32(state0, savedAbef);
        state1 = _mm_add_epi32(state1, savedCdgh);
    }
    swapped = _mm_shuffle_epi32(state0, 0x1B);
    state1 = _mm_shuffle_epi32(state1, 0xB1);
    state0 = _mm_blend_epi16(swapped, state1, 0xF0);
    state1 = _mm_alignr_epi8(state1, swapped, 8);
    store128(state.data(), state0);
    store128(state.data() + 4, state1);
}

[[nodiscard]] bool x86ShaExtensionsAvailable() noexcept {
    constexpr unsigned kSsse3 = 1U << 9U;
    constexpr unsigned kSse41 = 1U << 19U;
    constexpr unsigned kSha = 1U << 29U;
#if defined(_MSC_VER) && !defined(__clang__)
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 0);
    if (registers[0] < 7) {
        return false;
    }
    __cpuid(registers.data(), 1);
    const auto ecx = static_cast<unsigned>(registers[2]);
    __cpuidex(registers.data(), 7, 0);
    const auto ebx = static_cast<unsigned>(registers[1]);
#else
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7 || __get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    const auto leafOneEcx = ecx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    ecx = leafOneEcx;
#endif
    return (ecx & kSsse3) != 0 && (ecx & kSse41) != 0 && (ebx & kSha) != 0;
}
#endif

#if defined(BLOOM_SHA256_ARM)
// Armv8 SHA2 instructions; the state stays in natural ABCD/EFGH order.
void compressArm(State& state, const std::byte* data, std::size_t blocks) noexcept {
    uint32x4_t state0 = vld1q_u32(state.data());
    uint32x4_t state1 = vld1q_u32(state.data() + 4);
    for (; blocks != 0; --blocks, data += 64) {
        const uint32x4_t savedAbcd = state0;
        const uint32x4_t savedEfgh = state1;
        std::array<uint32x4_t, 4> words{};
        for (std::size_t group = 0; group < 4; ++group) {
            words[group] = vreinterpretq_u32_u8(
                vrev32q_u8(vld1q_u8(reinterpret_cast<const std::uint8_t*>(data + group * 16))));
        }
        for (std::size_t group = 0; group < 16; ++group) {
            auto& current = words[group & 3U];
            const uint32x4_t message =
                vaddq_u32(current, vld1q_u32(kRoundConstants.data() + group * 4));
            if (group < 12) {
                current = vsha256su0q_u32(current, words[(group + 1) & 3U]);
            }
            const uint32x4_t previousAbcd = state0;
            state0 = vsha256hq_u32(state0, state1, message);
            state1 = vsha256h2q_u32(state1, previousAbcd, message);
            if (group < 12) {
                current =
                    vsha256su1q_u32(current, words[(group + 2) & 3U], words[(group + 3) & 3U]);
            }
        }
        state0 = vaddq_u32(state0, savedAbcd);
        state1 = vaddq_u32(state1, savedEfgh);
    }
    vst1q_u32(state.data(), state0);
    vst1q_u32(state.data() + 4, state1);
}
#endif

struct SelectedRounds final {
    void (*compress)(State&, const std::byte*, std::size_t) noexcept;
    bloom::core::detail::Sha256Implementation kind;
};

[[nodiscard]] SelectedRounds selectRounds() noexcept {
#if defined(BLOOM_SHA256_X86)
    if (x86ShaExtensionsAvailable()) {
        return {compressX86, bloom::core::detail::Sha256Implementation::X86ShaExtensions};
    }
#endif
#if defined(BLOOM_SHA256_ARM)
    return {compressArm, bloom::core::detail::Sha256Implementation::ArmSha2};
#else
    return {compressPortable, bloom::core::detail::Sha256Implementation::Portable};
#endif
}

[[nodiscard]] const SelectedRounds& selectedRounds() noexcept {
    static const SelectedRounds rounds = selectRounds();
    return rounds;
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
        compress(bufferedBytes_.data(), 1);
        bufferedByteCount_ = 0;
    }

    const auto blocks = remaining.size() / bufferedBytes_.size();
    if (blocks != 0) {
        compress(remaining.data(), blocks);
        remaining = remaining.subspan(blocks * bufferedBytes_.size());
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
        final.compress(final.bufferedBytes_.data(), 1);
        final.bufferedByteCount_ = 0;
    }
    std::fill(final.bufferedBytes_.begin() + final.bufferedByteCount_,
              final.bufferedBytes_.begin() + 56, std::byte{0});
    storeBigEndian64(final.totalBytes_ * 8U, final.bufferedBytes_.data() + 56);
    final.compress(final.bufferedBytes_.data(), 1);

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

void Sha256Hasher::compress(const std::byte* data, const std::size_t blocks) noexcept {
    selectedRounds().compress(state_, data, blocks);
}

namespace detail {

Sha256Implementation activeSha256Implementation() noexcept { return selectedRounds().kind; }

std::optional<Sha256Digest> portableSha256(const std::span<const std::byte> bytes) noexcept {
    constexpr auto maximumMessageBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
    if (bytes.size() > maximumMessageBytes) {
        return std::nullopt;
    }
    State state = kInitialState;
    const auto fullBlocks = bytes.size() / 64U;
    compressPortable(state, bytes.data(), fullBlocks);
    std::array<std::byte, 128> tail{};
    const auto remaining = bytes.size() - fullBlocks * 64U;
    std::copy_n(bytes.data() + fullBlocks * 64U, remaining, tail.begin());
    tail[remaining] = std::byte{0x80};
    const std::size_t tailBlocks = remaining + 1 > 56 ? 2 : 1;
    storeBigEndian64(static_cast<std::uint64_t>(bytes.size()) * 8U,
                     tail.data() + tailBlocks * 64U - 8U);
    compressPortable(state, tail.data(), tailBlocks);
    Sha256Digest::Bytes digestBytes{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        storeBigEndian32(state[index], digestBytes.data() + index * 4);
    }
    return Sha256Digest::fromBytes(digestBytes);
}

} // namespace detail

} // namespace bloom::core
