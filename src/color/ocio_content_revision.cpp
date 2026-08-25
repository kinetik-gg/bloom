#include <bloom/color/ocio_content_revision.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

constexpr std::string_view kRevisionDomain{"BloomOcioRevision\0", 18};
constexpr std::uint64_t kSerializedHeaderByteCount = 29;
constexpr std::uint64_t kMaximumSha256MessageBytes = std::numeric_limits<std::uint64_t>::max() / 8U;

static_assert(kRevisionDomain.size() == 18 && kRevisionDomain.back() == '\0');

[[nodiscard]] constexpr bool
isSupported(const bloom::color::OcioContentLocatorKind locatorKind) noexcept {
    using enum bloom::color::OcioContentLocatorKind;
    return locatorKind == BuiltIn || locatorKind == ProjectRelativeArchive ||
           locatorKind == ExternalArchive;
}

template <typename Integer>
[[nodiscard]] constexpr std::uint64_t toUint64(const Integer value) noexcept {
    if constexpr (std::is_same_v<Integer, std::uint64_t>) {
        return value;
    } else {
        return static_cast<std::uint64_t>(value);
    }
}

void storeBigEndian16(const std::uint16_t value, std::byte* const destination) noexcept {
    destination[0] = static_cast<std::byte>(value >> 8U);
    destination[1] = static_cast<std::byte>(value);
}

void storeBigEndian64(const std::uint64_t value, std::byte* const destination) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        destination[index] = static_cast<std::byte>(value >> shift);
    }
}

} // namespace

namespace bloom::color {

OcioContentRevisionResult
computeOcioContentRevisionV1(const OcioContentLocatorKind locatorKind,
                             const std::span<const std::byte> payload) noexcept {
    if (!isSupported(locatorKind)) {
        return OcioContentRevisionResult(OcioContentRevisionError::InvalidLocatorKind);
    }
    if (!std::in_range<std::uint64_t>(payload.size())) {
        return OcioContentRevisionResult(OcioContentRevisionError::PayloadByteCountUnrepresentable);
    }

    const auto payloadByteCount = toUint64(payload.size());
    if (payloadByteCount > kMaximumSha256MessageBytes - kSerializedHeaderByteCount) {
        return OcioContentRevisionResult(OcioContentRevisionError::MessageByteCountExceedsSha256);
    }

    std::array<std::byte, 11> framing{};
    storeBigEndian16(kOcioContentRevisionVersion, framing.data());
    framing[2] = static_cast<std::byte>(locatorKind);
    storeBigEndian64(payloadByteCount, framing.data() + 3);

    core::Sha256Hasher hasher;
    const auto domain = std::as_bytes(std::span(kRevisionDomain.data(), kRevisionDomain.size()));
    if (!hasher.update(domain) || !hasher.update(framing) || !hasher.update(payload)) {
        return OcioContentRevisionResult(OcioContentRevisionError::MessageByteCountExceedsSha256);
    }
    return OcioContentRevisionResult(hasher.finalize());
}

} // namespace bloom::color
