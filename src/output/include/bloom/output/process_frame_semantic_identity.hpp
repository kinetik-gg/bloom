#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bloom::output {

inline constexpr std::uint16_t kProcessPixelStreamSerializationVersion = 1;
inline constexpr std::uint16_t kProcessFrameSemanticIdentitySerializationVersion = 1;
inline constexpr std::size_t kCompositionProcessFrameSemanticIdentityV1Bytes = 249;
inline constexpr std::size_t kProxyProcessFrameSemanticIdentityV1Bytes = 257;

// Version 1 has no caller-provided text. Its color and process pixel-semantics identifiers are
// fixed exact ASCII constants, so this boundary deliberately performs no Unicode normalization.
enum class ProcessFrameSemanticIdentityErrorCode : std::uint8_t {
    None,
    MissingPlan,
    InvalidStableId,
    InvalidTime,
    InvalidOutput,
    InvalidResolution,
    UnsupportedEvaluationQuality,
    UnsupportedColorIntent,
    InvalidSemanticsVersion,
    InvalidImage,
    InconsistentImage,
    InvalidPixel,
    HashInputTooLarge,
    InsufficientCapacity,
    InternalInvariant,
};

class [[nodiscard]] ProcessPixelDigestV1Result final {
  public:
    [[nodiscard]] static constexpr ProcessPixelDigestV1Result
    success(const core::Sha256Digest digest) noexcept {
        return ProcessPixelDigestV1Result(ProcessFrameSemanticIdentityErrorCode::None, digest);
    }
    [[nodiscard]] static constexpr ProcessPixelDigestV1Result
    failure(const ProcessFrameSemanticIdentityErrorCode code) noexcept {
        return ProcessPixelDigestV1Result(
            code == ProcessFrameSemanticIdentityErrorCode::None
                ? ProcessFrameSemanticIdentityErrorCode::InternalInvariant
                : code,
            {});
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == ProcessFrameSemanticIdentityErrorCode::None;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr ProcessFrameSemanticIdentityErrorCode error() const noexcept {
        return error_;
    }
    [[nodiscard]] constexpr core::Sha256Digest digest() const noexcept { return digest_; }

  private:
    constexpr ProcessPixelDigestV1Result(const ProcessFrameSemanticIdentityErrorCode error,
                                         const core::Sha256Digest digest) noexcept
        : digest_(digest), error_(error) {}

    core::Sha256Digest digest_;
    ProcessFrameSemanticIdentityErrorCode error_;
};

class [[nodiscard]] ProcessFrameSemanticIdentityV1Validation final {
  public:
    [[nodiscard]] static constexpr ProcessFrameSemanticIdentityV1Validation
    success(const std::size_t requiredBytes, const core::Sha256Digest digest) noexcept {
        return ProcessFrameSemanticIdentityV1Validation(ProcessFrameSemanticIdentityErrorCode::None,
                                                        requiredBytes, digest);
    }
    [[nodiscard]] static constexpr ProcessFrameSemanticIdentityV1Validation
    failure(const ProcessFrameSemanticIdentityErrorCode code) noexcept {
        return ProcessFrameSemanticIdentityV1Validation(
            code == ProcessFrameSemanticIdentityErrorCode::None
                ? ProcessFrameSemanticIdentityErrorCode::InternalInvariant
                : code,
            0, {});
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == ProcessFrameSemanticIdentityErrorCode::None;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr ProcessFrameSemanticIdentityErrorCode error() const noexcept {
        return error_;
    }
    [[nodiscard]] constexpr std::size_t requiredBytes() const noexcept { return requiredBytes_; }
    [[nodiscard]] constexpr core::Sha256Digest processPixelDigest() const noexcept {
        return processPixelDigest_;
    }

  private:
    constexpr ProcessFrameSemanticIdentityV1Validation(
        const ProcessFrameSemanticIdentityErrorCode error, const std::size_t requiredBytes,
        const core::Sha256Digest digest) noexcept
        : requiredBytes_(requiredBytes), processPixelDigest_(digest), error_(error) {}

    std::size_t requiredBytes_;
    core::Sha256Digest processPixelDigest_;
    ProcessFrameSemanticIdentityErrorCode error_;
};

class [[nodiscard]] ProcessFrameSemanticIdentityV1WriteResult final {
  public:
    [[nodiscard]] static constexpr ProcessFrameSemanticIdentityV1WriteResult
    success(const std::size_t bytesWritten, const core::Sha256Digest digest) noexcept {
        return ProcessFrameSemanticIdentityV1WriteResult(
            ProcessFrameSemanticIdentityErrorCode::None, bytesWritten, bytesWritten, 0, digest);
    }
    [[nodiscard]] static constexpr ProcessFrameSemanticIdentityV1WriteResult
    failure(const ProcessFrameSemanticIdentityErrorCode code, const std::size_t requiredBytes = 0,
            const std::size_t availableBytes = 0, const core::Sha256Digest digest = {}) noexcept {
        return ProcessFrameSemanticIdentityV1WriteResult(
            code == ProcessFrameSemanticIdentityErrorCode::None
                ? ProcessFrameSemanticIdentityErrorCode::InternalInvariant
                : code,
            requiredBytes, 0, availableBytes, digest);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == ProcessFrameSemanticIdentityErrorCode::None;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr ProcessFrameSemanticIdentityErrorCode error() const noexcept {
        return error_;
    }
    [[nodiscard]] constexpr std::size_t requiredBytes() const noexcept { return requiredBytes_; }
    [[nodiscard]] constexpr std::size_t writtenBytes() const noexcept { return writtenBytes_; }
    [[nodiscard]] constexpr std::size_t availableBytes() const noexcept { return availableBytes_; }
    [[nodiscard]] constexpr core::Sha256Digest processPixelDigest() const noexcept {
        return processPixelDigest_;
    }

  private:
    constexpr ProcessFrameSemanticIdentityV1WriteResult(
        const ProcessFrameSemanticIdentityErrorCode error, const std::size_t requiredBytes,
        const std::size_t writtenBytes, const std::size_t availableBytes,
        const core::Sha256Digest digest) noexcept
        : requiredBytes_(requiredBytes), writtenBytes_(writtenBytes),
          availableBytes_(availableBytes), processPixelDigest_(digest), error_(error) {}

    std::size_t requiredBytes_;
    std::size_t writtenBytes_;
    std::size_t availableBytes_;
    core::Sha256Digest processPixelDigest_;
    ProcessFrameSemanticIdentityErrorCode error_;
};

// Hashes the exact packed process rows without allocating or copying the image. The digest domain,
// count, components, and component bit patterns are those of Process Pixel Stream Version 1.
[[nodiscard]] ProcessPixelDigestV1Result
hashProcessPixelStreamV1(const render::Rgba32fImage& image) noexcept;

// Validation computes the pixel digest and exact encoded size without touching caller storage.
[[nodiscard]] ProcessFrameSemanticIdentityV1Validation
validateProcessFrameSemanticIdentityV1(const runtime::ProcessFrameIdentity& identity,
                                       const render::Rgba32fImage& image) noexcept;
[[nodiscard]] ProcessFrameSemanticIdentityV1Validation
validateProcessFrameSemanticIdentityV1(const runtime::ProcessFrame& frame) noexcept;

// Writing is transactional: every identity, plan, image, conversion, digest, and capacity check
// completes before any byte in destination is changed. The canonical record preserves call-defined
// semantic field order; it contains no execution provider, provenance, pointer, or budget data.
[[nodiscard]] ProcessFrameSemanticIdentityV1WriteResult
writeProcessFrameSemanticIdentityV1(const runtime::ProcessFrameIdentity& identity,
                                    const render::Rgba32fImage& image,
                                    std::span<std::byte> destination) noexcept;
[[nodiscard]] ProcessFrameSemanticIdentityV1WriteResult
writeProcessFrameSemanticIdentityV1(const runtime::ProcessFrame& frame,
                                    std::span<std::byte> destination) noexcept;

static_assert(std::is_trivially_copyable_v<ProcessPixelDigestV1Result>);
static_assert(std::is_trivially_copyable_v<ProcessFrameSemanticIdentityV1Validation>);
static_assert(std::is_trivially_copyable_v<ProcessFrameSemanticIdentityV1WriteResult>);

} // namespace bloom::output
