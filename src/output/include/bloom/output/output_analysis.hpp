#pragma once

#include <bloom/output/output_facet_descriptor.hpp>
#include <bloom/output/output_limits.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::output {

inline constexpr std::uint16_t kOutputAnalysisSerializationVersionV1 = 1;
inline constexpr std::uint32_t kOutputPresetVersionV1 = 1;
inline constexpr std::size_t kOutputAnalysisFacetCountV1 = 11;
inline constexpr std::uint16_t kOutputAnalysisAllFacetsPermittedV1 = 0x07FFU;
enum class OutputPresetV1 : std::uint8_t {
    PngRgba8SrgbV1 = 1,
    FlatExrRgba32fLinRec709SceneV1 = 2,
};

enum class OutputFacetIdV1 : std::uint8_t {
    Pixels = 1,
    Precision = 2,
    Color = 3,
    AlphaAssociation = 4,
    Channels = 5,
    DataWindow = 6,
    DisplayWindow = 7,
    PixelAspect = 8,
    Compression = 9,
    Metadata = 10,
    ExternalDependencies = 11,
};

// The explicit values are part of OutputAnalysis serialization version 1.
enum class OutputPreservationStateV1 : std::uint8_t {
    Exact = 1,
    Equivalent = 2,
    Approximated = 3,
    Omitted = 4,
    ExternalReference = 5,
    Missing = 6,
    Unsupported = 7,
};

// Enum ordinals are process-local implementation details. Only outputFacetStableCodeTextV1()
// supplies the portable spelling.
enum class OutputFacetStableCodeV1 : std::uint8_t {
    None,
    PngDisplayTransformClampQuantize,
    ProcessFrameMissing,
    PixelsUnsupported,
    PngFloat32ToUint8,
    PrecisionUnsupported,
    PngLinRec709SceneToSrgb,
    OcioMissing,
    OcioChanged,
    OcioInvalid,
    OcioResourceMissing,
    OcioVersionUnsupported,
    ColorUnsupported,
    PngPremultipliedToStraight,
    AlphaUnsupported,
    ChannelsUnsupported,
    PngOriginWindowRequired,
    WindowOutOfRange,
    PngEqualWindowRequired,
    PngSquarePixelRequired,
    ExrParRoundedBinary32,
    PixelAspectUnsupported,
    CompressionUnavailable,
    CompressionUnsupported,
    MetadataUnsupported,
    PngOcioExternalReference,
    DependencyMissing,
    AdapterUnavailable,
    ResourceLimitExceeded,
};

struct OutputPresetIdentityV1 final {
    std::string_view serializedId;
    std::uint32_t version;
    std::string_view outputPixelSemanticsProfileId;
};

struct OutputFacetDescriptorSchemasV1 final {
    OutputFacetDescriptorSchemaV1 source;
    OutputFacetDescriptorSchemaV1 target;
};

struct OutputFacetStableCodeRuleV1 final {
    std::string_view serializedCode;
    std::uint16_t facetMask;
    OutputPreservationStateV1 requiredState;
    bool validForPng;
    bool validForFlatExr;
    bool presetPermits;

    [[nodiscard]] constexpr bool appliesToFacet(const OutputFacetIdV1 facet) const noexcept {
        const auto value = static_cast<std::uint8_t>(facet);
        return value >= static_cast<std::uint8_t>(OutputFacetIdV1::Pixels) &&
               value <= static_cast<std::uint8_t>(OutputFacetIdV1::ExternalDependencies) &&
               (facetMask & (std::uint16_t{1} << (value - 1U))) != 0;
    }
};

[[nodiscard]] std::optional<OutputPresetIdentityV1>
outputPresetIdentityV1(OutputPresetV1 preset) noexcept;
[[nodiscard]] std::optional<OutputFacetDescriptorSchemasV1>
outputFacetDescriptorSchemasV1(OutputPresetV1 preset, OutputFacetIdV1 facet) noexcept;
[[nodiscard]] std::optional<OutputFacetStableCodeRuleV1>
outputFacetStableCodeRuleV1(OutputFacetStableCodeV1 code) noexcept;
[[nodiscard]] std::optional<std::string_view>
outputFacetStableCodeTextV1(OutputFacetStableCodeV1 code) noexcept;

struct OutputFacetAssessmentV1View final {
    OutputFacetIdV1 facet;
    OutputPreservationStateV1 state;
    OutputFacetStableCodeV1 stableCode;
    std::string_view sourceDescriptor;
    std::string_view targetDescriptor;
};

struct OutputAnalysisReportV1View final {
    OutputPresetV1 preset;
    std::span<const OutputFacetAssessmentV1View> facets;
};

