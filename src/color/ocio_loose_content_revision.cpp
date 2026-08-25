#include <bloom/color/ocio_loose_content_revision.hpp>

#include <bloom/core/utf8.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

constexpr std::string_view kRevisionDomain{"BloomOcioLooseRevision\0", 23};
constexpr std::string_view kConfigKey = "config.ocio";
constexpr std::uint64_t kSerializedHeaderByteCount = 29;
constexpr std::uint64_t kMaximumSha256MessageBytes = std::numeric_limits<std::uint64_t>::max() / 8U;

static_assert(kRevisionDomain.size() == 23 && kRevisionDomain.back() == '\0');

template <typename Integer>
[[nodiscard]] constexpr std::uint64_t toUint64(const Integer value) noexcept {
    if constexpr (std::is_same_v<Integer, std::uint64_t>) {
        return value;
    } else {
        return static_cast<std::uint64_t>(value);
    }
}

[[nodiscard]] constexpr bool isAsciiLetter(const char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

[[nodiscard]] bool hasCanonicalStructure(const std::string_view key) noexcept {
    if (key.empty() || key.front() == '/' || key.back() == '/' ||
        (key.size() >= 2 && isAsciiLetter(key[0]) && key[1] == ':')) {
        return false;
    }

    std::size_t componentStart = 0;
    for (std::size_t index = 0; index <= key.size(); ++index) {
        if (index < key.size() && key[index] != '/') {
            if (key[index] == '\0' || key[index] == '\\') {
                return false;
            }
            continue;
        }

        const auto component = key.substr(componentStart, index - componentStart);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        componentStart = index + 1;
    }
    return true;
}

[[nodiscard]] constexpr bool addChecked(std::uint64_t& value,
                                        const std::uint64_t increment) noexcept {
    if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
    }
    value += increment;
    return true;
}

void storeBigEndian16(const std::uint16_t value, std::byte* const destination) noexcept {
    destination[0] = static_cast<std::byte>(value >> 8U);
    destination[1] = static_cast<std::byte>(value);
}

void storeBigEndian32(const std::uint32_t value, std::byte* const destination) noexcept {
    destination[0] = static_cast<std::byte>(value >> 24U);
    destination[1] = static_cast<std::byte>(value >> 16U);
    destination[2] = static_cast<std::byte>(value >> 8U);
    destination[3] = static_cast<std::byte>(value);
}

void storeBigEndian64(const std::uint64_t value, std::byte* const destination) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        destination[index] = static_cast<std::byte>(value >> shift);
    }
}

} // namespace

namespace bloom::color {

OcioLooseContentRevisionResult
computeOcioLooseContentRevisionV1(const std::span<const OcioLooseResourceView> entries) noexcept {
    if (!std::in_range<std::uint32_t>(entries.size())) {
        return OcioLooseContentRevisionResult(
            OcioLooseContentRevisionError::EntryCountUnrepresentable);
    }
    if (entries.size() > kOcioLooseMaximumEntryCount) {
        return OcioLooseContentRevisionResult(
            OcioLooseContentRevisionError::EntryCountLimitExceeded);
    }

    std::uint64_t aggregatePayloadBytes = 0;
    std::uint64_t serializedBytes = kSerializedHeaderByteCount;
    bool hasConfig = false;
    std::string_view previousKey;
    bool hasPreviousKey = false;

    for (const auto& entry : entries) {
        if (!std::in_range<std::uint32_t>(entry.key.size())) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::KeyByteCountUnrepresentable);
        }
        if (entry.key.size() > kOcioLooseMaximumKeyBytes) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::KeyByteCountLimitExceeded);
        }
        if (!core::isValidUtf8(entry.key)) {
            return OcioLooseContentRevisionResult(OcioLooseContentRevisionError::InvalidKeyUtf8);
        }
        if (!hasCanonicalStructure(entry.key)) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::InvalidKeyStructure);
        }

        if (hasPreviousKey) {
            const auto order = core::compareUtf8Bytes(previousKey, entry.key);
            if (order == std::strong_ordering::equal) {
                return OcioLooseContentRevisionResult(OcioLooseContentRevisionError::DuplicateKey);
            }
            if (order == std::strong_ordering::greater) {
                return OcioLooseContentRevisionResult(
                    OcioLooseContentRevisionError::KeysNotStrictlyOrdered);
            }
        }
        previousKey = entry.key;
        hasPreviousKey = true;

        if (!std::in_range<std::uint64_t>(entry.payload.size())) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::PayloadByteCountUnrepresentable);
        }
        const auto payloadBytes = toUint64(entry.payload.size());
        if (entry.key == kConfigKey && payloadBytes > kOcioLooseMaximumConfigBytes) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::ConfigByteCountLimitExceeded);
        }
        if (payloadBytes > kOcioLooseMaximumResourceBytes) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::ResourceByteCountLimitExceeded);
        }
        if (!addChecked(aggregatePayloadBytes, payloadBytes)) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::AggregateByteCountOverflow);
        }
        if (aggregatePayloadBytes > kOcioLooseMaximumAggregateBytes) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::AggregateByteCountLimitExceeded);
        }

        const auto keyBytes = toUint64(entry.key.size());
        if (!addChecked(serializedBytes, 4) || !addChecked(serializedBytes, keyBytes) ||
            !addChecked(serializedBytes, 8) || !addChecked(serializedBytes, payloadBytes)) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::MessageByteCountOverflow);
        }
        if (serializedBytes > kMaximumSha256MessageBytes) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::MessageByteCountExceedsSha256);
        }
        hasConfig = hasConfig || entry.key == kConfigKey;
    }

    if (!hasConfig) {
        return OcioLooseContentRevisionResult(OcioLooseContentRevisionError::MissingConfig);
    }

    std::array<std::byte, 6> header{};
    storeBigEndian16(kOcioLooseContentRevisionVersion, header.data());
    storeBigEndian32(static_cast<std::uint32_t>(entries.size()), header.data() + 2);

    core::Sha256Hasher hasher;
    const auto domain = std::as_bytes(std::span(kRevisionDomain.data(), kRevisionDomain.size()));
    if (!hasher.update(domain) || !hasher.update(header)) {
        return OcioLooseContentRevisionResult(
            OcioLooseContentRevisionError::MessageByteCountExceedsSha256);
    }

    for (const auto& entry : entries) {
        std::array<std::byte, 4> keyLength{};
        storeBigEndian32(static_cast<std::uint32_t>(entry.key.size()), keyLength.data());
        std::array<std::byte, 8> payloadLength{};
        storeBigEndian64(toUint64(entry.payload.size()), payloadLength.data());
        const auto keyBytes = std::as_bytes(std::span(entry.key.data(), entry.key.size()));
        if (!hasher.update(keyLength) || !hasher.update(keyBytes) ||
            !hasher.update(payloadLength) || !hasher.update(entry.payload)) {
            return OcioLooseContentRevisionResult(
                OcioLooseContentRevisionError::MessageByteCountExceedsSha256);
        }
    }
    return OcioLooseContentRevisionResult(hasher.finalize());
}

} // namespace bloom::color
