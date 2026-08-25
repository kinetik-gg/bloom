#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace bloom::output {

inline constexpr std::uint16_t kProcessPixelStreamSerializationVersion = 1;
inline constexpr std::uint16_t kProcessFrameSemanticIdentitySerializationVersion = 1;
inline constexpr std::size_t kCompositionProcessFrameSemanticIdentityV1Bytes = 249;
inline constexpr std::size_t kProxyProcessFrameSemanticIdentityV1Bytes = 257;

// Version 1 has no caller-provided text. Its color and process pixel-semantics identifiers are
// fixed exact ASCII constants, so this boundary deliberately performs no Unicode normalization.
enum class ProcessFrameSemanticIdentityErrorCode : std::uint8_t {
    None,
    MissingFrame,
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
    ResourceLimitExceeded,
    HashInputTooLarge,
    AllocationFailure,
    InternalInvariant,
};

enum class ProcessFrameSemanticIdentityPreparationStatus : std::uint8_t {
    Prepared,
    Cancelled,
    Failed,
};

enum class ProcessFrameSemanticIdentityProgressStage : std::uint8_t {
    Preflight,
    HashingPixels,
    Encoding,
};

struct ProcessFrameSemanticIdentityProgress final {
    ProcessFrameSemanticIdentityProgressStage stage =
        ProcessFrameSemanticIdentityProgressStage::Preflight;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;

    friend bool operator==(const ProcessFrameSemanticIdentityProgress&,
                           const ProcessFrameSemanticIdentityProgress&) = default;
};

using ProcessFrameSemanticIdentityProgressCallback =
    std::function<void(const ProcessFrameSemanticIdentityProgress&)>;

class ProcessFrameSemanticIdentityV1Preparer;

// One inseparable lifetime binds the exact immutable frame, its canonical identity bytes, and the
// process-pixel digest. Construction is private and publication occurs only after preparation has
// completed successfully.
class ProcessFrameSemanticIdentityV1 final {
  public:
    ProcessFrameSemanticIdentityV1(const ProcessFrameSemanticIdentityV1&) = delete;
    ProcessFrameSemanticIdentityV1& operator=(const ProcessFrameSemanticIdentityV1&) = delete;
    ProcessFrameSemanticIdentityV1(ProcessFrameSemanticIdentityV1&&) = delete;
    ProcessFrameSemanticIdentityV1& operator=(ProcessFrameSemanticIdentityV1&&) = delete;
    ~ProcessFrameSemanticIdentityV1() = default;

    [[nodiscard]] const std::shared_ptr<const runtime::ProcessFrame>&
    processFrame() const& noexcept {
        return processFrame_;
    }
    [[nodiscard]] const std::shared_ptr<const runtime::ProcessFrame>&
    processFrame() const&& = delete;
    [[nodiscard]] std::span<const std::byte> canonicalBytes() const& noexcept {
        return std::span(canonicalBytes_).first(canonicalByteCount_);
    }
    [[nodiscard]] std::span<const std::byte> canonicalBytes() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& processPixelDigest() const& noexcept {
        return processPixelDigest_;
    }
    [[nodiscard]] const core::Sha256Digest& processPixelDigest() const&& = delete;

  private:
    ProcessFrameSemanticIdentityV1(
        std::shared_ptr<const runtime::ProcessFrame> processFrame,
        std::array<std::byte, kProxyProcessFrameSemanticIdentityV1Bytes> canonicalBytes,
        std::size_t canonicalByteCount, core::Sha256Digest processPixelDigest) noexcept;

    std::shared_ptr<const runtime::ProcessFrame> processFrame_;
    std::array<std::byte, kProxyProcessFrameSemanticIdentityV1Bytes> canonicalBytes_{};
    std::size_t canonicalByteCount_ = 0;
    core::Sha256Digest processPixelDigest_{};

    friend class ProcessFrameSemanticIdentityV1Preparer;
};

class [[nodiscard]] ProcessFrameSemanticIdentityV1PreparationResult final {
  public:
    // Publication results are cheap shared-ownership values. Explicit copy operations suppress
    // consuming moves, so an rvalue copy cannot leave a Prepared source with a null product.
    ProcessFrameSemanticIdentityV1PreparationResult(
        const ProcessFrameSemanticIdentityV1PreparationResult&) noexcept = default;
    ProcessFrameSemanticIdentityV1PreparationResult&
    operator=(const ProcessFrameSemanticIdentityV1PreparationResult&) noexcept = default;

    [[nodiscard]] ProcessFrameSemanticIdentityPreparationStatus status() const noexcept {
        return status_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    identity() const& noexcept {
        return identity_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    identity() const&& = delete;
    [[nodiscard]] ProcessFrameSemanticIdentityErrorCode error() const noexcept { return error_; }

  private:
    static ProcessFrameSemanticIdentityV1PreparationResult
    prepared(std::shared_ptr<const ProcessFrameSemanticIdentityV1> identity) noexcept;
    static ProcessFrameSemanticIdentityV1PreparationResult cancelled() noexcept;
    static ProcessFrameSemanticIdentityV1PreparationResult
    failed(ProcessFrameSemanticIdentityErrorCode error) noexcept;

    ProcessFrameSemanticIdentityV1PreparationResult(
        ProcessFrameSemanticIdentityPreparationStatus status,
        std::shared_ptr<const ProcessFrameSemanticIdentityV1> identity,
        ProcessFrameSemanticIdentityErrorCode error) noexcept;

    ProcessFrameSemanticIdentityPreparationStatus status_ =
        ProcessFrameSemanticIdentityPreparationStatus::Failed;
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> identity_;
    ProcessFrameSemanticIdentityErrorCode error_ =
        ProcessFrameSemanticIdentityErrorCode::InternalInvariant;

    friend class ProcessFrameSemanticIdentityV1Preparer;
};

// A worker-style, bounded producer. It performs complete cheap preflight before reading pixels,
// hashes pixels in fixed stack chunks with cancellation checks, encodes into fixed storage, and
// publishes the immutable product only as its final action.
class ProcessFrameSemanticIdentityV1Preparer final {
  public:
    [[nodiscard]] ProcessFrameSemanticIdentityV1PreparationResult
    prepare(std::shared_ptr<const runtime::ProcessFrame> processFrame,
            const runtime::CancellationToken& cancellation,
            const ProcessFrameSemanticIdentityProgressCallback& progress = {}) const noexcept;
};

} // namespace bloom::output
