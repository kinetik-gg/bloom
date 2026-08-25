#include "output_semantic_identity.hpp"

#include "output_semantic_identity_internal.hpp"

#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::output {

PngRgba8SrgbVerifiedSemanticProductV1::PngRgba8SrgbVerifiedSemanticProductV1(
    std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> boundAnalysis,
    const render::ImageExtent dimensions, std::vector<std::uint8_t>&& rgbaBytes) noexcept
    : boundAnalysis_(std::move(boundAnalysis)), dimensions_(dimensions),
      rgbaBytes_(std::move(rgbaBytes)) {}

FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1::
    FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1(
        std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> boundAnalysis,
        const FlatExrRgba32fSemanticMetadataV1 metadata,
        std::vector<std::uint32_t>&& rgbaComponentBits) noexcept
    : boundAnalysis_(std::move(boundAnalysis)), metadata_(metadata),
      rgbaComponentBits_(std::move(rgbaComponentBits)) {}

OutputSemanticIdentityV1::OutputSemanticIdentityV1(const core::Sha256Digest digest,
                                                   RetainedProducts&& retainedProducts,
                                                   const std::uint64_t preimageByteCount) noexcept
    : digest_(digest), retainedProducts_(std::move(retainedProducts)),
      preimageByteCount_(preimageByteCount) {}

const core::Sha256Digest& OutputSemanticIdentityV1::analysisDigest() const& noexcept {
    if (const auto* png = std::get_if<PngRgba8SrgbVerifiedSemanticProductV1>(&retainedProducts_)) {
        return png->boundAnalysis()->digest();
    }
    const auto* exr =
        std::get_if<FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1>(&retainedProducts_);
    return exr->boundAnalysis()->digest();
}

const std::shared_ptr<const ProcessFrameSemanticIdentityV1>&
OutputSemanticIdentityV1::processIdentity() const& noexcept {
    if (const auto* png = std::get_if<PngRgba8SrgbVerifiedSemanticProductV1>(&retainedProducts_)) {
        return png->boundAnalysis()->processIdentity();
    }
    const auto* exr =
        std::get_if<FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1>(&retainedProducts_);
    return exr->boundAnalysis()->processIdentity();
}

const color::DisplayProcessorIdentityV1*
OutputSemanticIdentityV1::displayProcessorIdentity() const noexcept {
    const auto* png = std::get_if<PngRgba8SrgbVerifiedSemanticProductV1>(&retainedProducts_);
    return png != nullptr ? png->boundAnalysis()->displayProcessorIdentity().get() : nullptr;
}

OutputSemanticPayloadKindV1 OutputSemanticIdentityV1::payloadKind() const noexcept {
    return std::holds_alternative<PngRgba8SrgbVerifiedSemanticProductV1>(retainedProducts_)
               ? OutputSemanticPayloadKindV1::PngRgba8
               : OutputSemanticPayloadKindV1::FlatExrRgba32f;
}

std::span<const std::uint8_t> OutputSemanticIdentityV1::pngRgba8Bytes() const& noexcept {
    const auto* retained = std::get_if<PngRgba8SrgbVerifiedSemanticProductV1>(&retainedProducts_);
    return retained != nullptr ? retained->bytes() : std::span<const std::uint8_t>{};
}

std::span<const std::uint32_t>
OutputSemanticIdentityV1::flatExrRgba32fComponentBits() const& noexcept {
    const auto* retained =
        std::get_if<FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1>(&retainedProducts_);
    return retained != nullptr ? retained->componentBits() : std::span<const std::uint32_t>{};
}

OutputSemanticIdentityV1PreparationResult OutputSemanticIdentityV1PreparationResult::prepared(
    std::shared_ptr<const OutputSemanticIdentityV1> identity) noexcept {
    if (identity == nullptr) {
        return failed(OutputSemanticIdentityErrorCodeV1::InternalInvariant);
    }
    return {OutputSemanticIdentityPreparationStatusV1::Prepared, std::move(identity),
            OutputSemanticIdentityErrorCodeV1::None};
}

OutputSemanticIdentityV1PreparationResult
OutputSemanticIdentityV1PreparationResult::cancelled() noexcept {
    return {OutputSemanticIdentityPreparationStatusV1::Cancelled,
            {},
            OutputSemanticIdentityErrorCodeV1::None};
}

OutputSemanticIdentityV1PreparationResult OutputSemanticIdentityV1PreparationResult::failed(
    const OutputSemanticIdentityErrorCodeV1 error) noexcept {
    return {OutputSemanticIdentityPreparationStatusV1::Failed,
            {},
            error == OutputSemanticIdentityErrorCodeV1::None
                ? OutputSemanticIdentityErrorCodeV1::InternalInvariant
                : error};
}

OutputSemanticIdentityV1PreparationResult::OutputSemanticIdentityV1PreparationResult(
    const OutputSemanticIdentityPreparationStatusV1 status,
    std::shared_ptr<const OutputSemanticIdentityV1> identity,
    const OutputSemanticIdentityErrorCodeV1 error) noexcept
    : status_(status), identity_(std::move(identity)), error_(error) {}

OutputSemanticIdentityV1PreparationResult OutputSemanticIdentityV1Preparer::preparePngRgba8SrgbV1(
    PngRgba8SrgbOutputSemanticIdentityInputV1 input, const runtime::CancellationToken& cancellation,
    const OutputSemanticIdentityProgressCallbackV1& progress) const noexcept {
    return detailPreparePngOutputSemanticIdentityV1(std::move(input), cancellation, progress,
                                                    false);
}

OutputSemanticIdentityV1PreparationResult
OutputSemanticIdentityV1Preparer::prepareFlatExrRgba32fLinRec709SceneV1(
    FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1 input,
    const runtime::CancellationToken& cancellation,
    const OutputSemanticIdentityProgressCallbackV1& progress) const noexcept {
    return detailPrepareFlatExrOutputSemanticIdentityV1(std::move(input), cancellation, progress,
                                                        false);
}

} // namespace bloom::output
