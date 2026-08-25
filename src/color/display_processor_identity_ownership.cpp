#include <bloom/color/display_processor_identity.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::color {

DisplayProcessorIdentityV1::DisplayProcessorIdentityV1(
    std::vector<std::byte>&& canonicalBytes,
    const core::Sha256Digest& expectedOcioRevision) noexcept
    : canonicalBytes_(std::move(canonicalBytes)), expectedOcioRevision_(expectedOcioRevision) {}

DisplayProcessorIdentityV1::DisplayProcessorIdentityV1(DisplayProcessorIdentityV1&& other) noexcept
    : canonicalBytes_(std::move(other.canonicalBytes_)),
      expectedOcioRevision_(other.expectedOcioRevision_) {
    other.canonicalBytes_.clear();
}

std::optional<DisplayProcessorIdentityV1View>
DisplayProcessorIdentityV1::borrowedView() const& noexcept {
    if (!hasValue()) {
        return std::nullopt;
    }
    return DisplayProcessorIdentityV1View(canonicalBytes_, expectedOcioRevision_);
}

DisplayProcessorIdentityV1AdoptionResult::DisplayProcessorIdentityV1AdoptionResult(
    DisplayProcessorIdentityV1&& identity) noexcept
    : identity_(std::move(identity)) {}

DisplayProcessorIdentityV1AdoptionResult::DisplayProcessorIdentityV1AdoptionResult(
    DisplayProcessorIdentityV1AdoptionResult&& other) noexcept
    : identity_(std::move(other.identity_)), errorOffset_(other.errorOffset_), error_(other.error_),
      identityTransferred_(other.identityTransferred_) {
    other.identity_.reset();
    if (error_ == DisplayProcessorIdentityError::None && !identityTransferred_) {
        other.identityTransferred_ = true;
    }
}

DisplayProcessorIdentityV1AdoptionResult::DisplayProcessorIdentityV1AdoptionResult(
    const DisplayProcessorIdentityError error, const std::size_t errorOffset) noexcept
    : errorOffset_(errorOffset), error_(error == DisplayProcessorIdentityError::None
                                            ? DisplayProcessorIdentityError::InternalInvariant
                                            : error) {}

std::optional<DisplayProcessorIdentityV1>
DisplayProcessorIdentityV1AdoptionResult::takeIdentity() && noexcept {
    const bool hadIdentity = hasIdentity();
    auto identity = std::move(identity_);
    identity_.reset();
    identityTransferred_ = identityTransferred_ || hadIdentity;
    return identity;
}

DisplayProcessorIdentityV1AdoptionResult
adoptDisplayProcessorIdentityV1(std::vector<std::byte>&& canonicalBytes) noexcept {
    const auto parsed = parseDisplayProcessorIdentityV1(
        std::span<const std::byte>(canonicalBytes.data(), canonicalBytes.size()));
    if (!parsed) {
        return DisplayProcessorIdentityV1AdoptionResult(parsed.error(), parsed.errorOffset());
    }
    const auto* const identity = parsed.identity();
    if (identity == nullptr) {
        return DisplayProcessorIdentityV1AdoptionResult(
            DisplayProcessorIdentityError::InternalInvariant, 0);
    }
    return DisplayProcessorIdentityV1AdoptionResult(
        DisplayProcessorIdentityV1(std::move(canonicalBytes), identity->expectedOcioRevision()));
}

} // namespace bloom::color
