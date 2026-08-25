#ifndef BLOOM_PLATFORM_STAGED_ARTIFACT_HPP
#define BLOOM_PLATFORM_STAGED_ARTIFACT_HPP

#include <bloom/core/artifact_target_key.hpp>
#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace bloom::platform {

inline constexpr std::size_t kDefaultActiveStagedArtifactLimit = 64;
inline constexpr std::size_t kDefaultStagedArtifactTargetRecordLimit = 4096;
inline constexpr std::uint64_t kDefaultStagedArtifactByteLimit = 17'179'869'184ULL;

enum class StagedArtifactError : std::uint8_t {
    None,
    InvalidConfiguration,
    ResourceUnavailable,
    UnsupportedPlatform,
    AdmissionLimit,
    TargetRecordLimit,
    TargetIdentityExhausted,
    CoordinatorMismatch,
    InvalidTargetPath,
    ParentResolutionFailed,
    ParentNotDirectory,
    TargetLeafSymlink,
    TargetLeafNotRegular,
    TargetInspectionFailed,
    ArtifactSizeLimit,
    ExternalModificationConflict,
    OverwritePolicyConflict,
    StageCreationFailed,
    StageIdentityMismatch,
    StageWriteFailed,
    StageNotWritable,
    StageWriterCloseFailed,
    StageReopenFailed,
    StageNotVerifying,
    StageVerificationReadOffsetOutOfRange,
    StageVerificationReadFailed,
    StageFlushFailed,
    StageVerificationNotAccepted,
    StageVerificationRejected,
    InvalidPublicationDisposition,
    ParentIdentityMismatch,
    AtomicCreateUnsupported,
    AtomicPublicationFailed,
    ParentDurabilityFailed,
    FaultInjected,
};

enum class ArtifactOverwritePolicy : std::uint8_t {
    CreateOnly,
    ReplaceExisting,
    CreateOrReplace,
};

enum class StagedArtifactFaultPoint : std::uint8_t {
    None,
    TargetInspection,
    StageCreation,
    StageWrite,
    StageWriterClose,
    StageReopen,
    StageVerificationRead,
    StageVerificationAccept,
    StageFlush,
    IdentityRevalidation,
    AtomicPublication,
    ParentDurability,
};

enum class PublicationDisposition : std::uint8_t {
    Proceed,
    Superseded,
    Cancelled,
};

enum class StagedArtifactPublicationOutcome : std::uint8_t {
    Published,
    PublishedWithDurabilityWarning,
    Superseded,
    CancelledBeforePublication,
    ExternalModificationConflict,
    FailedBeforePublication,
};

struct StagedArtifactFaultPlan final {
    StagedArtifactFaultPoint point = StagedArtifactFaultPoint::None;
    std::uint64_t occurrence = 1;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        switch (point) {
        case StagedArtifactFaultPoint::None:
            return true;
        case StagedArtifactFaultPoint::TargetInspection:
        case StagedArtifactFaultPoint::StageCreation:
        case StagedArtifactFaultPoint::StageWrite:
        case StagedArtifactFaultPoint::StageWriterClose:
        case StagedArtifactFaultPoint::StageReopen:
        case StagedArtifactFaultPoint::StageVerificationRead:
        case StagedArtifactFaultPoint::StageVerificationAccept:
        case StagedArtifactFaultPoint::StageFlush:
        case StagedArtifactFaultPoint::IdentityRevalidation:
        case StagedArtifactFaultPoint::AtomicPublication:
        case StagedArtifactFaultPoint::ParentDurability:
            return occurrence > 0;
        }
        return false;
    }
};

struct StagedArtifactConfig final {
    std::size_t activeTargetLimit = kDefaultActiveStagedArtifactLimit;
    std::size_t targetRecordLimit = kDefaultStagedArtifactTargetRecordLimit;
    std::uint64_t artifactByteLimit = kDefaultStagedArtifactByteLimit;
    StagedArtifactFaultPlan faults;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return activeTargetLimit > 0 && targetRecordLimit > 0 && artifactByteLimit > 0 &&
               faults.isValid();
    }
};

struct ArtifactFileIdentity final {
    std::uint64_t first = 0;
    std::uint64_t second = 0;

    friend constexpr bool operator==(const ArtifactFileIdentity&,
                                     const ArtifactFileIdentity&) noexcept = default;
};

struct ArtifactTargetFingerprint final {
    ArtifactFileIdentity identity;
    std::uint64_t byteSize = 0;
    std::int64_t modificationSeconds = 0;
    std::uint32_t modificationNanoseconds = 0;
    core::Sha256Digest digest;

    friend constexpr bool operator==(const ArtifactTargetFingerprint&,
                                     const ArtifactTargetFingerprint&) noexcept = default;
};

struct ArtifactTargetObservation final {
    bool exists = false;
    ArtifactTargetFingerprint fingerprint;

