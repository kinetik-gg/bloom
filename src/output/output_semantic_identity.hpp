#pragma once

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/output/output_analysis_analyzer.hpp>
#include <bloom/output/output_analysis_digest.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::output {

inline constexpr std::uint16_t kOutputSemanticIdentitySerializationVersionV1 = 1;
inline constexpr std::string_view kPngRgba8SrgbSemanticProfileV1 = "png.ihdr-srgb-idat-iend.v1";
inline constexpr std::string_view kFlatExrRgba32fSemanticProfileV1 =
    "exr.singlepart-scanline-rgba32f.v1";
inline constexpr std::array<std::uint32_t, 8> kFlatExrRec709D65ChromaticitiesBitsV1{
    0x3F23D70AU, 0x3EA8F5C3U, 0x3E99999AU, 0x3F19999AU,
    0x3E19999AU, 0x3D75C28FU, 0x3EA01A37U, 0x3EA872B0U,
};

enum class OutputSemanticPayloadKindV1 : std::uint8_t {
    PngRgba8 = 1,
    FlatExrRgba32f = 2,
};

struct FlatExrInclusiveWindowV1 final {
    std::int32_t xMin;
    std::int32_t yMin;
    std::int32_t xMax;
    std::int32_t yMax;

    friend constexpr bool operator==(const FlatExrInclusiveWindowV1&,
                                     const FlatExrInclusiveWindowV1&) noexcept = default;
};

// Compression, line order, color identity, chromaticities, and the allowlisted metadata profile
// are fixed by this type's version and are deliberately not caller-provided fields.
struct FlatExrRgba32fSemanticMetadataV1 final {
    FlatExrInclusiveWindowV1 dataWindow;
    FlatExrInclusiveWindowV1 displayWindow;
    std::uint32_t pixelAspectRatioBits;
};

namespace detail {
class PngRgba8SrgbSemanticPayloadVerifierV1;
class FlatExrRgba32fLinRec709SceneSemanticPayloadVerifierV1;
class OutputSemanticPayloadV1TestAccess;
} // namespace detail

class PngRgba8SrgbBoundOutputAnalysisV1;
class FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1;

// Production construction is reserved for the matching staged-format verifier; serializer tests
// use a module-private fixture seam. Each product owns every preset-specific semantic value and
// exact bound analysis supplied by its issuer. Production verification remains pending, and this
// serializer neither performs it nor manufactures production evidence.
class PngRgba8SrgbVerifiedSemanticProductV1 final {
  public:
    PngRgba8SrgbVerifiedSemanticProductV1(PngRgba8SrgbVerifiedSemanticProductV1&&) noexcept =
        default;
    PngRgba8SrgbVerifiedSemanticProductV1&
    operator=(PngRgba8SrgbVerifiedSemanticProductV1&&) noexcept = default;
    PngRgba8SrgbVerifiedSemanticProductV1(const PngRgba8SrgbVerifiedSemanticProductV1&) = delete;
    PngRgba8SrgbVerifiedSemanticProductV1&
    operator=(const PngRgba8SrgbVerifiedSemanticProductV1&) = delete;

