#include <bloom/host/publication_coordinator.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::host {

class PublicationCoordinatorTestAccess final {
  public:
    [[nodiscard]] static bool setLastIssuedIntent(PublicationCoordinator& coordinator,
                                                  const std::uint64_t value) noexcept {
        return coordinator.setLastIssuedIntentForTesting(value);
    }
};

} // namespace bloom::host

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::host::ArtifactTargetKey;
using bloom::host::PublicationAdmission;
using bloom::host::PublicationAdmissionStatus;
using bloom::host::PublicationCoordinator;
using bloom::host::PublicationCoordinatorConfig;
using bloom::host::PublicationGuardStatus;
using bloom::host::PublicationRegistrationStatus;
using bloom::host::PublicationTargetClaim;
using bloom::host::PublicationTargetSnapshotStatus;

[[nodiscard]] std::optional<PublicationAdmission> admit(PublicationCoordinator& coordinator,
                                                        Expectations& expectations,
                                                        const std::string_view context) {
    auto result = coordinator.admit();
    expectations.expect(static_cast<bool>(result), context);
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeAdmission();
}

[[nodiscard]] std::optional<PublicationTargetClaim>
registerTarget(PublicationCoordinator& coordinator, PublicationAdmission admission,
               const ArtifactTargetKey target, Expectations& expectations,
               const std::string_view context) {
    auto result = coordinator.registerTarget(std::move(admission), target);
    expectations.expect(static_cast<bool>(result), context);
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeClaim();
}

void testConfigurationAndAbandonedAdmission(Expectations& expectations) {
    expectations.expect(
        !PublicationCoordinator::create({.unresolvedAdmissionLimit = 0, .targetRecordLimit = 1})
             .has_value(),
        "a zero unresolved limit is invalid");
    expectations.expect(
        !PublicationCoordinator::create({.unresolvedAdmissionLimit = 1, .targetRecordLimit = 0})
             .has_value(),
        "a zero target-record limit is invalid");

    auto coordinatorResult = PublicationCoordinator::create();
    expectations.expect(coordinatorResult.has_value(), "the default coordinator is valid");
    if (!coordinatorResult.has_value()) {
        return;
    }
    auto coordinator = std::move(*coordinatorResult);
    {
        auto first = admit(coordinator, expectations, "the first intent is admitted");
        if (!first.has_value()) {
            return;
        }
        expectations.expect(first->intentId().value() == 1,
                            "the process-local intent sequence starts at one");
        auto moved = std::move(*first);
        expectations.expect(!first->isValid() && moved.isValid(),
                            "moving admission transfers unresolved ownership");
    }
    const auto released = coordinator.snapshot();
    expectations.expect(released.unresolvedAdmissionCount == 0 &&
                            released.lastIssuedIntent.value() == 1,
                        "destroying an admission abandons it without reusing its ID");

    auto second = admit(coordinator, expectations, "a later admission still succeeds");
    if (second.has_value()) {
        expectations.expect(second->intentId().value() == 2,
                            "abandoned intent IDs remain consumed");
        second->abandon();
    }
}

void testInvalidTargetAndAdmission(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the invalid-input fixture creates a coordinator");
        return;
    }
    auto admission = admit(*coordinator, expectations, "invalid-target admission succeeds");
    if (!admission.has_value()) {
        return;
    }
    auto invalidTarget = coordinator->registerTarget(std::move(*admission), ArtifactTargetKey{});
    expectations.expect(
        !invalidTarget && invalidTarget.status() == PublicationRegistrationStatus::InvalidTarget &&
            coordinator->snapshot().unresolvedAdmissionCount == 0,
        "an invalid target consumes and abandons the registration attempt");

    auto valid = admit(*coordinator, expectations, "moved-admission fixture is admitted");
    if (!valid.has_value()) {
        return;
    }
    auto owner = std::move(*valid);
    auto invalidAdmission =
        coordinator->registerTarget(std::move(*valid), ArtifactTargetKey::fromRaw(10));
    expectations.expect(!invalidAdmission &&
                            invalidAdmission.status() ==
                                PublicationRegistrationStatus::InvalidAdmission &&
                            coordinator->snapshot().unresolvedAdmissionCount == 1,
                        "a moved-from admission cannot consume the live unresolved owner");
    owner.abandon();
}

