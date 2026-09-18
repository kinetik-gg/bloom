#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/operation_cache.hpp>
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

// MEDIA-1 bumps evaluator semantics 5 -> 6: process evaluation now admits interpreted external
// image bytes and composition-rate sequence selection. Decoded source keys bind member digest,
// interpretation and the unchanged Bloom Neutral config revision. Primitive, plan and animation
// semantics remain unchanged: no existing primitive or operand contract changed.
// Bumped to 5 by the blend-mode slice: the Layer Stack stage reads each entry's Layer Output blend
// mode and folds through render::blendLinearRec709SceneRow() instead of compositing source-over
// unconditionally. A composition whose every layer is Normal evaluates to bit-identical pixels, but
// a frame identity from before this change must not compare equal to one from after it, because the
// same plan value can now mean a different picture.
//
// Bumped to 4 by the layer transform breadth slice (task S4): the Layer Output stage now resolves
// five parameters instead of two, resamples through an affine transform, and sizes each layer's
// data window to its own transformed bounds rather than the whole composition. A frame identity
// from before this change must not compare equal to one from after it, even where the pixels
// coincide.
// VECTOR-1: semantics 8 covers transformed vector geometry at output resolution.
// COLOR-3: semantics 9 adds image-effect kernels; existing primitive pixels are unchanged.
inline constexpr std::uint32_t kCpuCompositionEvaluatorSemanticsVersion = 9;

enum class EvaluationQuality : std::uint8_t {
    Reference,
};

inline constexpr std::string_view kLinearRec709SceneColorSpaceId = "lin_rec709_scene";
inline constexpr std::string_view kBloomNeutralOcioConfigUri =
    "bloom://ocio/neutral-v1/config.ocio";

struct EvaluationColorIntent final {
    std::string_view workingColorSpaceId{kLinearRec709SceneColorSpaceId};
    core::Sha256Digest ocioConfigRevision{};
    std::string_view ocioConfigUri{kBloomNeutralOcioConfigUri};

    // Compatibility spelling for existing request builders. Its value is the generalized
    // identity {lin_rec709_scene, empty revision}; new plans should carry the selected config
    // revision alongside the working-space id.
    static const EvaluationColorIntent LinearRec709Scene;

    friend bool operator==(const EvaluationColorIntent&, const EvaluationColorIntent&) = default;
};

enum class EvaluationProvider : std::uint8_t {
    CpuReference,
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
    bool bypassOperationCache = false;
    std::optional<render::ImageWindow> roi = std::nullopt;
};

// This deliberately retains the complete immutable plan. Exact deep equality is the conservative
// identity until Bloom has a versioned canonical plan digest. Request generation, cancellation,
// priority, and memory budget do not affect pixels and therefore do not belong here.
struct ProcessFrameIdentity final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    core::RationalTime time;
    OperationIndex output;
    EvaluationResolution resolution;
    EvaluationQuality quality = EvaluationQuality::Reference;
    EvaluationColorIntent colorIntent = EvaluationColorIntent::LinearRec709Scene;
    EvaluationProvider provider = EvaluationProvider::CpuReference;
    std::uint32_t evaluatorSemanticsVersion = kCpuCompositionEvaluatorSemanticsVersion;
    std::uint32_t animationSamplingSemanticsVersion = kAnimationSamplingSemanticsVersion;
    std::uint32_t imagePrimitiveSemanticsVersion = 0;
    std::optional<render::ImageWindow> roi = std::nullopt;

    friend bool operator==(const ProcessFrameIdentity& lhs, const ProcessFrameIdentity& rhs);
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
};

struct EvaluationProgress final {
    EvaluationProgressStage stage = EvaluationProgressStage::Preflight;
    std::optional<OperationIndex> operation;
    std::uint64_t completed = 0;
    std::optional<std::uint64_t> total;

    friend bool operator==(const EvaluationProgress&, const EvaluationProgress&) = default;
};

using EvaluationProgressCallback = std::function<void(const EvaluationProgress&)>;

class ProcessFrame final {
  public:
    ProcessFrame(const ProcessFrame&) = delete;
    ProcessFrame& operator=(const ProcessFrame&) = delete;
    ProcessFrame(ProcessFrame&&) noexcept = default;
    ProcessFrame& operator=(ProcessFrame&&) noexcept = default;
    ~ProcessFrame() = default;

    [[nodiscard]] const OperationCacheStatistics& operationCacheStatistics() const& noexcept {
        return statistics_;
    }
    [[nodiscard]] const OperationCacheStatistics& operationCacheStatistics() const&& = delete;
    [[nodiscard]] std::span<const EvaluatedOperationBounds> evaluatedBounds() const& noexcept {
        return bounds_;
    }
    [[nodiscard]] std::span<const EvaluatedOperationBounds> evaluatedBounds() const&& = delete;
    [[nodiscard]] std::span<const CompiledValue> valueOutputs() const& noexcept {
        return valueOutputs_;
    }
    [[nodiscard]] std::span<const CompiledValue> valueOutputs() const&& = delete;
    [[nodiscard]] const ProcessFrameIdentity& identity() const& noexcept { return identity_; }
    [[nodiscard]] const ProcessFrameIdentity& identity() const&& = delete;
    [[nodiscard]] const render::Rgba32fImage& processImage() const& noexcept {
        return *processImage_;
    }
    [[nodiscard]] const render::Rgba32fImage& processImage() const&& = delete;

  private:
    friend class CpuCompositionEvaluator;

    ProcessFrame(ProcessFrameIdentity identity,
                 std::shared_ptr<const render::Rgba32fImage> processImage,
                 OperationCacheStatistics statistics, std::vector<EvaluatedOperationBounds> bounds,
                 std::vector<CompiledValue> valueOutputs, std::string contentHash) noexcept;

    std::string contentHash_;
    std::vector<EvaluatedOperationBounds> bounds_;
    std::vector<CompiledValue> valueOutputs_;
    OperationCacheStatistics statistics_;
    ProcessFrameIdentity identity_;
    std::shared_ptr<const render::Rgba32fImage> processImage_;
};

class EvaluationResult final {
  public:
    [[nodiscard]] static EvaluationResult
    evaluated(std::shared_ptr<const ProcessFrame> frame,
              std::vector<EvaluationDiagnostic> diagnostics = {});
    [[nodiscard]] static EvaluationResult
    cancelled(std::vector<EvaluationDiagnostic> diagnostics = {});
    [[nodiscard]] static EvaluationResult failed(EvaluationDiagnostic diagnostic);
    [[nodiscard]] static EvaluationResult failed(std::vector<EvaluationDiagnostic> diagnostics);

    [[nodiscard]] EvaluationStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] const std::vector<EvaluationDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    EvaluationResult(EvaluationStatus status, std::shared_ptr<const ProcessFrame> frame,
                     std::vector<EvaluationDiagnostic> diagnostics) noexcept;

    EvaluationStatus status_ = EvaluationStatus::Failed;
    std::shared_ptr<const ProcessFrame> frame_;
    std::vector<EvaluationDiagnostic> diagnostics_;
};

} // namespace bloom::runtime