    [[nodiscard]] const std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1>&
    boundAnalysis() const& noexcept {
        return boundAnalysis_;
    }
    [[nodiscard]] const std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1>&
    boundAnalysis() const&& = delete;
    [[nodiscard]] render::ImageExtent dimensions() const noexcept { return dimensions_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const& noexcept { return rgbaBytes_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const&& = delete;

  private:
    PngRgba8SrgbVerifiedSemanticProductV1(
        std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> boundAnalysis,
        render::ImageExtent dimensions, std::vector<std::uint8_t>&& rgbaBytes) noexcept;

    std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> boundAnalysis_;
    render::ImageExtent dimensions_;
    std::vector<std::uint8_t> rgbaBytes_;

    friend class detail::PngRgba8SrgbSemanticPayloadVerifierV1;
    friend class detail::OutputSemanticPayloadV1TestAccess;
};

class FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1 final {
  public:
    FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1(
        FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1&&) noexcept = default;
    FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1&
    operator=(FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1&&) noexcept = default;
    FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1(
        const FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1&) = delete;
    FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1&
    operator=(const FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1&) = delete;

    [[nodiscard]] const std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>&
    boundAnalysis() const& noexcept {
        return boundAnalysis_;
    }
    [[nodiscard]] const std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>&
    boundAnalysis() const&& = delete;
    [[nodiscard]] const FlatExrRgba32fSemanticMetadataV1& metadata() const& noexcept {
        return metadata_;
    }
    [[nodiscard]] const FlatExrRgba32fSemanticMetadataV1& metadata() const&& = delete;
    [[nodiscard]] std::span<const std::uint32_t> componentBits() const& noexcept {
        return rgbaComponentBits_;
    }
    [[nodiscard]] std::span<const std::uint32_t> componentBits() const&& = delete;

  private:
    FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1(
        std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> boundAnalysis,
        FlatExrRgba32fSemanticMetadataV1 metadata,
        std::vector<std::uint32_t>&& rgbaComponentBits) noexcept;

    std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> boundAnalysis_;
    FlatExrRgba32fSemanticMetadataV1 metadata_;
    std::vector<std::uint32_t> rgbaComponentBits_;

    friend class detail::FlatExrRgba32fLinRec709SceneSemanticPayloadVerifierV1;
    friend class detail::OutputSemanticPayloadV1TestAccess;
};

enum class BoundOutputAnalysisErrorCodeV1 : std::uint8_t {
    None,
    MissingProcessIdentity,
    MissingReport,
    MissingDisplayIdentity,
    DigestRejected,
    AllocationFailure,
    InternalInvariant,
};

class PngRgba8SrgbBoundOutputAnalysisV1Result;
class FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result;

class PngRgba8SrgbBoundOutputAnalysisV1 final {
  public:
    PngRgba8SrgbBoundOutputAnalysisV1(const PngRgba8SrgbBoundOutputAnalysisV1&) = delete;
    PngRgba8SrgbBoundOutputAnalysisV1& operator=(const PngRgba8SrgbBoundOutputAnalysisV1&) = delete;
    PngRgba8SrgbBoundOutputAnalysisV1(PngRgba8SrgbBoundOutputAnalysisV1&&) = delete;
    PngRgba8SrgbBoundOutputAnalysisV1& operator=(PngRgba8SrgbBoundOutputAnalysisV1&&) = delete;

    [[nodiscard]] const core::Sha256Digest& digest() const& noexcept { return digest_; }
    [[nodiscard]] const core::Sha256Digest& digest() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const& noexcept {
        return processIdentity_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const& noexcept {
        return report_;
    }
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const color::DisplayProcessorIdentityV1>&
    displayProcessorIdentity() const& noexcept {
        return displayProcessorIdentity_;
    }
    [[nodiscard]] const std::shared_ptr<const color::DisplayProcessorIdentityV1>&
    displayProcessorIdentity() const&& = delete;

  private:
    PngRgba8SrgbBoundOutputAnalysisV1(
        core::Sha256Digest digest,
        std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
        std::shared_ptr<const OutputAnalysisReportV1> report,
        std::shared_ptr<const color::DisplayProcessorIdentityV1> displayProcessorIdentity) noexcept;

    core::Sha256Digest digest_;
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity_;
    std::shared_ptr<const OutputAnalysisReportV1> report_;
    std::shared_ptr<const color::DisplayProcessorIdentityV1> displayProcessorIdentity_;

    friend class PngRgba8SrgbBoundOutputAnalysisV1Result;
    friend PngRgba8SrgbBoundOutputAnalysisV1Result bindPngRgba8SrgbOutputAnalysisV1(
        std::shared_ptr<const ProcessFrameSemanticIdentityV1>,
        std::shared_ptr<const OutputAnalysisReportV1>, core::Sha256Digest,
        std::shared_ptr<const color::DisplayProcessorIdentityV1>) noexcept;
};

class FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1 final {
  public:
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1(
        const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1&) = delete;
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1&
    operator=(const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1&) = delete;
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1(
        FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1&&) = delete;
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1&
    operator=(FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1&&) = delete;

    [[nodiscard]] const core::Sha256Digest& digest() const& noexcept { return digest_; }
    [[nodiscard]] const core::Sha256Digest& digest() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const& noexcept {
        return processIdentity_;
    }
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const& noexcept {
        return report_;
    }
    [[nodiscard]] const std::shared_ptr<const OutputAnalysisReportV1>& report() const&& = delete;

  private:
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1(
        core::Sha256Digest digest,
        std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
        std::shared_ptr<const OutputAnalysisReportV1> report) noexcept;

    core::Sha256Digest digest_;
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity_;
    std::shared_ptr<const OutputAnalysisReportV1> report_;

    friend class FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result;
    friend FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result
        bindFlatExrRgba32fLinRec709SceneOutputAnalysisV1(
            std::shared_ptr<const ProcessFrameSemanticIdentityV1>,
            std::shared_ptr<const OutputAnalysisReportV1>) noexcept;
};

class [[nodiscard]] PngRgba8SrgbBoundOutputAnalysisV1Result final {
  public:
    PngRgba8SrgbBoundOutputAnalysisV1Result(
        const PngRgba8SrgbBoundOutputAnalysisV1Result&) noexcept = default;
    PngRgba8SrgbBoundOutputAnalysisV1Result&
    operator=(const PngRgba8SrgbBoundOutputAnalysisV1Result&) noexcept = default;

    [[nodiscard]] const std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1>&
    analysis() const& noexcept {
        return analysis_;
    }
    [[nodiscard]] const std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1>&
    analysis() const&& = delete;
    [[nodiscard]] BoundOutputAnalysisErrorCodeV1 error() const noexcept { return error_; }
    [[nodiscard]] OutputAnalysisDigestErrorCodeV1 digestError() const noexcept {
        return digestError_;
    }

  private:
    static PngRgba8SrgbBoundOutputAnalysisV1Result
    success(std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> analysis) noexcept;
    static PngRgba8SrgbBoundOutputAnalysisV1Result
    failure(BoundOutputAnalysisErrorCodeV1 error,
            OutputAnalysisDigestErrorCodeV1 digestError =
                OutputAnalysisDigestErrorCodeV1::None) noexcept;

    PngRgba8SrgbBoundOutputAnalysisV1Result(
        std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> analysis,
        BoundOutputAnalysisErrorCodeV1 error, OutputAnalysisDigestErrorCodeV1 digestError) noexcept;

    std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> analysis_;
    BoundOutputAnalysisErrorCodeV1 error_ = BoundOutputAnalysisErrorCodeV1::InternalInvariant;
    OutputAnalysisDigestErrorCodeV1 digestError_ = OutputAnalysisDigestErrorCodeV1::None;

    friend PngRgba8SrgbBoundOutputAnalysisV1Result bindPngRgba8SrgbOutputAnalysisV1(
        std::shared_ptr<const ProcessFrameSemanticIdentityV1>,
        std::shared_ptr<const OutputAnalysisReportV1>, core::Sha256Digest,
        std::shared_ptr<const color::DisplayProcessorIdentityV1>) noexcept;
};

class [[nodiscard]] FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result final {
  public:
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result(
        const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result&) noexcept = default;
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result&
    operator=(const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result&) noexcept = default;

    [[nodiscard]] const std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>&
    analysis() const& noexcept {
        return analysis_;
    }
    [[nodiscard]] const std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>&
    analysis() const&& = delete;
    [[nodiscard]] BoundOutputAnalysisErrorCodeV1 error() const noexcept { return error_; }
    [[nodiscard]] OutputAnalysisDigestErrorCodeV1 digestError() const noexcept {
        return digestError_;
    }

  private:
    static FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result success(
        std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> analysis) noexcept;
    static FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result
    failure(BoundOutputAnalysisErrorCodeV1 error,
            OutputAnalysisDigestErrorCodeV1 digestError =
                OutputAnalysisDigestErrorCodeV1::None) noexcept;

    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result(
        std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> analysis,
        BoundOutputAnalysisErrorCodeV1 error, OutputAnalysisDigestErrorCodeV1 digestError) noexcept;

    std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> analysis_;
    BoundOutputAnalysisErrorCodeV1 error_ = BoundOutputAnalysisErrorCodeV1::InternalInvariant;
    OutputAnalysisDigestErrorCodeV1 digestError_ = OutputAnalysisDigestErrorCodeV1::None;

    friend FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result
        bindFlatExrRgba32fLinRec709SceneOutputAnalysisV1(
            std::shared_ptr<const ProcessFrameSemanticIdentityV1>,
            std::shared_ptr<const OutputAnalysisReportV1>) noexcept;
};

// These functions are the single digest-computation point. Success retains every exact owning
// product used by the digest so a later output identity cannot pair it with substitute inputs.
[[nodiscard]] PngRgba8SrgbBoundOutputAnalysisV1Result bindPngRgba8SrgbOutputAnalysisV1(
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
    std::shared_ptr<const OutputAnalysisReportV1> report, core::Sha256Digest expectedOcioRevision,
    std::shared_ptr<const color::DisplayProcessorIdentityV1> displayProcessorIdentity) noexcept;
[[nodiscard]] FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result
bindFlatExrRgba32fLinRec709SceneOutputAnalysisV1(
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
    std::shared_ptr<const OutputAnalysisReportV1> report) noexcept;

struct PngRgba8SrgbOutputSemanticIdentityInputV1 final {
    // Authorization of this exact bound digest belongs to FrameExportRequest, outside this slice.
    PngRgba8SrgbVerifiedSemanticProductV1 verifiedProduct;
};

struct FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1 final {
    // Authorization of this exact bound digest belongs to FrameExportRequest, outside this slice.
    FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1 verifiedProduct;
};

enum class OutputSemanticIdentityErrorCodeV1 : std::uint8_t {
    None,
    MissingBoundAnalysis,
    MissingProcessIdentity,
    InvalidProcessIdentity,
    MissingDisplayIdentity,
    InvalidDisplayIdentity,
    InvalidDimensions,
    InvalidWindow,
    ProcessDescriptorMismatch,
    InvalidPixelAspectRatio,
    InvalidSemanticPayload,
    PayloadSizeMismatch,
    ResourceLimitExceeded,
    HashInputTooLarge,
    AllocationFailure,
    InternalInvariant,
};

enum class OutputSemanticIdentityPreparationStatusV1 : std::uint8_t {
    Prepared,
    Cancelled,
    Failed,
};

enum class OutputSemanticIdentityProgressStageV1 : std::uint8_t {
    Preflight,
    HashingSemanticPayload,
    Publishing,
};

struct OutputSemanticIdentityProgressV1 final {
    OutputSemanticIdentityProgressStageV1 stage = OutputSemanticIdentityProgressStageV1::Preflight;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;

    friend bool operator==(const OutputSemanticIdentityProgressV1&,
                           const OutputSemanticIdentityProgressV1&) = default;
};

using OutputSemanticIdentityProgressCallbackV1 =
    std::function<void(const OutputSemanticIdentityProgressV1&)>;

class OutputSemanticIdentityV1Preparer;
class OutputSemanticIdentityV1PreparationResult;

// One immutable publication retains every product whose borrowed bytes entered the digest. Views
// and references remain valid only while this owning identity is retained and unchanged.
class OutputSemanticIdentityV1 final {
  public:
    OutputSemanticIdentityV1(const OutputSemanticIdentityV1&) = delete;
    OutputSemanticIdentityV1& operator=(const OutputSemanticIdentityV1&) = delete;
    OutputSemanticIdentityV1(OutputSemanticIdentityV1&&) = delete;
    OutputSemanticIdentityV1& operator=(OutputSemanticIdentityV1&&) = delete;
    ~OutputSemanticIdentityV1() = default;

    [[nodiscard]] OutputSemanticPayloadKindV1 payloadKind() const noexcept;
    [[nodiscard]] const core::Sha256Digest& digest() const& noexcept { return digest_; }
    [[nodiscard]] const core::Sha256Digest& digest() const&& = delete;
    [[nodiscard]] const core::Sha256Digest& analysisDigest() const& noexcept;
    [[nodiscard]] const core::Sha256Digest& analysisDigest() const&& = delete;
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const& noexcept;
    [[nodiscard]] const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
    processIdentity() const&& = delete;
    [[nodiscard]] const color::DisplayProcessorIdentityV1*
    displayProcessorIdentity() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> pngRgba8Bytes() const& noexcept;
    [[nodiscard]] std::span<const std::uint8_t> pngRgba8Bytes() const&& = delete;
    [[nodiscard]] std::span<const std::uint32_t> flatExrRgba32fComponentBits() const& noexcept;
    [[nodiscard]] std::span<const std::uint32_t> flatExrRgba32fComponentBits() const&& = delete;
    [[nodiscard]] std::uint64_t preimageByteCount() const noexcept { return preimageByteCount_; }

  private:
    using RetainedProducts = std::variant<PngRgba8SrgbVerifiedSemanticProductV1,
                                          FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1>;

    OutputSemanticIdentityV1(core::Sha256Digest digest, RetainedProducts&& retainedProducts,
                             std::uint64_t preimageByteCount) noexcept;

    core::Sha256Digest digest_;
    RetainedProducts retainedProducts_;
    std::uint64_t preimageByteCount_ = 0;

    friend class OutputSemanticIdentityV1Preparer;
    friend OutputSemanticIdentityV1PreparationResult detailPreparePngOutputSemanticIdentityV1(
        PngRgba8SrgbOutputSemanticIdentityInputV1, const runtime::CancellationToken&,
        const OutputSemanticIdentityProgressCallbackV1&, bool) noexcept;
    friend OutputSemanticIdentityV1PreparationResult detailPrepareFlatExrOutputSemanticIdentityV1(
        FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1,
        const runtime::CancellationToken&, const OutputSemanticIdentityProgressCallbackV1&,
        bool) noexcept;
};

class [[nodiscard]] OutputSemanticIdentityV1PreparationResult final {
  public:
    OutputSemanticIdentityV1PreparationResult(
        const OutputSemanticIdentityV1PreparationResult&) noexcept = default;
    OutputSemanticIdentityV1PreparationResult&
    operator=(const OutputSemanticIdentityV1PreparationResult&) noexcept = default;

    [[nodiscard]] OutputSemanticIdentityPreparationStatusV1 status() const noexcept {
        return status_;
    }
    [[nodiscard]] const std::shared_ptr<const OutputSemanticIdentityV1>&
    identity() const& noexcept {
        return identity_;
    }
    [[nodiscard]] const std::shared_ptr<const OutputSemanticIdentityV1>&
    identity() const&& = delete;
    [[nodiscard]] OutputSemanticIdentityErrorCodeV1 error() const noexcept { return error_; }

  private:
    static OutputSemanticIdentityV1PreparationResult
    prepared(std::shared_ptr<const OutputSemanticIdentityV1> identity) noexcept;
    static OutputSemanticIdentityV1PreparationResult cancelled() noexcept;
    static OutputSemanticIdentityV1PreparationResult
    failed(OutputSemanticIdentityErrorCodeV1 error) noexcept;

    OutputSemanticIdentityV1PreparationResult(
        OutputSemanticIdentityPreparationStatusV1 status,
        std::shared_ptr<const OutputSemanticIdentityV1> identity,
        OutputSemanticIdentityErrorCodeV1 error) noexcept;

    OutputSemanticIdentityPreparationStatusV1 status_ =
        OutputSemanticIdentityPreparationStatusV1::Failed;
    std::shared_ptr<const OutputSemanticIdentityV1> identity_;
    OutputSemanticIdentityErrorCodeV1 error_ = OutputSemanticIdentityErrorCodeV1::InternalInvariant;

    friend class OutputSemanticIdentityV1Preparer;
    friend OutputSemanticIdentityV1PreparationResult detailPreparePngOutputSemanticIdentityV1(
        PngRgba8SrgbOutputSemanticIdentityInputV1, const runtime::CancellationToken&,
        const OutputSemanticIdentityProgressCallbackV1&, bool) noexcept;
    friend OutputSemanticIdentityV1PreparationResult detailPrepareFlatExrOutputSemanticIdentityV1(
        FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1,
        const runtime::CancellationToken&, const OutputSemanticIdentityProgressCallbackV1&,
        bool) noexcept;
};

// Worker-style entry points are preset-specific. There is no preset enum plus optional-field union.
class OutputSemanticIdentityV1Preparer final {
  public:
    [[nodiscard]] OutputSemanticIdentityV1PreparationResult preparePngRgba8SrgbV1(
        PngRgba8SrgbOutputSemanticIdentityInputV1 input,
        const runtime::CancellationToken& cancellation,
        const OutputSemanticIdentityProgressCallbackV1& progress = {}) const noexcept;

    [[nodiscard]] OutputSemanticIdentityV1PreparationResult prepareFlatExrRgba32fLinRec709SceneV1(
        FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1 input,
        const runtime::CancellationToken& cancellation,
        const OutputSemanticIdentityProgressCallbackV1& progress = {}) const noexcept;
};

} // namespace bloom::output