void testLateAliasOrderingAndTombstone(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the alias fixture creates a coordinator");
        return;
    }
    const auto target = ArtifactTargetKey::fromRaw(400);
    auto older = admit(*coordinator, expectations, "the unresolved older alias is admitted");
    auto newer = admit(*coordinator, expectations, "the newer operation is admitted");
    if (!older.has_value() || !newer.has_value()) {
        return;
    }
    auto newerClaim = registerTarget(*coordinator, std::move(*newer), target, expectations,
                                     "the newer operation registers its target");
    if (!newerClaim.has_value()) {
        return;
    }
    newerClaim->reset();
    expectations.expect(coordinator->targetSnapshot(target).status() ==
                            PublicationTargetSnapshotStatus::Found,
                        "an older unresolved alias retains the completed target tombstone");

    auto olderClaim = registerTarget(*coordinator, std::move(*older), target, expectations,
                                     "the late older alias receives a target claim");
    if (!olderClaim.has_value()) {
        return;
    }
    auto entry = olderClaim->tryEnterPublication();
    expectations.expect(!entry && entry.status() == PublicationGuardStatus::Superseded,
                        "a late older alias cannot enter publication");
    olderClaim->reset();
    expectations.expect(coordinator->targetSnapshot(target).status() ==
                            PublicationTargetSnapshotStatus::NotTracked,
                        "the tombstone prunes once no unresolved older intent can arrive");
}

void testPruningFrontier(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the frontier fixture creates a coordinator");
        return;
    }
    const auto target = ArtifactTargetKey::fromRaw(401);
    auto registered = admit(*coordinator, expectations, "the registered intent is admitted");
    auto later = admit(*coordinator, expectations, "the later unresolved intent is admitted");
    if (!registered.has_value() || !later.has_value()) {
        return;
    }
    auto claim = registerTarget(*coordinator, std::move(*registered), target, expectations,
                                "the earlier intent registers its target");
    if (!claim.has_value()) {
        return;
    }
    claim->reset();
    const auto frontierSnapshot = coordinator->snapshot();
    expectations.expect(coordinator->targetSnapshot(target).status() ==
                                PublicationTargetSnapshotStatus::NotTracked &&
                            frontierSnapshot.lowestUnresolvedIntent.has_value() &&
                            frontierSnapshot.lowestUnresolvedIntent->value() == 2,
                        "a tombstone below the global unresolved frontier prunes immediately");
    later->abandon();
}

void testTrackingLimits(Expectations& expectations) {
    auto unresolvedCoordinator =
        PublicationCoordinator::create({.unresolvedAdmissionLimit = 2, .targetRecordLimit = 4});
    if (!unresolvedCoordinator.has_value()) {
        expectations.expect(false, "the unresolved-cap fixture creates a coordinator");
        return;
    }
    auto first = admit(*unresolvedCoordinator, expectations, "unresolved slot one is admitted");
    auto second = admit(*unresolvedCoordinator, expectations, "unresolved slot two is admitted");
    const auto rejected = unresolvedCoordinator->admit();
    expectations.expect(
        !rejected && rejected.status() == PublicationAdmissionStatus::PublicationTrackingLimit &&
            rejected.intentId().value() == 0 &&
            unresolvedCoordinator->snapshot().lastIssuedIntent.value() == 2,
        "the unresolved cap rejects before issuing another ID");
    first.reset();
    second.reset();

    auto targetCoordinator =
        PublicationCoordinator::create({.unresolvedAdmissionLimit = 8, .targetRecordLimit = 1});
    if (!targetCoordinator.has_value()) {
        expectations.expect(false, "the target-cap fixture creates a coordinator");
        return;
    }
    auto oldAlias = admit(*targetCoordinator, expectations, "the pruning blocker is admitted");
    auto pendingNewTarget =
        admit(*targetCoordinator, expectations, "the second target intent is admitted");
    auto firstTargetIntent =
        admit(*targetCoordinator, expectations, "the first target intent is admitted");
    if (!oldAlias.has_value() || !pendingNewTarget.has_value() || !firstTargetIntent.has_value()) {
        return;
    }
    auto firstTargetClaim = registerTarget(*targetCoordinator, std::move(*firstTargetIntent),
                                           ArtifactTargetKey::fromRaw(1), expectations,
                                           "the sole target record is created");
    if (!firstTargetClaim.has_value()) {
        return;
    }
    firstTargetClaim->reset();
    auto targetLimit = targetCoordinator->registerTarget(std::move(*pendingNewTarget),
                                                         ArtifactTargetKey::fromRaw(2));
    expectations.expect(
        !targetLimit &&
            targetLimit.status() == PublicationRegistrationStatus::PublicationTrackingLimit &&
            targetCoordinator->snapshot().unresolvedAdmissionCount == 1,
        "a new target rejects before consuming record capacity beyond the hard cap");
    const auto admissionLimit = targetCoordinator->admit();
    expectations.expect(!admissionLimit && admissionLimit.status() ==
                                               PublicationAdmissionStatus::PublicationTrackingLimit,
                        "admission also rejects while an unprunable target record fills the cap");
    oldAlias->abandon();
    expectations.expect(targetCoordinator->snapshot().targetRecordCount == 0,
                        "advancing the unresolved frontier releases target capacity");
}

