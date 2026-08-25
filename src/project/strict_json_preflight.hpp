#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bloom::project::detail {

inline constexpr std::size_t kStrictJsonManifestMaximumInputBytes = 1U << 20U;
inline constexpr std::size_t kStrictJsonDocumentMaximumInputBytes = 256U << 20U;
inline constexpr std::uint32_t kStrictJsonMaximumDepth = 128;
inline constexpr std::uint64_t kStrictJsonMaximumValues = 4'000'000;
inline constexpr std::uint64_t kStrictJsonMaximumContainerEntries = 1'000'000;
inline constexpr std::uint64_t kStrictJsonMaximumDecodedStringBytes = 89'478'488;
inline constexpr std::size_t kStrictJsonCheckpointCadenceBytes = 64U << 10U;

struct StrictJsonPreflightLimits final {
    std::size_t maximumInputBytes = kStrictJsonDocumentMaximumInputBytes;
    std::uint64_t maximumValues = kStrictJsonMaximumValues;
    std::uint64_t maximumContainerEntries = kStrictJsonMaximumContainerEntries;
    std::uint32_t maximumDepth = kStrictJsonMaximumDepth;
    std::uint64_t maximumDecodedStringBytes = kStrictJsonMaximumDecodedStringBytes;
};

// The caller supplies the value budget remaining after earlier project-container entries. These
// factories do not clamp an invalid budget: preflight reports InvalidLimits instead.
[[nodiscard]] constexpr StrictJsonPreflightLimits
strictJsonManifestPreflightLimits(const std::uint64_t remainingValueBudget) noexcept {
    return {
        .maximumInputBytes = kStrictJsonManifestMaximumInputBytes,
        .maximumValues = remainingValueBudget,
        .maximumContainerEntries = kStrictJsonMaximumContainerEntries,
        .maximumDepth = kStrictJsonMaximumDepth,
        .maximumDecodedStringBytes = kStrictJsonManifestMaximumInputBytes,
    };
}

[[nodiscard]] constexpr StrictJsonPreflightLimits
strictJsonDocumentPreflightLimits(const std::uint64_t remainingValueBudget) noexcept {
    return {
        .maximumInputBytes = kStrictJsonDocumentMaximumInputBytes,
        .maximumValues = remainingValueBudget,
        .maximumContainerEntries = kStrictJsonMaximumContainerEntries,
        .maximumDepth = kStrictJsonMaximumDepth,
        .maximumDecodedStringBytes = kStrictJsonMaximumDecodedStringBytes,
    };
}

using StrictJsonCheckpointFunction = bool (*)(void* context, std::size_t consumedBytes,
                                              std::size_t totalBytes) noexcept;

struct StrictJsonCheckpoint final {
    void* context = nullptr;
    StrictJsonCheckpointFunction function = nullptr;
};

enum class StrictJsonPreflightError : std::uint8_t {
    None,
    InvalidLimits,
    InputTooLarge,
    BomForbidden,
    EmptyInput,
    InvalidUtf8,
    InvalidSyntax,
    InvalidEscape,
    InvalidUnicodeScalar,
    InvalidNumber,
    TrailingData,
    DepthLimitExceeded,
    ValueLimitExceeded,
    ContainerEntryLimitExceeded,
    DecodedStringLimitExceeded,
    SizeOverflow,
    Cancelled,
};

struct StrictJsonPreflightResult final {
    StrictJsonPreflightError error = StrictJsonPreflightError::InvalidSyntax;
    std::size_t errorOffset = 0;
    std::uint64_t valueCount = 0;
    std::uint32_t maximumObservedDepth = 0;

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error == StrictJsonPreflightError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }

    friend constexpr bool operator==(const StrictJsonPreflightResult&,
                                     const StrictJsonPreflightResult&) noexcept = default;
};

// Performs only strict RFC 8259 syntax, Unicode, and closed resource preflight. Success is not a
// trusted project parse: decoded duplicate keys, schemas, typed numeric conversion,
// cross-references, and unknown-member preservation remain the later bounded DOM/semantic decoder's
// responsibility. The scanner allocates nothing, retains no input view, and invokes checkpoints
// synchronously.
[[nodiscard]] StrictJsonPreflightResult
preflightStrictJson(std::span<const std::byte> input, StrictJsonPreflightLimits limits,
                    StrictJsonCheckpoint checkpoint = {}) noexcept;

static_assert(std::is_trivially_copyable_v<StrictJsonPreflightLimits>);
static_assert(std::is_trivially_copyable_v<StrictJsonCheckpoint>);
static_assert(std::is_trivially_copyable_v<StrictJsonPreflightResult>);

} // namespace bloom::project::detail