class OutputAnalysisPermissionMaskV1 final {
  public:
    [[nodiscard]] constexpr std::uint16_t bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr bool permits(const OutputFacetIdV1 facet) const noexcept {
        const auto value = static_cast<std::uint8_t>(facet);
        return value >= static_cast<std::uint8_t>(OutputFacetIdV1::Pixels) &&
               value <= static_cast<std::uint8_t>(OutputFacetIdV1::ExternalDependencies) &&
               (bits_ & (std::uint16_t{1} << (value - 1U))) != 0;
    }
    [[nodiscard]] constexpr bool allPermitted() const noexcept {
        return bits_ == kOutputAnalysisAllFacetsPermittedV1;
    }

  private:
    explicit constexpr OutputAnalysisPermissionMaskV1(const std::uint16_t bits) noexcept
        : bits_(bits) {}

    std::uint16_t bits_ = 0;

    friend class OutputAnalysisReportValidationV1;
};

enum class OutputAnalysisReportErrorCodeV1 : std::uint8_t {
    None,
    InvalidPreset,
    IncorrectFacetCount,
    InvalidFacet,
    FacetOutOfOrder,
    InvalidState,
    InvalidStableCode,
    StableCodeFacetMismatch,
    StableCodePresetMismatch,
    StateMismatch,
    NominalTupleMismatch,
    SourceDescriptorInvalid,
    TargetDescriptorInvalid,
    DescriptorVocabularyMismatch,
    DescriptorRelationshipMismatch,
    InternalInvariant,
};

enum class OutputFacetDescriptorSideV1 : std::uint8_t {
    None,
    Source,
    Target,
};

struct OutputAnalysisReportIssueV1 final {
    OutputAnalysisReportErrorCodeV1 code = OutputAnalysisReportErrorCodeV1::None;
    std::size_t facetIndex = 0;
    OutputFacetDescriptorSideV1 descriptorSide = OutputFacetDescriptorSideV1::None;
    OutputFacetDescriptorErrorCode descriptorError = OutputFacetDescriptorErrorCode::None;
    std::size_t descriptorErrorOffset = 0;
};

class [[nodiscard]] OutputAnalysisReportValidationV1 final {
  public:
    [[nodiscard]] constexpr bool hasValue() const noexcept { return permissionMask_.has_value(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr std::optional<OutputAnalysisPermissionMaskV1>
    permissionMask() const& noexcept {
        return permissionMask_;
    }
    [[nodiscard]] std::optional<OutputAnalysisPermissionMaskV1> permissionMask() const&& = delete;
    [[nodiscard]] constexpr bool approvable() const& noexcept {
        return permissionMask_ && permissionMask_->allPermitted();
    }
    [[nodiscard]] bool approvable() const&& = delete;
    [[nodiscard]] constexpr OutputAnalysisReportIssueV1 issue() const noexcept { return issue_; }

  private:
    [[nodiscard]] static constexpr OutputAnalysisReportValidationV1
    success(const std::uint16_t permissionBits) noexcept {
        return OutputAnalysisReportValidationV1(OutputAnalysisPermissionMaskV1(permissionBits));
    }
    [[nodiscard]] static constexpr OutputAnalysisReportValidationV1
    failure(OutputAnalysisReportIssueV1 issue) noexcept {
        if (issue.code == OutputAnalysisReportErrorCodeV1::None) {
            issue.code = OutputAnalysisReportErrorCodeV1::InternalInvariant;
        }
        return OutputAnalysisReportValidationV1(issue);
    }

    explicit constexpr OutputAnalysisReportValidationV1(
        const OutputAnalysisPermissionMaskV1 permissionMask) noexcept
        : permissionMask_(permissionMask) {}
    explicit constexpr OutputAnalysisReportValidationV1(
        const OutputAnalysisReportIssueV1 issue) noexcept
        : issue_(issue) {}

    // Keep only facts derived during validation. In particular, this result never retains a report
    // span or any descriptor string_view into caller-owned storage.
    std::optional<OutputAnalysisPermissionMaskV1> permissionMask_;
    OutputAnalysisReportIssueV1 issue_{};

    friend OutputAnalysisReportValidationV1
        validateOutputAnalysisReportV1(OutputAnalysisReportV1View) noexcept;
};

// Validation is allocation-free and separates malformed reports from valid reports whose derived
// permission mask is non-approvable. It validates the closed tuple vocabulary and descriptor
// relationships internal to the report. A future digest stage must additionally bind the dynamic
// source fields to its validated ProcessFrame and the PNG dependency fields to its validated
// display-processor identity. Digest intake accepts the raw report view and synchronously repeats
// this validation; no validation result serves as a borrowed report token.
[[nodiscard]] OutputAnalysisReportValidationV1
validateOutputAnalysisReportV1(OutputAnalysisReportV1View report) noexcept;

static_assert(std::is_trivially_copyable_v<OutputFacetAssessmentV1View>);
static_assert(std::is_trivially_copyable_v<OutputAnalysisReportV1View>);

} // namespace bloom::output
