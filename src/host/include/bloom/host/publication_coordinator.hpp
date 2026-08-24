#pragma once

#include <bloom/core/artifact_target_key.hpp>
#include <bloom/core/id.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::host {

struct PublicationIntentIdTag;

using PublicationIntentId = core::Id<PublicationIntentIdTag>;
using ArtifactTargetKey = core::ArtifactTargetKey;

inline constexpr std::size_t kDefaultUnresolvedPublicationLimit = 256;
inline constexpr std::size_t kDefaultPublicationTargetRecordLimit = 4096;

struct PublicationCoordinatorConfig final {
    std::size_t unresolvedAdmissionLimit = kDefaultUnresolvedPublicationLimit;
    std::size_t targetRecordLimit = kDefaultPublicationTargetRecordLimit;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return unresolvedAdmissionLimit > 0 && targetRecordLimit > 0;
    }
};

enum class PublicationAdmissionStatus : std::uint8_t {
    Accepted,
    PublicationTrackingLimit,
    RuntimeIdentityExhausted,
    ResourceUnavailable,
};

enum class PublicationRegistrationStatus : std::uint8_t {
    Registered,
    InvalidAdmission,
    InvalidTarget,
    PublicationTrackingLimit,
    ResourceUnavailable,
};

enum class PublicationGuardStatus : std::uint8_t {
    Entered,
    InvalidClaim,
    Superseded,
    TargetBusy,
    AlreadyEntered,
};

enum class PublicationTargetSnapshotStatus : std::uint8_t {
    Found,
    InvalidTarget,
    NotTracked,
};

struct PublicationCoordinatorSnapshot final {
    std::size_t unresolvedAdmissionCount = 0;
    std::size_t unresolvedAdmissionLimit = 0;
    std::size_t targetRecordCount = 0;
    std::size_t targetRecordLimit = 0;
    std::uint64_t activeTargetClaimCount = 0;
    std::uint64_t activePublicationGuardCount = 0;
    PublicationIntentId lastIssuedIntent;
    std::optional<PublicationIntentId> lowestUnresolvedIntent;
    bool identityExhausted = false;

    friend bool operator==(const PublicationCoordinatorSnapshot&,
                           const PublicationCoordinatorSnapshot&) = default;
};

struct PublicationTargetSnapshot final {
    ArtifactTargetKey target;
    PublicationIntentId highestRegisteredIntent;
    std::uint64_t activeTargetClaimCount = 0;
    std::uint64_t activePublicationGuardCount = 0;

    friend bool operator==(const PublicationTargetSnapshot&,
                           const PublicationTargetSnapshot&) = default;
};

namespace detail {
struct PublicationCoordinatorState;
struct PublicationTargetClaimState;
} // namespace detail

class PublicationCoordinator;
class PublicationCoordinatorTestAccess;
class PublicationAdmissionResult;
class PublicationRegistrationResult;
class PublicationGuardResult;

class PublicationAdmission final {
  public:
    PublicationAdmission(const PublicationAdmission&) = delete;
    PublicationAdmission& operator=(const PublicationAdmission&) = delete;
    PublicationAdmission(PublicationAdmission&& other) noexcept;
    PublicationAdmission& operator=(PublicationAdmission&& other) noexcept;
    ~PublicationAdmission();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] PublicationIntentId intentId() const noexcept { return intent_; }
    void abandon() noexcept;

  private:
    friend class PublicationAdmissionResult;
    friend class PublicationCoordinator;

    PublicationAdmission() noexcept = default;
    PublicationAdmission(std::shared_ptr<detail::PublicationCoordinatorState> state,
                         PublicationIntentId intent) noexcept;
    void consume() noexcept;

    std::shared_ptr<detail::PublicationCoordinatorState> state_;
    PublicationIntentId intent_;
};

class [[nodiscard]] PublicationAdmissionResult final {
  public:
    PublicationAdmissionResult(const PublicationAdmissionResult&) = delete;
    PublicationAdmissionResult& operator=(const PublicationAdmissionResult&) = delete;
    PublicationAdmissionResult(PublicationAdmissionResult&&) noexcept = default;
    PublicationAdmissionResult& operator=(PublicationAdmissionResult&&) noexcept = default;
    ~PublicationAdmissionResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] PublicationAdmissionStatus status() const noexcept { return status_; }
    [[nodiscard]] PublicationIntentId intentId() const noexcept;
    [[nodiscard]] PublicationAdmission takeAdmission() && noexcept;

  private:
    friend class PublicationCoordinator;

    explicit PublicationAdmissionResult(PublicationAdmissionStatus status) noexcept;
    explicit PublicationAdmissionResult(PublicationAdmission admission) noexcept;

    PublicationAdmissionStatus status_ = PublicationAdmissionStatus::ResourceUnavailable;
    PublicationAdmission admission_;
};

class PublicationTargetClaim final {
  public:
    PublicationTargetClaim(const PublicationTargetClaim&) = delete;
    PublicationTargetClaim& operator=(const PublicationTargetClaim&) = delete;
    PublicationTargetClaim(PublicationTargetClaim&&) noexcept = default;
    PublicationTargetClaim& operator=(PublicationTargetClaim&&) noexcept = default;
    ~PublicationTargetClaim() = default;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] PublicationIntentId intentId() const noexcept;
    [[nodiscard]] ArtifactTargetKey target() const noexcept;
    [[nodiscard]] PublicationGuardResult tryEnterPublication() & noexcept;
    void reset() noexcept;

  private:
    friend class PublicationRegistrationResult;
    friend class PublicationCoordinator;
    friend class PublicationGuard;

    PublicationTargetClaim() noexcept = default;
    explicit PublicationTargetClaim(
        std::shared_ptr<detail::PublicationTargetClaimState> state) noexcept;

    std::shared_ptr<detail::PublicationTargetClaimState> state_;
};

