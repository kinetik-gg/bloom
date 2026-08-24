#include <bloom/host/publication_coordinator.hpp>

#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <unordered_map>
#include <utility>

namespace bloom::host::detail {

struct PublicationTargetRecord final {
    std::uint64_t highestRegisteredIntent = 0;
    std::uint64_t activeTargetClaims = 0;
    std::uint64_t activePublicationGuards = 0;
};

struct PublicationCoordinatorState final {
    explicit PublicationCoordinatorState(const PublicationCoordinatorConfig value) noexcept
        : config(value) {}

    [[nodiscard]] std::optional<std::uint64_t> lowestUnresolvedLocked() const noexcept {
        if (unresolvedIntents.empty()) {
            return std::nullopt;
        }
        return *unresolvedIntents.begin();
    }

    void pruneEligibleTargetsLocked() noexcept {
        const auto lowestUnresolved = lowestUnresolvedLocked();
        for (auto iterator = targets.begin(); iterator != targets.end();) {
            const auto& record = iterator->second;
            const bool beyondUnresolvedFrontier =
                !lowestUnresolved.has_value() || *lowestUnresolved > record.highestRegisteredIntent;
            if (record.activeTargetClaims == 0 && record.activePublicationGuards == 0 &&
                beyondUnresolvedFrontier) {
                iterator = targets.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void abandonAdmission(const std::uint64_t intent) noexcept {
        std::lock_guard lock(mutex);
        unresolvedIntents.erase(intent);
        pruneEligibleTargetsLocked();
    }

    void releaseTargetClaim(const std::uint64_t target) noexcept {
        std::lock_guard lock(mutex);
        const auto iterator = targets.find(target);
        if (iterator == targets.end() || iterator->second.activeTargetClaims == 0 ||
            activeTargetClaims == 0) {
            std::terminate();
        }
        --iterator->second.activeTargetClaims;
        --activeTargetClaims;
        pruneEligibleTargetsLocked();
    }

    void releasePublicationGuard(const std::uint64_t target) noexcept {
        std::lock_guard lock(mutex);
        const auto iterator = targets.find(target);
        if (iterator == targets.end() || iterator->second.activePublicationGuards == 0 ||
            activePublicationGuards == 0) {
            std::terminate();
        }
        --iterator->second.activePublicationGuards;
        --activePublicationGuards;
        pruneEligibleTargetsLocked();
    }

    PublicationCoordinatorConfig config;
    std::mutex mutex;
    std::uint64_t lastIssuedIntent = 0;
    std::set<std::uint64_t> unresolvedIntents;
    std::unordered_map<std::uint64_t, PublicationTargetRecord> targets;
    std::uint64_t activeTargetClaims = 0;
    std::uint64_t activePublicationGuards = 0;
};

struct PublicationTargetClaimState final {
    PublicationTargetClaimState(std::shared_ptr<PublicationCoordinatorState> coordinatorState,
                                const ArtifactTargetKey targetValue,
                                const PublicationIntentId intentValue) noexcept
        : coordinator(std::move(coordinatorState)), target(targetValue), intent(intentValue) {}

    ~PublicationTargetClaimState() {
        if (active) {
            coordinator->releaseTargetClaim(target.value());
        }
    }

    std::shared_ptr<PublicationCoordinatorState> coordinator;
    ArtifactTargetKey target;
    PublicationIntentId intent;
    bool active = false;
    bool enteredPublication = false;
};

} // namespace bloom::host::detail

namespace bloom::host {

PublicationAdmission::PublicationAdmission(
    std::shared_ptr<detail::PublicationCoordinatorState> state,
    const PublicationIntentId intent) noexcept
    : state_(std::move(state)), intent_(intent) {}

PublicationAdmission::PublicationAdmission(PublicationAdmission&& other) noexcept
    : state_(std::move(other.state_)), intent_(std::exchange(other.intent_, {})) {}

PublicationAdmission& PublicationAdmission::operator=(PublicationAdmission&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    abandon();
    state_ = std::move(other.state_);
    intent_ = std::exchange(other.intent_, {});
    return *this;
}

PublicationAdmission::~PublicationAdmission() { abandon(); }

bool PublicationAdmission::isValid() const noexcept {
    return state_ != nullptr && intent_.isValid();
}

void PublicationAdmission::abandon() noexcept {
    auto state = std::move(state_);
    const auto intent = std::exchange(intent_, {});
    if (state != nullptr && intent.isValid()) {
        state->abandonAdmission(intent.value());
    }
}

void PublicationAdmission::consume() noexcept {
    state_.reset();
    intent_ = {};
}

PublicationAdmissionResult::PublicationAdmissionResult(
    const PublicationAdmissionStatus status) noexcept
    : status_(status) {}

PublicationAdmissionResult::PublicationAdmissionResult(PublicationAdmission admission) noexcept
    : status_(PublicationAdmissionStatus::Accepted), admission_(std::move(admission)) {}

PublicationAdmissionResult::operator bool() const noexcept {
    return status_ == PublicationAdmissionStatus::Accepted && admission_.isValid();
}

PublicationIntentId PublicationAdmissionResult::intentId() const noexcept {
    return admission_.intentId();
}

PublicationAdmission PublicationAdmissionResult::takeAdmission() && noexcept {
    return std::move(admission_);
}

PublicationTargetClaim::PublicationTargetClaim(
    std::shared_ptr<detail::PublicationTargetClaimState> state) noexcept
    : state_(std::move(state)) {}

bool PublicationTargetClaim::isValid() const noexcept {
    return state_ != nullptr && state_->active;
}

PublicationIntentId PublicationTargetClaim::intentId() const noexcept {
    return state_ != nullptr ? state_->intent : PublicationIntentId{};
}

ArtifactTargetKey PublicationTargetClaim::target() const noexcept {
    return state_ != nullptr ? state_->target : ArtifactTargetKey{};
}

void PublicationTargetClaim::reset() noexcept { state_.reset(); }

PublicationRegistrationResult::PublicationRegistrationResult(
    const PublicationRegistrationStatus status) noexcept
    : status_(status) {}

PublicationRegistrationResult::PublicationRegistrationResult(PublicationTargetClaim claim) noexcept
    : status_(PublicationRegistrationStatus::Registered), claim_(std::move(claim)) {}

PublicationRegistrationResult::operator bool() const noexcept {
    return status_ == PublicationRegistrationStatus::Registered && claim_.isValid();
}

PublicationTargetClaim PublicationRegistrationResult::takeClaim() && noexcept {
    return std::move(claim_);
}

PublicationGuard::PublicationGuard(
    std::shared_ptr<detail::PublicationTargetClaimState> claim) noexcept
    : claim_(std::move(claim)) {}

PublicationGuard::PublicationGuard(PublicationGuard&& other) noexcept
    : claim_(std::move(other.claim_)) {}

PublicationGuard& PublicationGuard::operator=(PublicationGuard&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    claim_ = std::move(other.claim_);
    return *this;
}

PublicationGuard::~PublicationGuard() { reset(); }

bool PublicationGuard::isValid() const noexcept { return claim_ != nullptr; }

PublicationIntentId PublicationGuard::intentId() const noexcept {
    return claim_ != nullptr ? claim_->intent : PublicationIntentId{};
}

ArtifactTargetKey PublicationGuard::target() const noexcept {
    return claim_ != nullptr ? claim_->target : ArtifactTargetKey{};
}

void PublicationGuard::reset() noexcept {
    auto claim = std::move(claim_);
    if (claim != nullptr) {
        claim->coordinator->releasePublicationGuard(claim->target.value());
    }
}

PublicationGuardResult::PublicationGuardResult(const PublicationGuardStatus status) noexcept
    : status_(status) {}

PublicationGuardResult::PublicationGuardResult(PublicationGuard guard) noexcept
    : status_(PublicationGuardStatus::Entered), guard_(std::move(guard)) {}

PublicationGuardResult::operator bool() const noexcept {
    return status_ == PublicationGuardStatus::Entered && guard_.isValid();
}

PublicationGuard PublicationGuardResult::takeGuard() && noexcept { return std::move(guard_); }

PublicationGuardResult PublicationTargetClaim::tryEnterPublication() & noexcept {
    if (!isValid()) {
        return PublicationGuardResult(PublicationGuardStatus::InvalidClaim);
    }

    const auto& coordinator = state_->coordinator;
    std::lock_guard lock(coordinator->mutex);
    const auto iterator = coordinator->targets.find(state_->target.value());
    if (iterator == coordinator->targets.end()) {
        return PublicationGuardResult(PublicationGuardStatus::InvalidClaim);
    }
    auto& record = iterator->second;
    if (state_->enteredPublication) {
        return PublicationGuardResult(PublicationGuardStatus::AlreadyEntered);
    }
    if (record.highestRegisteredIntent != state_->intent.value()) {
        return PublicationGuardResult(PublicationGuardStatus::Superseded);
    }
    if (record.activePublicationGuards != 0) {
        return PublicationGuardResult(PublicationGuardStatus::TargetBusy);
    }
    if (record.activePublicationGuards == std::numeric_limits<std::uint64_t>::max() ||
        coordinator->activePublicationGuards == std::numeric_limits<std::uint64_t>::max()) {
        return PublicationGuardResult(PublicationGuardStatus::TargetBusy);
    }

    ++record.activePublicationGuards;
    ++coordinator->activePublicationGuards;
    state_->enteredPublication = true;
    return PublicationGuardResult(PublicationGuard(state_));
}

PublicationTargetSnapshotResult::PublicationTargetSnapshotResult(
    const PublicationTargetSnapshotStatus status) noexcept
    : status_(status) {}

PublicationTargetSnapshotResult::PublicationTargetSnapshotResult(
    PublicationTargetSnapshot snapshot) noexcept
    : status_(PublicationTargetSnapshotStatus::Found), snapshot_(snapshot) {}

const PublicationTargetSnapshot& PublicationTargetSnapshotResult::snapshot() const& noexcept {
    return snapshot_;
}

std::optional<PublicationCoordinator>
PublicationCoordinator::create(const PublicationCoordinatorConfig config) noexcept {
    if (!config.isValid()) {
        return std::nullopt;
    }
    try {
        return PublicationCoordinator(
            std::make_shared<detail::PublicationCoordinatorState>(config));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

PublicationCoordinator::PublicationCoordinator(
    std::shared_ptr<detail::PublicationCoordinatorState> state) noexcept
    : state_(std::move(state)) {}

PublicationAdmissionResult PublicationCoordinator::admit() noexcept {
    if (state_ == nullptr) {
        return PublicationAdmissionResult(PublicationAdmissionStatus::ResourceUnavailable);
    }

    std::lock_guard lock(state_->mutex);
    state_->pruneEligibleTargetsLocked();
    if (state_->lastIssuedIntent == std::numeric_limits<std::uint64_t>::max()) {
        return PublicationAdmissionResult(PublicationAdmissionStatus::RuntimeIdentityExhausted);
    }
    if (state_->unresolvedIntents.size() >= state_->config.unresolvedAdmissionLimit ||
        state_->targets.size() >= state_->config.targetRecordLimit) {
        return PublicationAdmissionResult(PublicationAdmissionStatus::PublicationTrackingLimit);
    }

    const auto nextIntent = state_->lastIssuedIntent + 1;
    try {
        state_->unresolvedIntents.insert(nextIntent);
    } catch (const std::bad_alloc&) {
        return PublicationAdmissionResult(PublicationAdmissionStatus::ResourceUnavailable);
    }
    state_->lastIssuedIntent = nextIntent;
    return PublicationAdmissionResult(
        PublicationAdmission(state_, PublicationIntentId::fromRaw(nextIntent)));
}

PublicationRegistrationResult
PublicationCoordinator::registerTarget(PublicationAdmission admission,
                                       const ArtifactTargetKey target) noexcept {
    if (!admission.isValid() || admission.state_ != state_) {
        return PublicationRegistrationResult(PublicationRegistrationStatus::InvalidAdmission);
    }
    if (!target.isValid()) {
        return PublicationRegistrationResult(PublicationRegistrationStatus::InvalidTarget);
    }

    std::shared_ptr<detail::PublicationTargetClaimState> claim;
    try {
        claim = std::make_shared<detail::PublicationTargetClaimState>(state_, target,
                                                                      admission.intent_);
    } catch (const std::bad_alloc&) {
        return PublicationRegistrationResult(PublicationRegistrationStatus::ResourceUnavailable);
    }

    std::lock_guard lock(state_->mutex);
    const auto unresolved = state_->unresolvedIntents.find(admission.intent_.value());
    if (unresolved == state_->unresolvedIntents.end()) {
        return PublicationRegistrationResult(PublicationRegistrationStatus::InvalidAdmission);
    }

    auto targetRecord = state_->targets.find(target.value());
    if (targetRecord == state_->targets.end()) {
        state_->pruneEligibleTargetsLocked();
        if (state_->targets.size() >= state_->config.targetRecordLimit) {
            return PublicationRegistrationResult(
                PublicationRegistrationStatus::PublicationTrackingLimit);
        }
        try {
            const auto [iterator, inserted] = state_->targets.emplace(
                target.value(), detail::PublicationTargetRecord{.highestRegisteredIntent =
                                                                    admission.intent_.value()});
            if (!inserted) {
                std::terminate();
            }
            targetRecord = iterator;
        } catch (const std::bad_alloc&) {
            return PublicationRegistrationResult(
                PublicationRegistrationStatus::ResourceUnavailable);
        }
    }

    auto& record = targetRecord->second;
    if (record.activeTargetClaims == std::numeric_limits<std::uint64_t>::max() ||
        state_->activeTargetClaims == std::numeric_limits<std::uint64_t>::max()) {
        return PublicationRegistrationResult(PublicationRegistrationStatus::ResourceUnavailable);
    }
    if (admission.intent_.value() > record.highestRegisteredIntent) {
        record.highestRegisteredIntent = admission.intent_.value();
    }
    ++record.activeTargetClaims;
    ++state_->activeTargetClaims;
    claim->active = true;
    state_->unresolvedIntents.erase(unresolved);
    admission.consume();
    return PublicationRegistrationResult(PublicationTargetClaim(std::move(claim)));
}

PublicationCoordinatorSnapshot PublicationCoordinator::snapshot() const noexcept {
    if (state_ == nullptr) {
        return {};
    }
    std::lock_guard lock(state_->mutex);
    const auto lowest = state_->lowestUnresolvedLocked();
    return {
        .unresolvedAdmissionCount = state_->unresolvedIntents.size(),
        .unresolvedAdmissionLimit = state_->config.unresolvedAdmissionLimit,
        .targetRecordCount = state_->targets.size(),
        .targetRecordLimit = state_->config.targetRecordLimit,
        .activeTargetClaimCount = state_->activeTargetClaims,
        .activePublicationGuardCount = state_->activePublicationGuards,
        .lastIssuedIntent = PublicationIntentId::fromRaw(state_->lastIssuedIntent),
        .lowestUnresolvedIntent = lowest.has_value() ? std::optional<PublicationIntentId>(
                                                           PublicationIntentId::fromRaw(*lowest))
                                                     : std::nullopt,
        .identityExhausted = state_->lastIssuedIntent == std::numeric_limits<std::uint64_t>::max(),
    };
}

PublicationTargetSnapshotResult
PublicationCoordinator::targetSnapshot(const ArtifactTargetKey target) const noexcept {
    if (!target.isValid()) {
        return PublicationTargetSnapshotResult(PublicationTargetSnapshotStatus::InvalidTarget);
    }
    if (state_ == nullptr) {
        return PublicationTargetSnapshotResult(PublicationTargetSnapshotStatus::NotTracked);
    }
    std::lock_guard lock(state_->mutex);
    const auto iterator = state_->targets.find(target.value());
    if (iterator == state_->targets.end()) {
        return PublicationTargetSnapshotResult(PublicationTargetSnapshotStatus::NotTracked);
    }
    const auto& record = iterator->second;
    return PublicationTargetSnapshotResult({
        .target = target,
        .highestRegisteredIntent = PublicationIntentId::fromRaw(record.highestRegisteredIntent),
        .activeTargetClaimCount = record.activeTargetClaims,
        .activePublicationGuardCount = record.activePublicationGuards,
    });
}

bool PublicationCoordinator::setLastIssuedIntentForTesting(const std::uint64_t value) noexcept {
    if (state_ == nullptr) {
        return false;
    }
    std::lock_guard lock(state_->mutex);
    if (!state_->unresolvedIntents.empty() || !state_->targets.empty() ||
        state_->activeTargetClaims != 0 || state_->activePublicationGuards != 0 ||
        value < state_->lastIssuedIntent) {
        return false;
    }
    state_->lastIssuedIntent = value;
    return true;
}

} // namespace bloom::host
