#ifndef BLOOM_PLATFORM_STAGED_ARTIFACT_PLATFORM_HPP
#define BLOOM_PLATFORM_STAGED_ARTIFACT_PLATFORM_HPP

#include <bloom/platform/staged_artifact.hpp>

#include <memory>
#include <utility>

namespace bloom::platform::detail {

struct StagedArtifactTargetState {
    StagedArtifactTargetState() = default;
    StagedArtifactTargetState(const StagedArtifactTargetState&) = delete;
    StagedArtifactTargetState& operator=(const StagedArtifactTargetState&) = delete;
    virtual ~StagedArtifactTargetState() = default;

    [[nodiscard]] virtual core::ArtifactTargetKey targetKey() const noexcept = 0;
    [[nodiscard]] virtual ArtifactTargetObservation observation() const noexcept = 0;
};

struct StagedArtifactLeaseState {
    StagedArtifactLeaseState() = default;
    StagedArtifactLeaseState(const StagedArtifactLeaseState&) = delete;
    StagedArtifactLeaseState& operator=(const StagedArtifactLeaseState&) = delete;
    virtual ~StagedArtifactLeaseState() = default;

    [[nodiscard]] virtual core::ArtifactTargetKey targetKey() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t stageBytes() const noexcept = 0;
    [[nodiscard]] virtual StagedArtifactOperationResult
    write(std::span<const std::byte> bytes) noexcept = 0;
    [[nodiscard]] virtual StagedArtifactOperationResult seal() noexcept = 0;
    [[nodiscard]] virtual StagedArtifactPublicationResult
    publish(PublicationDisposition disposition) noexcept = 0;
};

struct PlatformPreflightResult final {
    explicit PlatformPreflightResult(const StagedArtifactError value) noexcept : error(value) {}
    explicit PlatformPreflightResult(std::unique_ptr<StagedArtifactTargetState> value) noexcept
        : target(std::move(value)) {}

    StagedArtifactError error = StagedArtifactError::None;
    std::unique_ptr<StagedArtifactTargetState> target;
};

struct PlatformStageResult final {
    explicit PlatformStageResult(const StagedArtifactError value) noexcept : error(value) {}
    explicit PlatformStageResult(std::unique_ptr<StagedArtifactLeaseState> value) noexcept
        : lease(std::move(value)) {}

    StagedArtifactError error = StagedArtifactError::None;
    std::unique_ptr<StagedArtifactLeaseState> lease;
};

struct StagedArtifactCoordinatorState {
    StagedArtifactCoordinatorState() = default;
    StagedArtifactCoordinatorState(const StagedArtifactCoordinatorState&) = delete;
    StagedArtifactCoordinatorState& operator=(const StagedArtifactCoordinatorState&) = delete;
    virtual ~StagedArtifactCoordinatorState() = default;

    [[nodiscard]] virtual PlatformPreflightResult
    preflight(const StagedArtifactPreflightRequest& request) noexcept = 0;
    [[nodiscard]] virtual PlatformStageResult
    stage(std::unique_ptr<StagedArtifactTargetState> target) noexcept = 0;
    [[nodiscard]] virtual StagedArtifactCoordinatorSnapshot snapshot() const noexcept = 0;
};

struct PlatformCoordinatorResult final {
    explicit PlatformCoordinatorResult(const StagedArtifactError value) noexcept : error(value) {}
    explicit PlatformCoordinatorResult(
        std::unique_ptr<StagedArtifactCoordinatorState> value) noexcept
        : coordinator(std::move(value)) {}

    StagedArtifactError error = StagedArtifactError::None;
    std::unique_ptr<StagedArtifactCoordinatorState> coordinator;
};

[[nodiscard]] PlatformCoordinatorResult
createPlatformStagedArtifactCoordinator(const StagedArtifactConfig& config) noexcept;

} // namespace bloom::platform::detail

#endif // BLOOM_PLATFORM_STAGED_ARTIFACT_PLATFORM_HPP
