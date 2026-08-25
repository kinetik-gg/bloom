#pragma once

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::color {

inline constexpr std::uint16_t kDisplayProcessorIdentityVersion = 1;
inline constexpr std::size_t kDisplayProcessorIdentityMaximumBytes = 2'097'152;
inline constexpr std::size_t kDisplayProcessorIdentityMaximumContextVariables = 256;
inline constexpr std::size_t kDisplayProcessorIdentityMaximumContextNameBytes = 128;
inline constexpr std::size_t kDisplayProcessorIdentityMaximumTextBytes = 4096;
inline constexpr std::size_t kDisplayProcessorIdentityMaximumLooks = 128;

inline constexpr std::string_view kDisplayProcessorIdentitySourceColorSpaceId = "lin_rec709_scene";
inline constexpr std::string_view kDisplayProcessorIdentityOutputColorSpaceId =
    "srgb_rec709_display";
inline constexpr std::string_view kDisplayProcessorIdentityQualityId = "reference";
inline constexpr std::string_view kDisplayProcessorIdentitySemanticsProfileId =
    "bloom.color.ocio-cpu-display.v1";
inline constexpr std::string_view kDisplayProcessorIdentityPackingId = "straight-rgba8";

enum class DisplayProcessorLookModeV1 : std::uint8_t {
    Bypass = 0,
    Ordered = 1,
};

struct DisplayProcessorContextVariableV1View final {
    std::string_view name;
    std::string_view value;
};

struct DisplayProcessorIdentityV1InputView final {
    core::Sha256Digest expectedOcioRevision;
    std::span<const DisplayProcessorContextVariableV1View> contextVariables;
    std::string_view sourceColorSpaceId;
    std::string_view displayName;
    std::string_view viewName;
    DisplayProcessorLookModeV1 lookMode = DisplayProcessorLookModeV1::Bypass;
    std::span<const std::string_view> lookNames;
    std::string_view outputColorSpaceId;
    std::string_view qualityId;
    std::string_view semanticsProfileId;
    std::string_view packingId;
};

enum class DisplayProcessorIdentityError : std::uint8_t {
    None,
    ContextVariableCountLimitExceeded,
    ContextNameByteCountLimitExceeded,
    InvalidContextName,
    DuplicateContextName,
    ContextVariablesNotStrictlyOrdered,
    TextByteCountLimitExceeded,
    InvalidUtf8,
    EmbeddedNul,
    NormalizationUnavailable,
    EmptyDisplayName,
    EmptyViewName,
    InvalidLookMode,
    LookCountMismatch,
    LookCountLimitExceeded,
    EmptyLookName,
    InvalidSourceColorSpaceId,
    InvalidOutputColorSpaceId,
    InvalidQualityId,
    InvalidSemanticsProfileId,
    InvalidPackingId,
    IdentityByteCountOverflow,
    IdentityByteCountLimitExceeded,
    DestinationTooSmall,
    InputAliasesDestination,
    Truncated,
    InvalidDomain,
    UnsupportedVersion,
    TrailingBytes,
    InternalInvariant,
};

class DisplayProcessorIdentityV1Validation;
class DisplayProcessorIdentityV1WriteResult;
class DisplayProcessorIdentityV1ParseResult;

[[nodiscard]] DisplayProcessorIdentityV1Validation
validateDisplayProcessorIdentityV1(const DisplayProcessorIdentityV1InputView& input) noexcept;

// Writes only after complete validation and destination-capacity checks. Every failure leaves the
// entire caller-owned destination unchanged.
[[nodiscard]] DisplayProcessorIdentityV1WriteResult
writeDisplayProcessorIdentityV1(const DisplayProcessorIdentityV1InputView& input,
                                std::span<std::byte> destination) noexcept;

class DisplayProcessorIdentityV1View;

// Validates a complete canonical record and returns a typed borrowed view. The caller retains the
// byte storage and must keep it alive and unchanged while using the view.
[[nodiscard]] DisplayProcessorIdentityV1ParseResult
parseDisplayProcessorIdentityV1(std::span<const std::byte> canonicalBytes) noexcept;

class [[nodiscard]] DisplayProcessorIdentityV1Validation final {
  public:
    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error_ == DisplayProcessorIdentityError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr DisplayProcessorIdentityError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::size_t requiredByteCount() const noexcept {
        return requiredByteCount_;
    }

  private:
    friend DisplayProcessorIdentityV1Validation
    validateDisplayProcessorIdentityV1(const DisplayProcessorIdentityV1InputView&) noexcept;

    explicit constexpr DisplayProcessorIdentityV1Validation(
        const std::size_t requiredByteCount) noexcept
        : requiredByteCount_(requiredByteCount) {}
    explicit constexpr DisplayProcessorIdentityV1Validation(
        const DisplayProcessorIdentityError error) noexcept
        : error_(error == DisplayProcessorIdentityError::None
                     ? DisplayProcessorIdentityError::InternalInvariant
                     : error) {}

    std::size_t requiredByteCount_ = 0;
    DisplayProcessorIdentityError error_ = DisplayProcessorIdentityError::None;
};

class [[nodiscard]] DisplayProcessorIdentityV1WriteResult final {
  public:
    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error_ == DisplayProcessorIdentityError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr DisplayProcessorIdentityError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::size_t requiredByteCount() const noexcept {
        return requiredByteCount_;
    }
    [[nodiscard]] constexpr std::size_t writtenByteCount() const noexcept {
        return succeeded() ? requiredByteCount_ : 0;
    }

  private:
    friend DisplayProcessorIdentityV1WriteResult
    writeDisplayProcessorIdentityV1(const DisplayProcessorIdentityV1InputView&,
                                    std::span<std::byte>) noexcept;

    explicit constexpr DisplayProcessorIdentityV1WriteResult(
        const std::size_t requiredByteCount) noexcept
        : requiredByteCount_(requiredByteCount) {}
    constexpr DisplayProcessorIdentityV1WriteResult(
        const std::size_t requiredByteCount, const DisplayProcessorIdentityError error) noexcept
        : requiredByteCount_(requiredByteCount),
          error_(error == DisplayProcessorIdentityError::None
                     ? DisplayProcessorIdentityError::InternalInvariant
                     : error) {}

    std::size_t requiredByteCount_ = 0;
    DisplayProcessorIdentityError error_ = DisplayProcessorIdentityError::None;
};

class DisplayProcessorIdentityV1View final {
  public:
    DisplayProcessorIdentityV1View(const DisplayProcessorIdentityV1View&) noexcept = default;
    DisplayProcessorIdentityV1View&
    operator=(const DisplayProcessorIdentityV1View&) noexcept = default;

    [[nodiscard]] constexpr std::span<const std::byte> canonicalBytes() const& noexcept {
        return canonicalBytes_;
    }
    [[nodiscard]] constexpr std::span<const std::byte> canonicalBytes() const&& = delete;
    [[nodiscard]] constexpr core::Sha256Digest expectedOcioRevision() const noexcept {
        return expectedOcioRevision_;
    }

  private:
    friend DisplayProcessorIdentityV1ParseResult
        parseDisplayProcessorIdentityV1(std::span<const std::byte>) noexcept;

    constexpr DisplayProcessorIdentityV1View(const std::span<const std::byte> canonicalBytes,
                                             const core::Sha256Digest expectedOcioRevision) noexcept
        : canonicalBytes_(canonicalBytes), expectedOcioRevision_(expectedOcioRevision) {}

    std::span<const std::byte> canonicalBytes_;
    core::Sha256Digest expectedOcioRevision_;
};

class [[nodiscard]] DisplayProcessorIdentityV1ParseResult final {
  public:
    [[nodiscard]] constexpr bool succeeded() const noexcept { return identity_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr const DisplayProcessorIdentityV1View* identity() const& noexcept {
        return identity_.has_value() ? &*identity_ : nullptr;
    }
    [[nodiscard]] constexpr const DisplayProcessorIdentityV1View* identity() const&& = delete;
    [[nodiscard]] constexpr DisplayProcessorIdentityError error() const noexcept { return error_; }
    // Parser failures identify the first byte belonging to the rejected field. End-of-input is
    // reported for a missing suffix. Successful parsing reports zero.
    [[nodiscard]] constexpr std::size_t errorOffset() const noexcept { return errorOffset_; }

  private:
    friend DisplayProcessorIdentityV1ParseResult
        parseDisplayProcessorIdentityV1(std::span<const std::byte>) noexcept;

    explicit constexpr DisplayProcessorIdentityV1ParseResult(
        const DisplayProcessorIdentityV1View identity) noexcept
        : identity_(identity) {}
    constexpr DisplayProcessorIdentityV1ParseResult(const DisplayProcessorIdentityError error,
                                                    const std::size_t errorOffset) noexcept
        : errorOffset_(errorOffset), error_(error == DisplayProcessorIdentityError::None
                                                ? DisplayProcessorIdentityError::InternalInvariant
                                                : error) {}

    std::optional<DisplayProcessorIdentityV1View> identity_;
    std::size_t errorOffset_ = 0;
    DisplayProcessorIdentityError error_ = DisplayProcessorIdentityError::None;
};

static_assert(std::is_trivially_copyable_v<DisplayProcessorContextVariableV1View>);
static_assert(std::is_trivially_copyable_v<DisplayProcessorIdentityV1InputView>);
static_assert(std::is_trivially_copyable_v<DisplayProcessorIdentityV1Validation>);
static_assert(std::is_trivially_copyable_v<DisplayProcessorIdentityV1WriteResult>);
static_assert(std::is_trivially_copyable_v<DisplayProcessorIdentityV1View>);
static_assert(std::is_trivially_copyable_v<DisplayProcessorIdentityV1ParseResult>);
static_assert(!std::is_default_constructible_v<DisplayProcessorIdentityV1View>);

} // namespace bloom::color