    [[nodiscard]] static constexpr ArtifactTargetObservation absent() noexcept { return {}; }
    [[nodiscard]] static constexpr ArtifactTargetObservation
    existing(const ArtifactTargetFingerprint value) noexcept {
        return {.exists = true, .fingerprint = value};
    }

    friend constexpr bool operator==(const ArtifactTargetObservation&,
                                     const ArtifactTargetObservation&) noexcept = default;
};

struct StagedArtifactPreflightRequest final {
    std::filesystem::path targetPath;
    ArtifactOverwritePolicy overwritePolicy = ArtifactOverwritePolicy::CreateOrReplace;
    std::optional<ArtifactTargetObservation> expectedTarget;
};

struct StagedArtifactCoordinatorSnapshot final {
    std::size_t activeTargetCount = 0;
    std::size_t activeTargetLimit = 0;
    std::size_t targetRecordCount = 0;
    std::size_t targetRecordLimit = 0;
    std::uint64_t cleanupFailureCount = 0;
    core::ArtifactTargetKey lastIssuedTargetKey;
    bool targetIdentityExhausted = false;
};

struct StagedArtifactOperationResult final {
    StagedArtifactError error = StagedArtifactError::None;
    std::uint64_t stageBytes = 0;

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error == StagedArtifactError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
};

struct StagedArtifactVerificationReadResult final {
    StagedArtifactError error = StagedArtifactError::None;
    std::uint64_t bytesRead = 0;
    std::uint64_t stageBytes = 0;
    bool endOfFile = false;

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error == StagedArtifactError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
};

struct StagedArtifactPublicationResult final {
    StagedArtifactPublicationOutcome outcome =
        StagedArtifactPublicationOutcome::FailedBeforePublication;
    StagedArtifactError error = StagedArtifactError::None;

    [[nodiscard]] constexpr bool targetWasPublished() const noexcept {
        return outcome == StagedArtifactPublicationOutcome::Published ||
               outcome == StagedArtifactPublicationOutcome::PublishedWithDurabilityWarning;
    }
};

namespace detail {
struct StagedArtifactTargetState;
struct StagedArtifactLeaseState;
struct StagedArtifactCoordinatorState;
} // namespace detail

class StagedArtifactCoordinator;
class StagedArtifactPreflightResult;
class StagedArtifactStageResult;
class StagedArtifactCoordinatorResult;

class StagedArtifactTarget final {
  public:
    StagedArtifactTarget(const StagedArtifactTarget&) = delete;
    StagedArtifactTarget& operator=(const StagedArtifactTarget&) = delete;
    StagedArtifactTarget(StagedArtifactTarget&& other) noexcept;
    StagedArtifactTarget& operator=(StagedArtifactTarget&& other) noexcept;
    ~StagedArtifactTarget();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] core::ArtifactTargetKey targetKey() const noexcept;
    [[nodiscard]] ArtifactTargetObservation observation() const noexcept;

  private:
    friend class StagedArtifactCoordinator;
    friend class StagedArtifactPreflightResult;

    StagedArtifactTarget() noexcept = default;
    explicit StagedArtifactTarget(
        std::unique_ptr<detail::StagedArtifactTargetState> state) noexcept;

    std::unique_ptr<detail::StagedArtifactTargetState> state_;
};

class [[nodiscard]] StagedArtifactPreflightResult final {
  public:
    StagedArtifactPreflightResult(const StagedArtifactPreflightResult&) = delete;
    StagedArtifactPreflightResult& operator=(const StagedArtifactPreflightResult&) = delete;
    StagedArtifactPreflightResult(StagedArtifactPreflightResult&& other) noexcept;
    StagedArtifactPreflightResult& operator=(StagedArtifactPreflightResult&& other) noexcept;
    ~StagedArtifactPreflightResult();

    [[nodiscard]] bool succeeded() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] StagedArtifactError error() const noexcept { return error_; }
    [[nodiscard]] const StagedArtifactTarget* target() const& noexcept;
    [[nodiscard]] const StagedArtifactTarget* target() const&& = delete;
    [[nodiscard]] StagedArtifactTarget takeTarget() && noexcept;

  private:
    friend class StagedArtifactCoordinator;

    explicit StagedArtifactPreflightResult(StagedArtifactError error) noexcept;
    explicit StagedArtifactPreflightResult(StagedArtifactTarget target) noexcept;

    StagedArtifactTarget target_;
    StagedArtifactError error_ = StagedArtifactError::None;
};

