#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/task_types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::runtime {

inline constexpr std::uint32_t kCpuCompositionEvaluatorSemanticsVersion = 2;
inline constexpr std::uint32_t kReferenceDisplayMapperSemanticsVersion = 2;

enum class EvaluationQuality : std::uint8_t {
    Reference,
};

enum class EvaluationColorIntent : std::uint8_t {
    LinearRec709Scene,
};

enum class EvaluationProvider : std::uint8_t {
    CpuReference,
};

enum class EvaluationDisplayIntent : std::uint8_t {
    LinearRec709SceneToSrgb,
};

struct CompositionFormatResolution final {
    friend constexpr bool operator==(const CompositionFormatResolution&,
                                     const CompositionFormatResolution&) noexcept = default;
};

struct ProxyResolution final {
    render::ImageExtent extent;

    friend constexpr bool operator==(const ProxyResolution&,
                                     const ProxyResolution&) noexcept = default;
};

using EvaluationResolution = std::variant<CompositionFormatResolution, ProxyResolution>;

struct EvaluationRequest final {
    core::RationalTime time;
    OperationIndex output;
    EvaluationResolution resolution;
    EvaluationQuality quality = EvaluationQuality::Reference;
    EvaluationColorIntent colorIntent = EvaluationColorIntent::LinearRec709Scene;
    std::size_t pixelStorageByteLimit = 0;
};

// This deliberately retains the complete immutable plan. Exact deep equality is the conservative
// identity until Bloom has a versioned canonical plan digest. Request generation, cancellation,
// priority, and memory budget do not affect pixels and therefore do not belong here.
struct EvaluationCacheIdentity final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    core::RationalTime time;
    OperationIndex output;
    EvaluationResolution resolution;
    EvaluationQuality quality = EvaluationQuality::Reference;
    EvaluationColorIntent colorIntent = EvaluationColorIntent::LinearRec709Scene;
    EvaluationProvider provider = EvaluationProvider::CpuReference;
    EvaluationDisplayIntent displayIntent = EvaluationDisplayIntent::LinearRec709SceneToSrgb;
    std::uint32_t evaluatorSemanticsVersion = kCpuCompositionEvaluatorSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion = kAnimationSamplingSemanticsVersion;
    std::uint32_t imagePrimitiveSemanticsVersion = 0;
    std::uint32_t displayMapperSemanticsVersion = kReferenceDisplayMapperSemanticsVersion;

    friend bool operator==(const EvaluationCacheIdentity& lhs, const EvaluationCacheIdentity& rhs);
};

enum class EvaluationStatus : std::uint8_t {
    Evaluated,
    Cancelled,
    Failed,
};

enum class EvaluationDiagnosticCode : std::uint8_t {
    InvalidRequest,
    InvalidPlan,
    InvalidProxyPixelAspect,
    ArithmeticOverflow,
    PixelStorageBudgetExceeded,
    AllocationFailure,
    InvalidPixel,
    InvalidParameter,
    UnsupportedFloatingPointEnvironment,
    IncompatibleImageDescriptor,
    InternalInvariant,
};

[[nodiscard]] std::string_view evaluationDiagnosticCodeId(EvaluationDiagnosticCode code) noexcept;

struct EvaluationSubject final {
    std::optional<OperationIndex> operation;
    std::optional<document::NodeId> nodeId;
    std::optional<document::LayerId> layerId;
    std::optional<document::ParameterId> parameterId;
    std::optional<document::AnimationCurveId> animationCurveId;
    std::optional<document::KeyframeId> keyframeId;
    std::string field;

    friend bool operator==(const EvaluationSubject&, const EvaluationSubject&) = default;
};

struct EvaluationDiagnostic final {
    EvaluationDiagnosticCode code = EvaluationDiagnosticCode::InternalInvariant;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    EvaluationSubject subject;
    std::string summary;
    std::string detail;

    friend bool operator==(const EvaluationDiagnostic&, const EvaluationDiagnostic&) = default;
};

enum class EvaluationProgressStage : std::uint8_t {
    Preflight,
    Operation,
    DisplayMapping,
};

struct EvaluationProgress final {
    EvaluationProgressStage stage = EvaluationProgressStage::Preflight;
    std::optional<OperationIndex> operation;
    std::uint64_t completed = 0;
    std::optional<std::uint64_t> total;

    friend bool operator==(const EvaluationProgress&, const EvaluationProgress&) = default;
};

using EvaluationProgressCallback = std::function<void(const EvaluationProgress&)>;

class EvaluatedFrame final {
  public:
    EvaluatedFrame(const EvaluatedFrame&) = delete;
    EvaluatedFrame& operator=(const EvaluatedFrame&) = delete;
    EvaluatedFrame(EvaluatedFrame&&) noexcept = default;
    EvaluatedFrame& operator=(EvaluatedFrame&&) noexcept = default;
    ~EvaluatedFrame() = default;

    [[nodiscard]] const EvaluationCacheIdentity& identity() const& noexcept { return identity_; }
    [[nodiscard]] const EvaluationCacheIdentity& identity() const&& = delete;
    [[nodiscard]] const render::Rgba32fImage& processImage() const& noexcept {
        return processImage_;
    }
    [[nodiscard]] const render::Rgba32fImage& processImage() const&& = delete;
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& displayBuffer() const& noexcept {
        return displayBuffer_;
    }
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& displayBuffer() const&& = delete;

  private:
    friend class CpuCompositionEvaluator;

    EvaluatedFrame(EvaluationCacheIdentity identity, render::Rgba32fImage processImage,
                   render::PreparedReferenceDisplayBuffer displayBuffer) noexcept;

    EvaluationCacheIdentity identity_;
    render::Rgba32fImage processImage_;
    render::PreparedReferenceDisplayBuffer displayBuffer_;
};

class EvaluationResult final {
  public:
    [[nodiscard]] static EvaluationResult
    evaluated(std::shared_ptr<const EvaluatedFrame> frame,
              std::vector<EvaluationDiagnostic> diagnostics = {});
    [[nodiscard]] static EvaluationResult
    cancelled(std::vector<EvaluationDiagnostic> diagnostics = {});
    [[nodiscard]] static EvaluationResult failed(EvaluationDiagnostic diagnostic);
    [[nodiscard]] static EvaluationResult failed(std::vector<EvaluationDiagnostic> diagnostics);

    [[nodiscard]] EvaluationStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::shared_ptr<const EvaluatedFrame>& frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] const std::vector<EvaluationDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    EvaluationResult(EvaluationStatus status, std::shared_ptr<const EvaluatedFrame> frame,
                     std::vector<EvaluationDiagnostic> diagnostics) noexcept;

    EvaluationStatus status_ = EvaluationStatus::Failed;
    std::shared_ptr<const EvaluatedFrame> frame_;
    std::vector<EvaluationDiagnostic> diagnostics_;
};

} // namespace bloom::runtime
