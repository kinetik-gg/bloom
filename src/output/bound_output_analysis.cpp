#include "output_semantic_identity.hpp"

#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace bloom::output {

PngRgba8SrgbBoundOutputAnalysisV1::PngRgba8SrgbBoundOutputAnalysisV1(
    const core::Sha256Digest digest,
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
    std::shared_ptr<const OutputAnalysisReportV1> report,
    std::shared_ptr<const color::DisplayProcessorIdentityV1> displayProcessorIdentity) noexcept
    : digest_(digest), processIdentity_(std::move(processIdentity)), report_(std::move(report)),
      displayProcessorIdentity_(std::move(displayProcessorIdentity)) {}

FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1::
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1(
        const core::Sha256Digest digest,
        std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
        std::shared_ptr<const OutputAnalysisReportV1> report) noexcept
    : digest_(digest), processIdentity_(std::move(processIdentity)), report_(std::move(report)) {}

PngRgba8SrgbBoundOutputAnalysisV1Result PngRgba8SrgbBoundOutputAnalysisV1Result::success(
    std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> analysis) noexcept {
    if (analysis == nullptr) {
        return failure(BoundOutputAnalysisErrorCodeV1::InternalInvariant);
    }
    return {std::move(analysis), BoundOutputAnalysisErrorCodeV1::None,
            OutputAnalysisDigestErrorCodeV1::None};
}

PngRgba8SrgbBoundOutputAnalysisV1Result PngRgba8SrgbBoundOutputAnalysisV1Result::failure(
    const BoundOutputAnalysisErrorCodeV1 error,
    const OutputAnalysisDigestErrorCodeV1 digestError) noexcept {
    return {{},
            error == BoundOutputAnalysisErrorCodeV1::None
                ? BoundOutputAnalysisErrorCodeV1::InternalInvariant
                : error,
            digestError};
}

PngRgba8SrgbBoundOutputAnalysisV1Result::PngRgba8SrgbBoundOutputAnalysisV1Result(
    std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1> analysis,
    const BoundOutputAnalysisErrorCodeV1 error,
    const OutputAnalysisDigestErrorCodeV1 digestError) noexcept
    : analysis_(std::move(analysis)), error_(error), digestError_(digestError) {}

FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result
FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::success(
    std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> analysis) noexcept {
    if (analysis == nullptr) {
        return failure(BoundOutputAnalysisErrorCodeV1::InternalInvariant);
    }
    return {std::move(analysis), BoundOutputAnalysisErrorCodeV1::None,
            OutputAnalysisDigestErrorCodeV1::None};
}

FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result
FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::failure(
    const BoundOutputAnalysisErrorCodeV1 error,
    const OutputAnalysisDigestErrorCodeV1 digestError) noexcept {
    return {{},
            error == BoundOutputAnalysisErrorCodeV1::None
                ? BoundOutputAnalysisErrorCodeV1::InternalInvariant
                : error,
            digestError};
}

FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::
    FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result(
        std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1> analysis,
        const BoundOutputAnalysisErrorCodeV1 error,
        const OutputAnalysisDigestErrorCodeV1 digestError) noexcept
    : analysis_(std::move(analysis)), error_(error), digestError_(digestError) {}

PngRgba8SrgbBoundOutputAnalysisV1Result bindPngRgba8SrgbOutputAnalysisV1(
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
    std::shared_ptr<const OutputAnalysisReportV1> report,
    const core::Sha256Digest expectedOcioRevision,
    std::shared_ptr<const color::DisplayProcessorIdentityV1> displayProcessorIdentity) noexcept {
    if (processIdentity == nullptr) {
        return PngRgba8SrgbBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::MissingProcessIdentity);
    }
    if (report == nullptr) {
        return PngRgba8SrgbBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::MissingReport);
    }
    if (displayProcessorIdentity == nullptr) {
        return PngRgba8SrgbBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::MissingDisplayIdentity);
    }
    const OutputAnalysisDigestDependenciesV1 dependencies{
        .expectedOcioRevision = expectedOcioRevision,
        .displayProcessorIdentity = displayProcessorIdentity.get(),
    };
    const auto digest =
        computeOutputAnalysisDigestV1(*processIdentity, report->view(), dependencies);
    if (!digest || digest.digest() == nullptr) {
        return PngRgba8SrgbBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::DigestRejected, digest.error());
    }
    try {
        auto analysis = std::shared_ptr<const PngRgba8SrgbBoundOutputAnalysisV1>(
            new PngRgba8SrgbBoundOutputAnalysisV1(*digest.digest(), std::move(processIdentity),
                                                  std::move(report),
                                                  std::move(displayProcessorIdentity)));
        return PngRgba8SrgbBoundOutputAnalysisV1Result::success(std::move(analysis));
    } catch (const std::bad_alloc&) {
        return PngRgba8SrgbBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::AllocationFailure);
    } catch (const std::length_error&) {
        return PngRgba8SrgbBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::AllocationFailure);
    }
}

FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result
bindFlatExrRgba32fLinRec709SceneOutputAnalysisV1(
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
    std::shared_ptr<const OutputAnalysisReportV1> report) noexcept {
    if (processIdentity == nullptr) {
        return FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::MissingProcessIdentity);
    }
    if (report == nullptr) {
        return FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::MissingReport);
    }
    const auto digest = computeOutputAnalysisDigestV1(*processIdentity, report->view());
    if (!digest || digest.digest() == nullptr) {
        return FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::DigestRejected, digest.error());
    }
    try {
        auto analysis = std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>(
            new FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1(
                *digest.digest(), std::move(processIdentity), std::move(report)));
        return FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::success(
            std::move(analysis));
    } catch (const std::bad_alloc&) {
        return FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::AllocationFailure);
    } catch (const std::length_error&) {
        return FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1Result::failure(
            BoundOutputAnalysisErrorCodeV1::AllocationFailure);
    }
}

} // namespace bloom::output
