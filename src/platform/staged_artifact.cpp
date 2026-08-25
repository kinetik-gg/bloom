#include <bloom/platform/staged_artifact.hpp>

#include "staged_artifact_platform.hpp"

#include <memory>
#include <utility>

namespace bloom::platform {

StagedArtifactTarget::StagedArtifactTarget(StagedArtifactTarget&& other) noexcept = default;
StagedArtifactTarget&
StagedArtifactTarget::operator=(StagedArtifactTarget&& other) noexcept = default;
StagedArtifactTarget::~StagedArtifactTarget() = default;

StagedArtifactTarget::StagedArtifactTarget(
    std::unique_ptr<detail::StagedArtifactTargetState> state) noexcept
    : state_(std::move(state)) {}

bool StagedArtifactTarget::isValid() const noexcept { return state_ != nullptr; }

core::ArtifactTargetKey StagedArtifactTarget::targetKey() const noexcept {
    return state_ != nullptr ? state_->targetKey() : core::ArtifactTargetKey{};
}

ArtifactTargetObservation StagedArtifactTarget::observation() const noexcept {
    return state_ != nullptr ? state_->observation() : ArtifactTargetObservation::absent();
}

StagedArtifactPreflightResult::StagedArtifactPreflightResult(
    StagedArtifactPreflightResult&& other) noexcept = default;
StagedArtifactPreflightResult&
StagedArtifactPreflightResult::operator=(StagedArtifactPreflightResult&& other) noexcept = default;
StagedArtifactPreflightResult::~StagedArtifactPreflightResult() = default;

StagedArtifactPreflightResult::StagedArtifactPreflightResult(
    const StagedArtifactError error) noexcept
    : error_(error) {}

StagedArtifactPreflightResult::StagedArtifactPreflightResult(StagedArtifactTarget target) noexcept
    : target_(std::move(target)) {}

bool StagedArtifactPreflightResult::succeeded() const noexcept {
    return error_ == StagedArtifactError::None && target_.isValid();
}

const StagedArtifactTarget* StagedArtifactPreflightResult::target() const& noexcept {
    return succeeded() ? &target_ : nullptr;
}

StagedArtifactTarget StagedArtifactPreflightResult::takeTarget() && noexcept {
    return std::move(target_);
}

StagedArtifactLease::StagedArtifactLease(StagedArtifactLease&& other) noexcept = default;
StagedArtifactLease& StagedArtifactLease::operator=(StagedArtifactLease&& other) noexcept = default;
StagedArtifactLease::~StagedArtifactLease() = default;

StagedArtifactLease::StagedArtifactLease(
    std::unique_ptr<detail::StagedArtifactLeaseState> state) noexcept
    : state_(std::move(state)) {}

bool StagedArtifactLease::isValid() const noexcept { return state_ != nullptr; }

core::ArtifactTargetKey StagedArtifactLease::targetKey() const noexcept {
    return state_ != nullptr ? state_->targetKey() : core::ArtifactTargetKey{};
}

std::uint64_t StagedArtifactLease::stageBytes() const noexcept {
    return state_ != nullptr ? state_->stageBytes() : 0;
}

StagedArtifactOperationResult
StagedArtifactLease::write(const std::span<const std::byte> bytes) noexcept {
    if (state_ == nullptr) {
        return {.error = StagedArtifactError::StageNotWritable};
    }
    return state_->write(bytes);
}

StagedArtifactOperationResult StagedArtifactLease::seal() noexcept {
    if (state_ == nullptr) {
        return {.error = StagedArtifactError::StageNotWritable};
    }
    return state_->seal();
}

StagedArtifactPublicationResult
StagedArtifactLease::publish(const PublicationDisposition disposition) noexcept {
    if (state_ == nullptr) {
        return {.outcome = StagedArtifactPublicationOutcome::FailedBeforePublication,
                .error = StagedArtifactError::StageNotSealed};
    }
    return state_->publish(disposition);
}

StagedArtifactStageResult::StagedArtifactStageResult(StagedArtifactStageResult&& other) noexcept =
    default;
StagedArtifactStageResult&
StagedArtifactStageResult::operator=(StagedArtifactStageResult&& other) noexcept = default;
StagedArtifactStageResult::~StagedArtifactStageResult() = default;

StagedArtifactStageResult::StagedArtifactStageResult(const StagedArtifactError error) noexcept
    : error_(error) {}

StagedArtifactStageResult::StagedArtifactStageResult(StagedArtifactLease lease) noexcept
    : lease_(std::move(lease)) {}

bool StagedArtifactStageResult::succeeded() const noexcept {
    return error_ == StagedArtifactError::None && lease_.isValid();
}

const StagedArtifactLease* StagedArtifactStageResult::lease() const& noexcept {
    return succeeded() ? &lease_ : nullptr;
}

StagedArtifactLease StagedArtifactStageResult::takeLease() && noexcept { return std::move(lease_); }

StagedArtifactCoordinator::StagedArtifactCoordinator(StagedArtifactCoordinator&& other) noexcept =
    default;
StagedArtifactCoordinator&
StagedArtifactCoordinator::operator=(StagedArtifactCoordinator&& other) noexcept = default;
StagedArtifactCoordinator::~StagedArtifactCoordinator() = default;

StagedArtifactCoordinator::StagedArtifactCoordinator(
    std::unique_ptr<detail::StagedArtifactCoordinatorState> state) noexcept
    : state_(std::move(state)) {}

StagedArtifactCoordinatorResult
StagedArtifactCoordinator::create(const StagedArtifactConfig& config) noexcept {
    if (!config.isValid()) {
        return StagedArtifactCoordinatorResult(StagedArtifactError::InvalidConfiguration);
    }
    auto platformResult = detail::createPlatformStagedArtifactCoordinator(config);
    if (platformResult.coordinator == nullptr) {
        return StagedArtifactCoordinatorResult(platformResult.error);
    }
    return StagedArtifactCoordinatorResult(
        StagedArtifactCoordinator(std::move(platformResult.coordinator)));
}

StagedArtifactPreflightResult
StagedArtifactCoordinator::preflight(const StagedArtifactPreflightRequest& request) noexcept {
    if (state_ == nullptr) {
        return StagedArtifactPreflightResult(StagedArtifactError::InvalidConfiguration);
    }
    auto result = state_->preflight(request);
    if (result.target == nullptr) {
        return StagedArtifactPreflightResult(result.error);
    }
    return StagedArtifactPreflightResult(StagedArtifactTarget(std::move(result.target)));
}

StagedArtifactStageResult StagedArtifactCoordinator::stage(StagedArtifactTarget target) noexcept {
    if (state_ == nullptr || !target.isValid()) {
        return StagedArtifactStageResult(StagedArtifactError::InvalidTargetPath);
    }
    auto result = state_->stage(std::move(target.state_));
    if (result.lease == nullptr) {
        return StagedArtifactStageResult(result.error);
    }
    return StagedArtifactStageResult(StagedArtifactLease(std::move(result.lease)));
}

StagedArtifactCoordinatorSnapshot StagedArtifactCoordinator::snapshot() const noexcept {
    return state_ != nullptr ? state_->snapshot() : StagedArtifactCoordinatorSnapshot{};
}

StagedArtifactCoordinatorResult::StagedArtifactCoordinatorResult(
    StagedArtifactCoordinatorResult&& other) noexcept = default;
StagedArtifactCoordinatorResult& StagedArtifactCoordinatorResult::operator=(
    StagedArtifactCoordinatorResult&& other) noexcept = default;
StagedArtifactCoordinatorResult::~StagedArtifactCoordinatorResult() = default;

StagedArtifactCoordinatorResult::StagedArtifactCoordinatorResult(
    const StagedArtifactError error) noexcept
    : error_(error) {}

StagedArtifactCoordinatorResult::StagedArtifactCoordinatorResult(
    StagedArtifactCoordinator coordinator) noexcept
    : coordinator_(std::move(coordinator)) {}

bool StagedArtifactCoordinatorResult::succeeded() const noexcept {
    return error_ == StagedArtifactError::None && coordinator_.state_ != nullptr;
}

StagedArtifactCoordinator StagedArtifactCoordinatorResult::takeCoordinator() && noexcept {
    return std::move(coordinator_);
}

} // namespace bloom::platform
