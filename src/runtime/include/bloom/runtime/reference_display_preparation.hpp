#pragma once

#include <bloom/render/display_buffer.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

inline constexpr std::uint32_t kReferenceDisplayMapperSemanticsVersion = 2;

enum class ReferenceDisplayIntent : std::uint8_t {
    LinearRec709SceneToSrgb,
};

enum class ReferenceDisplayProvider : std::uint8_t {
    CpuReference,
};

enum class ReferenceDisplayPipeline : std::uint8_t {
    UnqualifiedLinearRec709SceneToSrgb,
};

enum class ReferenceDisplayPacking : std::uint8_t {
    StraightRgba8,
};

struct ReferenceDisplayPreparationRequest final {
    ReferenceDisplayIntent intent = ReferenceDisplayIntent::LinearRec709SceneToSrgb;
    std::size_t aggregatePixelStorageByteLimit = 0;
};

struct ReferenceDisplayFrameIdentity final {
    ProcessFrameIdentity processFrame;
    ReferenceDisplayIntent intent = ReferenceDisplayIntent::LinearRec709SceneToSrgb;
    ReferenceDisplayProvider provider = ReferenceDisplayProvider::CpuReference;
    ReferenceDisplayPipeline pipeline =
        ReferenceDisplayPipeline::UnqualifiedLinearRec709SceneToSrgb;
    ReferenceDisplayPacking packing = ReferenceDisplayPacking::StraightRgba8;
    std::uint32_t mapperSemanticsVersion = kReferenceDisplayMapperSemanticsVersion;

    friend bool operator==(const ReferenceDisplayFrameIdentity&,
                           const ReferenceDisplayFrameIdentity&) = default;
};

enum class ReferenceDisplayPreparationStatus : std::uint8_t {
    Prepared,
    Cancelled,
    Failed,
};

enum class ReferenceDisplayDiagnosticCode : std::uint8_t {
    InvalidRequest,
    ArithmeticOverflow,
    PixelStorageBudgetExceeded,
    AllocationFailure,
    InvalidPixel,
    UnsupportedFloatingPointEnvironment,
    IncompatibleImageDescriptor,
    InternalInvariant,
};

[[nodiscard]] std::string_view
referenceDisplayDiagnosticCodeId(ReferenceDisplayDiagnosticCode code) noexcept;

struct ReferenceDisplayDiagnostic final {
    ReferenceDisplayDiagnosticCode code = ReferenceDisplayDiagnosticCode::InternalInvariant;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string summary;
    std::string detail;

    friend bool operator==(const ReferenceDisplayDiagnostic&,
                           const ReferenceDisplayDiagnostic&) = default;
};

enum class ReferenceDisplayProgressStage : std::uint8_t {
    Preflight,
    Mapping,
};

struct ReferenceDisplayProgress final {
    ReferenceDisplayProgressStage stage = ReferenceDisplayProgressStage::Preflight;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;

    friend bool operator==(const ReferenceDisplayProgress&,
                           const ReferenceDisplayProgress&) = default;
};

using ReferenceDisplayProgressCallback = std::function<void(const ReferenceDisplayProgress&)>;

class ReferenceDisplayFrame final {
  public:
    ReferenceDisplayFrame(const ReferenceDisplayFrame&) = delete;
    ReferenceDisplayFrame& operator=(const ReferenceDisplayFrame&) = delete;
    ReferenceDisplayFrame(ReferenceDisplayFrame&&) noexcept = default;
    ReferenceDisplayFrame& operator=(ReferenceDisplayFrame&&) noexcept = default;
    ~ReferenceDisplayFrame() = default;

    [[nodiscard]] const ReferenceDisplayFrameIdentity& identity() const& noexcept {
        return identity_;
    }
    [[nodiscard]] const ReferenceDisplayFrameIdentity& identity() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const& noexcept {
        return processFrame_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrame>& processFrame() const&& = delete;
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& buffer() const& noexcept {
        return buffer_;
    }
    [[nodiscard]] const render::PreparedReferenceDisplayBuffer& buffer() const&& = delete;

  private:
    friend class CpuReferenceDisplayPreparer;

    ReferenceDisplayFrame(ReferenceDisplayFrameIdentity identity,
                          std::shared_ptr<const ProcessFrame> processFrame,
                          render::PreparedReferenceDisplayBuffer buffer) noexcept;

    ReferenceDisplayFrameIdentity identity_;
    std::shared_ptr<const ProcessFrame> processFrame_;
    render::PreparedReferenceDisplayBuffer buffer_;
};

class ReferenceDisplayPreparationResult final {
  public:
    [[nodiscard]] static ReferenceDisplayPreparationResult
    prepared(std::shared_ptr<const ReferenceDisplayFrame> frame,
             std::vector<ReferenceDisplayDiagnostic> diagnostics = {});
    [[nodiscard]] static ReferenceDisplayPreparationResult
    cancelled(std::vector<ReferenceDisplayDiagnostic> diagnostics = {});
    [[nodiscard]] static ReferenceDisplayPreparationResult
    failed(ReferenceDisplayDiagnostic diagnostic);
    [[nodiscard]] static ReferenceDisplayPreparationResult
    failed(std::vector<ReferenceDisplayDiagnostic> diagnostics);

    [[nodiscard]] ReferenceDisplayPreparationStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::shared_ptr<const ReferenceDisplayFrame>& frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] const std::vector<ReferenceDisplayDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    ReferenceDisplayPreparationResult(ReferenceDisplayPreparationStatus status,
                                      std::shared_ptr<const ReferenceDisplayFrame> frame,
                                      std::vector<ReferenceDisplayDiagnostic> diagnostics) noexcept;

    ReferenceDisplayPreparationStatus status_ = ReferenceDisplayPreparationStatus::Failed;
    std::shared_ptr<const ReferenceDisplayFrame> frame_;
    std::vector<ReferenceDisplayDiagnostic> diagnostics_;
};

class CpuReferenceDisplayPreparer final {
  public:
    [[nodiscard]] ReferenceDisplayPreparationResult
    prepare(std::shared_ptr<const ProcessFrame> processFrame,
            const ReferenceDisplayPreparationRequest& request,
            const CancellationToken& cancellation,
            const ReferenceDisplayProgressCallback& progress = {}) const;
};

} // namespace bloom::runtime
