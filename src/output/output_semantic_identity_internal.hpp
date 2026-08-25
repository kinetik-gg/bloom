#pragma once

#include "output_semantic_identity.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace bloom::output {

[[nodiscard]] OutputSemanticIdentityV1PreparationResult
detailPreparePngOutputSemanticIdentityV1(PngRgba8SrgbOutputSemanticIdentityInputV1 input,
                                         const runtime::CancellationToken& cancellation,
                                         const OutputSemanticIdentityProgressCallbackV1& progress,
                                         bool injectAllocationFailure) noexcept;

[[nodiscard]] OutputSemanticIdentityV1PreparationResult
detailPrepareFlatExrOutputSemanticIdentityV1(
    FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1 input,
    const runtime::CancellationToken& cancellation,
    const OutputSemanticIdentityProgressCallbackV1& progress,
    bool injectAllocationFailure) noexcept;

namespace detail {

[[nodiscard]] bool
validOutputSemanticExrPixelBitsV1(std::span<const std::uint32_t, 4> components) noexcept;

[[nodiscard]] std::optional<std::uint64_t>
flatExrInclusiveWindowPixelCountV1(const FlatExrInclusiveWindowV1& window) noexcept;

} // namespace detail

} // namespace bloom::output