void testCrossOperationOrderingAndGuardLifecycle(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the guard fixture creates a coordinator");
        return;
    }
    const auto target = ArtifactTargetKey::fromRaw(777);
    auto olderAdmission = admit(*coordinator, expectations, "the older operation is admitted");
    if (!olderAdmission.has_value()) {
        return;
    }
    auto older = registerTarget(*coordinator, std::move(*olderAdmission), target, expectations,
                                "the older operation registers first");
    if (!older.has_value()) {
        return;
    }
    auto olderEntry = older->tryEnterPublication();
    expectations.expect(static_cast<bool>(olderEntry),
                        "the current winner enters publication exactly once");
    if (!olderEntry) {
        return;
    }
    auto olderGuard = std::move(olderEntry).takeGuard();

    auto newerAdmission = admit(*coordinator, expectations, "the newer operation is admitted");
    if (!newerAdmission.has_value()) {
        return;
    }
    auto newer = registerTarget(*coordinator, std::move(*newerAdmission), target, expectations,
                                "the newer operation registers while the old guard is active");
    if (!newer.has_value()) {
        return;
    }
    auto busy = newer->tryEnterPublication();
    expectations.expect(!busy && busy.status() == PublicationGuardStatus::TargetBusy,
                        "the newer winner waits while the older publication guard is active");
    const auto active = coordinator->targetSnapshot(target);
    expectations.expect(active && active.snapshot().highestRegisteredIntent.value() == 2 &&
                            active.snapshot().activeTargetClaimCount == 2 &&
                            active.snapshot().activePublicationGuardCount == 1,
                        "target snapshots expose bounded ordering and lifecycle counts");

    older->reset();
    expectations.expect(coordinator->snapshot().activeTargetClaimCount == 2,
                        "the publication guard retains its logical target claim");
    olderGuard.reset();
    expectations.expect(coordinator->snapshot().activeTargetClaimCount == 1 &&
                            coordinator->snapshot().activePublicationGuardCount == 0,
                        "guard release ends the older claim only after publication exits");

    auto newerEntry = newer->tryEnterPublication();
    expectations.expect(static_cast<bool>(newerEntry),
                        "the newer operation enters after the older guard exits");
    if (!newerEntry) {
        return;
    }
    auto newerGuard = std::move(newerEntry).takeGuard();
    auto duplicate = newer->tryEnterPublication();
    expectations.expect(!duplicate && duplicate.status() == PublicationGuardStatus::AlreadyEntered,
                        "a target claim cannot publish twice");
    newer->reset();
    newerGuard.reset();
    expectations.expect(coordinator->snapshot().targetRecordCount == 0,
                        "completed guard and claim lifetimes leave no unneeded record");
}

void testNewerRegistrationSupersedesOlderBeforeEntry(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the pre-entry fixture creates a coordinator");
        return;
    }
    const auto target = ArtifactTargetKey::fromRaw(778);
    auto firstAdmission = admit(*coordinator, expectations, "first pre-entry intent admitted");
    auto secondAdmission = admit(*coordinator, expectations, "second pre-entry intent admitted");
    if (!firstAdmission.has_value() || !secondAdmission.has_value()) {
        return;
    }
    auto first = registerTarget(*coordinator, std::move(*firstAdmission), target, expectations,
                                "first pre-entry claim registered");
    auto second = registerTarget(*coordinator, std::move(*secondAdmission), target, expectations,
                                 "second pre-entry claim registered");
    if (!first.has_value() || !second.has_value()) {
        return;
    }
    auto firstEntry = first->tryEnterPublication();
    expectations.expect(!firstEntry && firstEntry.status() == PublicationGuardStatus::Superseded,
                        "a newer registration prevents older publication entry");
    auto secondEntry = second->tryEnterPublication();
    expectations.expect(static_cast<bool>(secondEntry),
                        "the newest registered intent remains eligible");
}

