#pragma once

#include "output_semantic_identity.hpp"

#include "output_semantic_identity_internal.hpp"

#include <utility>

namespace bloom::output::detail {

class OutputSemanticPayloadV1TestAccess final {
  public:
    [[nodiscard]] static PngRgba8SrgbVerifiedSemanticProductV1
    forgePngForTest(std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> boundAnalysis,
                    render::ImageExtent dimensions,
                    std::vector<std::uint8_t>&& rgbaBytes) noexcept {
        return {std::move(boundAnalysis), dimensions, std::move(rgbaBytes)};
    }

    [[nodiscard]] static FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1 forgeExrForTest(
        std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> boundAnalysis,
        FlatExrRgba32fSemanticMetadataV1 metadata,
        std::vector<std::uint32_t>&& rgbaComponentBits) noexcept {
        return {std::move(boundAnalysis), metadata, std::move(rgbaComponentBits)};
    }
};

[[nodiscard]] inline OutputSemanticIdentityV1PreparationResult
preparePngOutputSemanticIdentityV1WithAllocationFailure(
    PngRgba8SrgbOutputSemanticIdentityInputV1 input, const runtime::CancellationToken& cancellation,
    const OutputSemanticIdentityProgressCallbackV1& progress = {}) noexcept {
    return detailPreparePngOutputSemanticIdentityV1(std::move(input), cancellation, progress, true);
}

[[nodiscard]] inline OutputSemanticIdentityV1PreparationResult
prepareFlatExrOutputSemanticIdentityV1WithAllocationFailure(
    FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1 input,
    const runtime::CancellationToken& cancellation,
    const OutputSemanticIdentityProgressCallbackV1& progress = {}) noexcept {
    return detailPrepareFlatExrOutputSemanticIdentityV1(std::move(input), cancellation, progress,
                                                        true);
}

} // namespace bloom::output::detail
