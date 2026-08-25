#pragma once

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::color {

inline constexpr std::uint16_t kOcioLooseContentRevisionVersion = 1;
inline constexpr std::size_t kOcioLooseMaximumEntryCount = 2048;
inline constexpr std::size_t kOcioLooseMaximumKeyBytes = 4096;
inline constexpr std::uint64_t kOcioLooseMaximumConfigBytes = 8'388'608;
inline constexpr std::uint64_t kOcioLooseMaximumResourceBytes = 67'108'864;
inline constexpr std::uint64_t kOcioLooseMaximumAggregateBytes = 268'435'456;

struct OcioLooseResourceView final {
    std::string_view key;
    std::span<const std::byte> payload;
};

enum class OcioLooseContentRevisionError : std::uint8_t {
    None,
    EntryCountUnrepresentable,
    EntryCountLimitExceeded,
    KeyByteCountUnrepresentable,
    KeyByteCountLimitExceeded,
    InvalidKeyUtf8,
    InvalidKeyStructure,
    DuplicateKey,
    KeysNotStrictlyOrdered,
    MissingConfig,
    PayloadByteCountUnrepresentable,
    ConfigByteCountLimitExceeded,
    ResourceByteCountLimitExceeded,
    AggregateByteCountOverflow,
    AggregateByteCountLimitExceeded,
    MessageByteCountOverflow,
    MessageByteCountExceedsSha256,
};

class OcioLooseContentRevisionResult;

// Computes the closed version 1 revision over an immutable table already resolved by Bloom's
// loose-config resolver. This function validates strict UTF-8, structural root-relative key
// spelling, ordering, uniqueness, and byte ceilings. It performs no allocation.
//
// This is deliberately not a loose-config resolver or qualification boundary: it does not inspect
// files, normalize Unicode, detect NFC/default-case-fold collisions, or establish filesystem
// safety. The resolver must prove those properties before publishing the immutable table.
[[nodiscard]] OcioLooseContentRevisionResult
computeOcioLooseContentRevisionV1(std::span<const OcioLooseResourceView> entries) noexcept;

class [[nodiscard]] OcioLooseContentRevisionResult final {
  public:
    [[nodiscard]] constexpr bool succeeded() const noexcept { return revision_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }

    [[nodiscard]] constexpr const core::Sha256Digest* revision() const& noexcept {
        return revision_.has_value() ? &*revision_ : nullptr;
    }
    [[nodiscard]] constexpr const core::Sha256Digest* revision() const&& = delete;

    [[nodiscard]] constexpr OcioLooseContentRevisionError error() const noexcept { return error_; }

  private:
    friend OcioLooseContentRevisionResult
        computeOcioLooseContentRevisionV1(std::span<const OcioLooseResourceView>) noexcept;

    explicit constexpr OcioLooseContentRevisionResult(const core::Sha256Digest revision) noexcept
        : revision_(revision) {}
    explicit constexpr OcioLooseContentRevisionResult(
        const OcioLooseContentRevisionError error) noexcept
        : error_(error) {}

    std::optional<core::Sha256Digest> revision_;
    OcioLooseContentRevisionError error_ = OcioLooseContentRevisionError::None;
};

static_assert(std::is_trivially_copyable_v<OcioLooseResourceView>);
static_assert(std::is_trivially_copyable_v<OcioLooseContentRevisionResult>);

} // namespace bloom::color
