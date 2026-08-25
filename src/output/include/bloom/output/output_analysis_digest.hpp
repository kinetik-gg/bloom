#pragma once

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace bloom::output {

inline constexpr std::uint16_t kOutputAnalysisDigestSerializationVersionV1 = 1;

// The owning display identity is referenced only for this synchronous call. PNG requires both
// fields; EXR requires both to be absent.
struct OutputAnalysisDigestDependenciesV1 final {
    std::optional<core::Sha256Digest> expectedOcioRevision;
    const color::DisplayProcessorIdentityV1* displayProcessorIdentity = nullptr;
};

enum class OutputAnalysisDigestErrorCodeV1 : std::uint8_t {
    None,
    InvalidReport,
    ReportNotApprovable,
    ProcessFrameDescriptorMismatch,
    ResourceLimitExceeded,
    MissingPngExpectedOcioRevision,
    MissingPngDisplayIdentity,
    UnexpectedExrExpectedOcioRevision,
    UnexpectedExrDisplayIdentity,
    InvalidDisplayIdentity,
    OcioRevisionMismatch,
    TargetDependencyRevisionMismatch,
    StableCodeInvariantViolation,
    PreimageSizeOverflow,
    PreimageTooLarge,
    InternalInvariant,
};

class [[nodiscard]] OutputAnalysisDigestV1Result final {
  public:
    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == OutputAnalysisDigestErrorCodeV1::None && digest_.has_value();
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr OutputAnalysisDigestErrorCodeV1 error() const noexcept {
        return error_;
    }
    [[nodiscard]] constexpr const core::Sha256Digest* digest() const& noexcept {
        if (!digest_.has_value()) {
            return nullptr;
        }
        return digest_.operator->();
    }
    [[nodiscard]] constexpr const core::Sha256Digest* digest() const&& = delete;
    [[nodiscard]] constexpr std::size_t preimageByteCount() const noexcept {
        return preimageByteCount_;
    }
    [[nodiscard]] constexpr OutputAnalysisReportIssueV1 reportIssue() const noexcept {
        return reportIssue_;
    }

  private:
    [[nodiscard]] static constexpr OutputAnalysisDigestV1Result
    success(const core::Sha256Digest digest, const std::size_t preimageByteCount) noexcept {
        return OutputAnalysisDigestV1Result(OutputAnalysisDigestErrorCodeV1::None,
                                            std::optional{digest}, preimageByteCount, {});
    }
    [[nodiscard]] static constexpr OutputAnalysisDigestV1Result
    failure(const OutputAnalysisDigestErrorCodeV1 error, const std::size_t preimageByteCount = 0,
            const OutputAnalysisReportIssueV1 reportIssue = {}) noexcept {
        return OutputAnalysisDigestV1Result(error == OutputAnalysisDigestErrorCodeV1::None
                                                ? OutputAnalysisDigestErrorCodeV1::InternalInvariant
                                                : error,
                                            std::nullopt, preimageByteCount, reportIssue);
    }

    constexpr OutputAnalysisDigestV1Result(const OutputAnalysisDigestErrorCodeV1 error,
                                           const std::optional<core::Sha256Digest> digest,
                                           const std::size_t preimageByteCount,
                                           const OutputAnalysisReportIssueV1 reportIssue) noexcept
        : digest_(digest), preimageByteCount_(preimageByteCount), reportIssue_(reportIssue),
          error_(error) {}

    std::optional<core::Sha256Digest> digest_;
    std::size_t preimageByteCount_ = 0;
    OutputAnalysisReportIssueV1 reportIssue_{};
    OutputAnalysisDigestErrorCodeV1 error_ = OutputAnalysisDigestErrorCodeV1::None;

    friend OutputAnalysisDigestV1Result
    computeOutputAnalysisDigestV1(const ProcessFrameSemanticIdentityV1&, OutputAnalysisReportV1View,
                                  const OutputAnalysisDigestDependenciesV1&) noexcept;
};

// Revalidates the report, binds its source descriptors to the frame retained by processIdentity,
// completes every cheap dependency and sizing check, then streams the already-prepared canonical
// identity bytes. No second frame or arbitrary process-identity bytes can be supplied.
[[nodiscard]] OutputAnalysisDigestV1Result
computeOutputAnalysisDigestV1(const ProcessFrameSemanticIdentityV1& processIdentity,
                              OutputAnalysisReportV1View report,
                              const OutputAnalysisDigestDependenciesV1& dependencies = {}) noexcept;

static_assert(std::is_trivially_copyable_v<OutputAnalysisDigestDependenciesV1>);
static_assert(std::is_trivially_copyable_v<OutputAnalysisDigestV1Result>);

} // namespace bloom::output
