#pragma once

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace bloom::color {

inline constexpr std::uint16_t kOcioContentRevisionVersion = 1;

enum class OcioContentLocatorKind : std::uint8_t {
    BuiltIn = 1,
    ProjectRelativeArchive = 2,
    ExternalArchive = 3,
};

enum class OcioContentRevisionError : std::uint8_t {
    None,
    InvalidLocatorKind,
    PayloadByteCountUnrepresentable,
    MessageByteCountExceedsSha256,
};

class OcioContentRevisionResult;

// Computes the version 1 content revision for one immutable built-in config.ocio payload or one
// complete .ocioz archive payload. It does not resolve locators, inspect archives, or qualify OCIO
// content. The caller retains ownership of the payload and no allocation is performed.
[[nodiscard]] OcioContentRevisionResult
computeOcioContentRevisionV1(OcioContentLocatorKind locatorKind,
                             std::span<const std::byte> payload) noexcept;

class [[nodiscard]] OcioContentRevisionResult final {
  public:
    [[nodiscard]] constexpr bool succeeded() const noexcept { return revision_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }

    [[nodiscard]] constexpr const core::Sha256Digest* revision() const& noexcept {
        return revision_.has_value() ? &*revision_ : nullptr;
    }
    [[nodiscard]] constexpr const core::Sha256Digest* revision() const&& = delete;

    [[nodiscard]] constexpr OcioContentRevisionError error() const noexcept { return error_; }

  private:
    friend OcioContentRevisionResult
        computeOcioContentRevisionV1(OcioContentLocatorKind, std::span<const std::byte>) noexcept;

    explicit constexpr OcioContentRevisionResult(const core::Sha256Digest revision) noexcept
        : revision_(revision) {}
    explicit constexpr OcioContentRevisionResult(const OcioContentRevisionError error) noexcept
        : error_(error) {}

    std::optional<core::Sha256Digest> revision_;
    OcioContentRevisionError error_ = OcioContentRevisionError::None;
};

static_assert(std::is_trivially_copyable_v<OcioContentRevisionResult>);

} // namespace bloom::color
