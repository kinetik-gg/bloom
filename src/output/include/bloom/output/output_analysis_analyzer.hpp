#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/render/image.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace bloom::output {

inline constexpr std::size_t kOutputAnalysisReportDescriptorStorageMaximumBytesV1 =
    kOutputAnalysisFacetCountV1 * 2U * kOutputFacetDescriptorV1MaximumBytes;

enum class OutputAnalysisProcessSourceStateV1 : std::uint8_t {
    Ready,
    Missing,
};

// Exactly one arm is valid. Ready retains the prepared semantic identity and derives its descriptor
// from that product's exact retained frame. Missing owns the already-validated image descriptor.
struct OutputAnalysisProcessSourceV1 final {
    OutputAnalysisProcessSourceStateV1 state = OutputAnalysisProcessSourceStateV1::Ready;
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> readyIdentity;
    std::optional<render::Rgba32fImageDescriptor> missingDescriptor;
};

enum class OutputAnalysisAdapterStateV1 : std::uint8_t {
    Qualified,
    Unavailable,
};

enum class OutputAnalysisCompressionStateV1 : std::uint8_t {
    Available,
    Unavailable,
};

enum class OutputAnalysisOtherDependencyStateV1 : std::uint8_t {
    Available,
    Missing,
};

enum class PngRgba8SrgbColorResolutionStateV1 : std::uint8_t {
    Ready,
    Missing,
    Changed,
    Invalid,
    MissingResource,
    UnsupportedVersion,
};

struct PngRgba8SrgbAnalysisInputV1 final {
    OutputAnalysisProcessSourceV1 process;
    core::Sha256Digest expectedOcioRevision;
    PngRgba8SrgbColorResolutionStateV1 colorResolution = PngRgba8SrgbColorResolutionStateV1::Ready;
    OutputAnalysisAdapterStateV1 adapter = OutputAnalysisAdapterStateV1::Qualified;
    OutputAnalysisCompressionStateV1 compression = OutputAnalysisCompressionStateV1::Available;
    OutputAnalysisOtherDependencyStateV1 otherDependency =
        OutputAnalysisOtherDependencyStateV1::Available;
};

struct FlatExrRgba32fLinRec709SceneAnalysisInputV1 final {
    OutputAnalysisProcessSourceV1 process;
    OutputAnalysisAdapterStateV1 adapter = OutputAnalysisAdapterStateV1::Qualified;
    OutputAnalysisCompressionStateV1 compression = OutputAnalysisCompressionStateV1::Available;
    OutputAnalysisOtherDependencyStateV1 otherDependency =
        OutputAnalysisOtherDependencyStateV1::Available;
};

enum class OutputAnalysisAnalyzerErrorCodeV1 : std::uint8_t {
    None,
    InvalidProcessSourceState,
    InvalidProcessSource,
    InvalidSourceDescriptor,
    InvalidAdapterState,
    InvalidCompressionState,
    InvalidOtherDependencyState,
    InvalidColorResolutionState,
    DescriptorTooLong,
    DescriptorStorageTooLarge,
    AllocationFailure,
    GeneratedReportInvariantViolation,
    InternalInvariant,
};

namespace detail {
class OutputAnalysisAnalyzerV1;
}

class OutputAnalysisReportV1 final {
  public:
    OutputAnalysisReportV1(const OutputAnalysisReportV1&) = delete;
    OutputAnalysisReportV1& operator=(const OutputAnalysisReportV1&) = delete;
    OutputAnalysisReportV1(OutputAnalysisReportV1&& other) noexcept;
    OutputAnalysisReportV1& operator=(OutputAnalysisReportV1&&) = delete;
    ~OutputAnalysisReportV1() = default;

    [[nodiscard]] OutputAnalysisReportV1View view() const& noexcept {
        return {.preset = preset_, .facets = assessmentViews_};
    }
    [[nodiscard]] OutputAnalysisReportV1View view() const&& = delete;
    [[nodiscard]] OutputAnalysisPermissionMaskV1 permissionMask() const noexcept {
        return permissionMask_;
    }
    [[nodiscard]] bool approvable() const noexcept { return permissionMask_.allPermitted(); }
    [[nodiscard]] std::size_t descriptorByteCount() const noexcept { return descriptorByteCount_; }