void testIdentityExhaustion(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the exhaustion fixture creates a coordinator");
        return;
    }
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    expectations.expect(bloom::host::PublicationCoordinatorTestAccess::setLastIssuedIntent(
                            *coordinator, maximum - 1),
                        "narrow test access positions a quiescent sequence at its boundary");
    auto finalIntent = coordinator->admit();
    expectations.expect(finalIntent && finalIntent.intentId().value() == maximum,
                        "the final nonzero uint64 intent is issued exactly once");
    if (finalIntent) {
        std::move(finalIntent).takeAdmission().abandon();
    }
    const auto exhausted = coordinator->admit();
    expectations.expect(
        !exhausted && exhausted.status() == PublicationAdmissionStatus::RuntimeIdentityExhausted &&
            coordinator->snapshot().lastIssuedIntent.value() == maximum &&
            coordinator->snapshot().identityExhausted,
        "identity exhaustion rejects without wrap or reuse");
}

void testHandlesOutliveCoordinatorWrapper(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the lifetime fixture creates a coordinator");
        return;
    }
    auto admission = admit(*coordinator, expectations, "the lifetime intent is admitted");
    if (!admission.has_value()) {
        return;
    }
    auto claim =
        registerTarget(*coordinator, std::move(*admission), ArtifactTargetKey::fromRaw(900),
                       expectations, "the lifetime target is registered");
    if (!claim.has_value()) {
        return;
    }
    coordinator.reset();
    auto entry = claim->tryEnterPublication();
    expectations.expect(static_cast<bool>(entry),
                        "a target claim safely retains coordinator state");
    claim->reset();
    expectations.expect(static_cast<bool>(entry),
                        "a guard safely retains its claim after wrapper release");
}

void testConcurrentRegistrationSelectsOneWinner(Expectations& expectations) {
    auto coordinator = PublicationCoordinator::create();
    if (!coordinator.has_value()) {
        expectations.expect(false, "the concurrency fixture creates a coordinator");
        return;
    }
    constexpr std::size_t operationCount = 32;
    const auto target = ArtifactTargetKey::fromRaw(1000);
    std::mutex claimsMutex;
    std::vector<PublicationTargetClaim> claims;
    claims.reserve(operationCount);
    std::vector<std::thread> workers;
    workers.reserve(operationCount);
    for (std::size_t index = 0; index < operationCount; ++index) {
        workers.emplace_back([&] {
            auto admission = coordinator->admit();
            if (!admission) {
                return;
            }
            auto registered =
                coordinator->registerTarget(std::move(admission).takeAdmission(), target);
            if (!registered) {
                return;
            }
            auto claim = std::move(registered).takeClaim();
            std::lock_guard lock(claimsMutex);
            claims.push_back(std::move(claim));
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    expectations.expect(claims.size() == operationCount,
                        "concurrent admission and registration retain every operation");
    if (claims.size() != operationCount) {
        return;
    }
    std::ranges::sort(claims, {},
                      [](const PublicationTargetClaim& claim) { return claim.intentId().value(); });
    std::size_t winners = 0;
    for (auto& claim : claims) {
        auto entry = claim.tryEnterPublication();
        if (entry) {
            ++winners;
        } else {
            expectations.expect(entry.status() == PublicationGuardStatus::Superseded,
                                "every concurrent non-winner is explicitly superseded");
        }
    }
    expectations.expect(winners == 1 && claims.back().intentId().value() == operationCount,
                        "the highest process-wide intent is the sole concurrent winner");
}

} // namespace

int main() {
    Expectations expectations;
    testConfigurationAndAbandonedAdmission(expectations);
    testInvalidTargetAndAdmission(expectations);
    testLateAliasOrderingAndTombstone(expectations);
    testPruningFrontier(expectations);
    testTrackingLimits(expectations);
    testCrossOperationOrderingAndGuardLifecycle(expectations);
    testNewerRegistrationSupersedesOlderBeforeEntry(expectations);
    testIdentityExhaustion(expectations);
    testHandlesOutliveCoordinatorWrapper(expectations);
    testConcurrentRegistrationSelectsOneWinner(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