class [[nodiscard]] PublicationRegistrationResult final {
  public:
    PublicationRegistrationResult(const PublicationRegistrationResult&) = delete;
    PublicationRegistrationResult& operator=(const PublicationRegistrationResult&) = delete;
    PublicationRegistrationResult(PublicationRegistrationResult&&) noexcept = default;
    PublicationRegistrationResult& operator=(PublicationRegistrationResult&&) noexcept = default;
    ~PublicationRegistrationResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] PublicationRegistrationStatus status() const noexcept { return status_; }
    [[nodiscard]] PublicationTargetClaim takeClaim() && noexcept;

  private:
    friend class PublicationCoordinator;

    explicit PublicationRegistrationResult(PublicationRegistrationStatus status) noexcept;
    explicit PublicationRegistrationResult(PublicationTargetClaim claim) noexcept;

    PublicationRegistrationStatus status_ = PublicationRegistrationStatus::ResourceUnavailable;
    PublicationTargetClaim claim_;
};

class PublicationGuard final {
  public:
    PublicationGuard(const PublicationGuard&) = delete;
    PublicationGuard& operator=(const PublicationGuard&) = delete;
    PublicationGuard(PublicationGuard&& other) noexcept;
    PublicationGuard& operator=(PublicationGuard&& other) noexcept;
    ~PublicationGuard();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] PublicationIntentId intentId() const noexcept;
    [[nodiscard]] ArtifactTargetKey target() const noexcept;
    void reset() noexcept;

  private:
    friend class PublicationGuardResult;
    friend class PublicationTargetClaim;

    PublicationGuard() noexcept = default;
    explicit PublicationGuard(std::shared_ptr<detail::PublicationTargetClaimState> claim) noexcept;

    std::shared_ptr<detail::PublicationTargetClaimState> claim_;
};

class [[nodiscard]] PublicationGuardResult final {
  public:
    PublicationGuardResult(const PublicationGuardResult&) = delete;
    PublicationGuardResult& operator=(const PublicationGuardResult&) = delete;
    PublicationGuardResult(PublicationGuardResult&&) noexcept = default;
    PublicationGuardResult& operator=(PublicationGuardResult&&) noexcept = default;
    ~PublicationGuardResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] PublicationGuardStatus status() const noexcept { return status_; }
    [[nodiscard]] PublicationGuard takeGuard() && noexcept;

  private:
    friend class PublicationTargetClaim;

    explicit PublicationGuardResult(PublicationGuardStatus status) noexcept;
    explicit PublicationGuardResult(PublicationGuard guard) noexcept;

    PublicationGuardStatus status_ = PublicationGuardStatus::InvalidClaim;
    PublicationGuard guard_;
};

class [[nodiscard]] PublicationTargetSnapshotResult final {
  public:
    [[nodiscard]] explicit operator bool() const noexcept {
        return status_ == PublicationTargetSnapshotStatus::Found;
    }
    [[nodiscard]] PublicationTargetSnapshotStatus status() const noexcept { return status_; }
    [[nodiscard]] const PublicationTargetSnapshot& snapshot() const& noexcept;
    const PublicationTargetSnapshot& snapshot() const&& = delete;

  private:
    friend class PublicationCoordinator;

    explicit PublicationTargetSnapshotResult(PublicationTargetSnapshotStatus status) noexcept;
    explicit PublicationTargetSnapshotResult(PublicationTargetSnapshot snapshot) noexcept;

    PublicationTargetSnapshotStatus status_ = PublicationTargetSnapshotStatus::NotTracked;
    PublicationTargetSnapshot snapshot_;
};

class PublicationCoordinator final {
  public:
    PublicationCoordinator(const PublicationCoordinator&) = delete;
    PublicationCoordinator& operator=(const PublicationCoordinator&) = delete;
    PublicationCoordinator(PublicationCoordinator&&) noexcept = default;
    PublicationCoordinator& operator=(PublicationCoordinator&&) noexcept = default;
    ~PublicationCoordinator() = default;

    [[nodiscard]] static std::optional<PublicationCoordinator>
    create(PublicationCoordinatorConfig config = {}) noexcept;

    [[nodiscard]] PublicationAdmissionResult admit() noexcept;
    [[nodiscard]] PublicationRegistrationResult registerTarget(PublicationAdmission admission,
                                                               ArtifactTargetKey target) noexcept;
    [[nodiscard]] PublicationCoordinatorSnapshot snapshot() const noexcept;
    [[nodiscard]] PublicationTargetSnapshotResult
    targetSnapshot(ArtifactTargetKey target) const noexcept;

  private:
    friend class PublicationCoordinatorTestAccess;

    explicit PublicationCoordinator(
        std::shared_ptr<detail::PublicationCoordinatorState> state) noexcept;
    [[nodiscard]] bool setLastIssuedIntentForTesting(std::uint64_t value) noexcept;

    std::shared_ptr<detail::PublicationCoordinatorState> state_;
};

} // namespace bloom::host
