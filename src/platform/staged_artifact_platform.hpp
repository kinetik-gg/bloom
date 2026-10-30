#ifndef BLOOM_PLATFORM_STAGED_ARTIFACT_PLATFORM_HPP
#define BLOOM_PLATFORM_STAGED_ARTIFACT_PLATFORM_HPP

#include <bloom/platform/staged_artifact.hpp>

#include <memory>
#include <utility>

#include <bloom/core/sha256.hpp>

#include <string>
#include <string_view>

namespace bloom::platform::detail {

// ADR 0033: staged files are named ".bloom-stage-<owner pid>-<target tag>-<nonce>" so a later run
// can remove exactly the stale stages of one target whose owner process is gone.
inline constexpr std::string_view kStagedArtifactPrefix = ".bloom-stage-";
// The first 16 hexadecimal digits of the SHA-256 of the target's file name.
[[nodiscard]] inline std::string stagedArtifactTargetTag(const std::string_view leaf) {
    core::Sha256Hasher hasher;
    (void)hasher.update(std::as_bytes(std::span(leaf.data(), leaf.size())));
    const auto digest = hasher.finalize().toLowercaseHex();
    return std::string(digest.data(), 16);
}
[[nodiscard]] inline std::string stagedArtifactNamePrefix(const std::int64_t pid,
                                                          const std::string_view leaf) {
    return std::string(kStagedArtifactPrefix) + std::to_string(pid) + "-" +
           stagedArtifactTargetTag(leaf) + "-";
}

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
    // Platforms without inheritable descriptors answer UnsupportedPlatform.
    [[nodiscard]] virtual StagedArtifactError openInheritableWriter(int& descriptor) noexcept {
        descriptor = -1;
        return StagedArtifactError::UnsupportedPlatform;
    }
    [[nodiscard]] virtual StagedArtifactOperationResult adoptExternalWrite(std::uint64_t) noexcept {
        return {.error = StagedArtifactError::UnsupportedPlatform, .stageBytes = stageBytes()};
    }
    [[nodiscard]] virtual StagedArtifactOperationResult finishWriting() noexcept = 0;
    [[nodiscard]] virtual StagedArtifactVerificationReadResult
    readForVerification(std::uint64_t offset, std::span<std::byte> destination) noexcept = 0;
    [[nodiscard]] virtual StagedArtifactOperationResult acceptVerification() noexcept = 0;
    [[nodiscard]] virtual StagedArtifactOperationResult rejectVerification() noexcept = 0;
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