class StagedArtifactLease final {
  public:
    StagedArtifactLease(const StagedArtifactLease&) = delete;
    StagedArtifactLease& operator=(const StagedArtifactLease&) = delete;
    StagedArtifactLease(StagedArtifactLease&& other) noexcept;
    StagedArtifactLease& operator=(StagedArtifactLease&& other) noexcept;
    ~StagedArtifactLease();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] core::ArtifactTargetKey targetKey() const noexcept;
    [[nodiscard]] std::uint64_t stageBytes() const noexcept;
    [[nodiscard]] StagedArtifactOperationResult write(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] StagedArtifactOperationResult finishWriting() noexcept;
    [[nodiscard]] StagedArtifactVerificationReadResult
    readForVerification(std::uint64_t offset, std::span<std::byte> destination) noexcept;
    [[nodiscard]] StagedArtifactOperationResult acceptVerification() noexcept;
    [[nodiscard]] StagedArtifactOperationResult rejectVerification() noexcept;
    [[nodiscard]] StagedArtifactPublicationResult
    publish(PublicationDisposition disposition) noexcept;

  private:
    friend class StagedArtifactCoordinator;
    friend class StagedArtifactStageResult;

    StagedArtifactLease() noexcept = default;
    explicit StagedArtifactLease(std::unique_ptr<detail::StagedArtifactLeaseState> state) noexcept;

    std::unique_ptr<detail::StagedArtifactLeaseState> state_;
};

class [[nodiscard]] StagedArtifactStageResult final {
  public:
    StagedArtifactStageResult(const StagedArtifactStageResult&) = delete;
    StagedArtifactStageResult& operator=(const StagedArtifactStageResult&) = delete;
    StagedArtifactStageResult(StagedArtifactStageResult&& other) noexcept;
    StagedArtifactStageResult& operator=(StagedArtifactStageResult&& other) noexcept;
    ~StagedArtifactStageResult();

    [[nodiscard]] bool succeeded() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] StagedArtifactError error() const noexcept { return error_; }
    [[nodiscard]] const StagedArtifactLease* lease() const& noexcept;
    [[nodiscard]] const StagedArtifactLease* lease() const&& = delete;
    [[nodiscard]] StagedArtifactLease takeLease() && noexcept;

  private:
    friend class StagedArtifactCoordinator;

    explicit StagedArtifactStageResult(StagedArtifactError error) noexcept;
    explicit StagedArtifactStageResult(StagedArtifactLease lease) noexcept;

    StagedArtifactLease lease_;
    StagedArtifactError error_ = StagedArtifactError::None;
};

class StagedArtifactCoordinator final {
  public:
    StagedArtifactCoordinator(const StagedArtifactCoordinator&) = delete;
    StagedArtifactCoordinator& operator=(const StagedArtifactCoordinator&) = delete;
    StagedArtifactCoordinator(StagedArtifactCoordinator&& other) noexcept;
    StagedArtifactCoordinator& operator=(StagedArtifactCoordinator&& other) noexcept;
    ~StagedArtifactCoordinator();

    [[nodiscard]] static StagedArtifactCoordinatorResult
    create(const StagedArtifactConfig& config) noexcept;

    [[nodiscard]] StagedArtifactPreflightResult
    preflight(const StagedArtifactPreflightRequest& request) noexcept;
    [[nodiscard]] StagedArtifactStageResult stage(StagedArtifactTarget target) noexcept;
    [[nodiscard]] StagedArtifactCoordinatorSnapshot snapshot() const noexcept;

  private:
    friend class StagedArtifactCoordinatorResult;

    StagedArtifactCoordinator() noexcept = default;
    explicit StagedArtifactCoordinator(
        std::unique_ptr<detail::StagedArtifactCoordinatorState> state) noexcept;

    std::unique_ptr<detail::StagedArtifactCoordinatorState> state_;
};

class [[nodiscard]] StagedArtifactCoordinatorResult final {
  public:
    StagedArtifactCoordinatorResult(const StagedArtifactCoordinatorResult&) = delete;
    StagedArtifactCoordinatorResult& operator=(const StagedArtifactCoordinatorResult&) = delete;
    StagedArtifactCoordinatorResult(StagedArtifactCoordinatorResult&& other) noexcept;
    StagedArtifactCoordinatorResult& operator=(StagedArtifactCoordinatorResult&& other) noexcept;
    ~StagedArtifactCoordinatorResult();

    [[nodiscard]] bool succeeded() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] StagedArtifactError error() const noexcept { return error_; }
    [[nodiscard]] StagedArtifactCoordinator takeCoordinator() && noexcept;

  private:
    friend class StagedArtifactCoordinator;

    explicit StagedArtifactCoordinatorResult(StagedArtifactError error) noexcept;
    explicit StagedArtifactCoordinatorResult(StagedArtifactCoordinator coordinator) noexcept;

    StagedArtifactCoordinator coordinator_;
    StagedArtifactError error_ = StagedArtifactError::None;
};

static_assert(!std::is_copy_constructible_v<StagedArtifactTarget>);
static_assert(!std::is_copy_constructible_v<StagedArtifactLease>);
static_assert(!std::is_copy_constructible_v<StagedArtifactCoordinator>);

} // namespace bloom::platform

#endif // BLOOM_PLATFORM_STAGED_ARTIFACT_HPP