  private:
    struct OwnedAssessment final {
        OutputFacetIdV1 facet = OutputFacetIdV1::Pixels;
        OutputPreservationStateV1 state = OutputPreservationStateV1::Exact;
        OutputFacetStableCodeV1 stableCode = OutputFacetStableCodeV1::None;
        std::string sourceDescriptor;
        std::string targetDescriptor;
    };

    OutputAnalysisReportV1(OutputPresetV1 preset,
                           std::array<OwnedAssessment, kOutputAnalysisFacetCountV1>&& assessments,
                           OutputAnalysisPermissionMaskV1 permissionMask,
                           std::size_t descriptorByteCount) noexcept;
    void bindAssessmentViews() noexcept;

    OutputPresetV1 preset_ = OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    std::array<OwnedAssessment, kOutputAnalysisFacetCountV1> assessments_;
    std::array<OutputFacetAssessmentV1View, kOutputAnalysisFacetCountV1> assessmentViews_;
    OutputAnalysisPermissionMaskV1 permissionMask_;
    std::size_t descriptorByteCount_ = 0;

    friend class detail::OutputAnalysisAnalyzerV1;
};

class [[nodiscard]] OutputAnalysisAnalyzerResultV1 final {
  public:
    // Results share one immutable report. Explicit copies suppress consuming moves, preserving a
    // coherent source even when an rvalue initializes another result.
    OutputAnalysisAnalyzerResultV1(const OutputAnalysisAnalyzerResultV1&) noexcept = default;
    OutputAnalysisAnalyzerResultV1&
    operator=(const OutputAnalysisAnalyzerResultV1&) noexcept = default;

    [[nodiscard]] bool hasReport() const noexcept { return report_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasReport(); }
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const& noexcept {
        return report_;
    }
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const&& = delete;
    [[nodiscard]] OutputAnalysisAnalyzerErrorCodeV1 error() const noexcept { return error_; }
    [[nodiscard]] OutputAnalysisReportIssueV1 generatedReportIssue() const noexcept {
        return generatedReportIssue_;
    }

  private:
    static OutputAnalysisAnalyzerResultV1
    success(std::shared_ptr<const OutputAnalysisReportV1> report) noexcept;
    static OutputAnalysisAnalyzerResultV1
    failure(OutputAnalysisAnalyzerErrorCodeV1 error,
            OutputAnalysisReportIssueV1 generatedReportIssue = {}) noexcept;

    OutputAnalysisAnalyzerResultV1(std::shared_ptr<const OutputAnalysisReportV1> report,
                                   OutputAnalysisAnalyzerErrorCodeV1 error,
                                   OutputAnalysisReportIssueV1 generatedReportIssue) noexcept;

    std::shared_ptr<const OutputAnalysisReportV1> report_;
    OutputAnalysisAnalyzerErrorCodeV1 error_ = OutputAnalysisAnalyzerErrorCodeV1::InternalInvariant;
    OutputAnalysisReportIssueV1 generatedReportIssue_{};

    friend class detail::OutputAnalysisAnalyzerV1;
};

[[nodiscard]] OutputAnalysisAnalyzerResultV1
analyzePngRgba8SrgbV1(PngRgba8SrgbAnalysisInputV1 input) noexcept;

[[nodiscard]] OutputAnalysisAnalyzerResultV1
analyzeFlatExrRgba32fLinRec709SceneV1(FlatExrRgba32fLinRec709SceneAnalysisInputV1 input) noexcept;

static_assert(!std::is_default_constructible_v<OutputAnalysisReportV1>);
static_assert(!std::is_copy_constructible_v<OutputAnalysisReportV1>);
static_assert(std::is_nothrow_move_constructible_v<OutputAnalysisReportV1>);
static_assert(!std::is_default_constructible_v<OutputAnalysisAnalyzerResultV1>);

} // namespace bloom::output
