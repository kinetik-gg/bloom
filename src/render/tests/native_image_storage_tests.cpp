#include <bloom/render/native_image_storage.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom::render;
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
NativeImageStorageToken& token(NativeImageStorageGrant& grant) {
    auto& admitted = grant.token;
    if (!admitted)
        throw std::runtime_error("expected admitted native image token");
    return *admitted;
}
GpuProcessingPhaseRequest request() {
    return {.fixedHostBytes = 10,
            .deviceWorkingBytes = 40,
            .requestedExecutorBytes = 60,
            .minimumExecutorBytes = 60,
            .sourceFree = true};
}
NativeImageStorageIdentity local() {
    return {.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
}
NativeImageStorageIdentity nonlocal() {
    return {.allocatorGeneration = 7, .memoryType = 3, .heap = 0, .deviceLocal = false};
}
void empty(const GpuProcessingPhaseBudget& budget) {
    const auto s = budget.snapshot();
    require(s.hostReservedBytes == 0 && s.totalHostClaimBytes == 0 && s.deviceReservedBytes == 0 &&
                s.totalDeviceClaimBytes == 0 && s.ownedNativeImageBytes == 0 &&
                s.ownedNonlocalNativeHostBytes == 0 && s.ownedNativeImages == 0 &&
                s.admittedPhases == 0 && s.unprovenPhases == 0,
            "all neutral phase/ownership constraints return to zero");
}
void atomicAdmissionAndFreshness() {
    GpuProcessingPhaseBudget budget;
    const auto now = Clock::now();
    budget.setHostCapacity(100, true, now + std::chrono::seconds(10));
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    require(phase.lease && phase.lease->deviceBytes() == 100, "immutable native grant admitted");
    auto device = NativeImageStorageToken::reserve(phase.lease, 60, local(), now);
    require(device.token.has_value(), "local image transfers preadmitted device debit");
    auto s = budget.snapshot();
    require(s.deviceReservedBytes == 40 && s.ownedNativeImageBytes == 60 &&
                s.totalDeviceClaimBytes == 100 && s.totalHostClaimBytes == 10,
            "local handoff conserves device total and does not charge BAR as host");
    const auto denied = NativeImageStorageToken::reserve(phase.lease, 50, nonlocal(), now);
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Device &&
                budget.snapshot().totalHostClaimBytes == 10,
            "native refusal cannot partially admit supplemental host");
    auto host = NativeImageStorageToken::reserve(phase.lease, 30, nonlocal(), now);
    require(host.token.has_value(), "nonlocal actual heap acquires additive host claim");
    s = budget.snapshot();
    require(s.ownedNativeImageBytes == 90 && s.ownedNonlocalNativeHostBytes == 30 &&
                s.totalDeviceClaimBytes == 100 && s.totalHostClaimBytes == 40,
            "nonlocal subset is separately charged without consuming host estimate");
    auto wrong = local();
    ++wrong.allocatorGeneration;
    require(!token(device).reconcile(40, wrong) && !token(device).reconcile(61, local()),
            "wrong-generation reconciliation and postallocation top-up rejected unchanged");
    require(token(device).reconcile(40, local()) && phase.lease->deviceBytes() == 100,
            "downward reconciliation preserves immutable grant");
    budget.setHostCapacity(35, true, now + std::chrono::seconds(10));
    const auto before = budget.snapshot();
    const auto hostDenied = NativeImageStorageToken::reserve(phase.lease, 1, nonlocal(), now);
    require(hostDenied.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                hostDenied.domain == GpuProcessingPhaseDomain::Host &&
                budget.snapshot().deviceReservedBytes == before.deviceReservedBytes &&
                budget.snapshot().ownedNativeImages == before.ownedNativeImages,
            "shrink below live host total refuses both halves without underflow/revocation");
    budget.setHostCapacity(100, true, now);
    const auto expired = NativeImageStorageToken::reserve(phase.lease, 1, nonlocal(),
                                                          now + std::chrono::nanoseconds(1));
    require(expired.status == GpuProcessingPhaseAdmission::AwaitingCapacity && !expired.token,
            "new additive host claim checks delivered deadline during admitted graph");
    budget.setDeviceCapacity(0, true);
    auto grandfathered =
        NativeImageStorageToken::reserve(phase.lease, 30, local(), now + std::chrono::seconds(11));
    require(grandfathered.token.has_value(),
            "existing device grant conversion survives stale host and shrunk device quota");
    grandfathered.token.reset();
    host.token.reset();
    device.token.reset();
    s = budget.snapshot();
    require(s.deviceReservedBytes == 100 && s.totalDeviceClaimBytes == 100 &&
                s.totalHostClaimBytes == 10 && s.ownedNativeImages == 0,
            "all provisional debits fold back once before phase release");
    phase.lease.reset();
    empty(budget);
}
void promotedPrefixFailureAndExternalRelease() {
    GpuProcessingPhaseBudget budget(2);
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto externalPhase = budget.reserve(request());
    auto external = NativeImageStorageToken::reserve(externalPhase.lease, 50, nonlocal());
    require(external.token && token(external).promote(), "independent retained image promoted");
    externalPhase.lease.reset();
    auto phase = budget.reserve(request());
    auto blocker = budget.reserve(request());
    require(phase.lease && blocker.lease, "two admitted attempts fill count capacity");
    auto ownPrefix = NativeImageStorageToken::reserve(phase.lease, 40, local());
    auto ownScratch = NativeImageStorageToken::reserve(phase.lease, 30, nonlocal());
    require(ownPrefix.token && token(ownPrefix).promote() && ownScratch.token,
            "per-image prefix promoted while unrelated scratch stays provisional");
    auto refused = budget.reserve(request());
    require(refused.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                refused.domain == GpuProcessingPhaseDomain::RequestCount,
            "real typed later refusal after provisional allocation and prefix promotion");
    const auto watermark = phase.lease->revisionAfterOwnRelease();
    const auto revision = budget.snapshot().admissionRevision;
    // Neutral control proof: the caller asserts backing is gone and no external/cache holder.
    require(token(ownPrefix).releaseFailedAttemptAfterBackingDestroyed(phase.lease),
            "exact own destroyed prefix folds back without synthetic independent release");
    ownPrefix.token.reset();
    ownScratch.token.reset();
    require(budget.snapshot().admissionRevision == revision,
            "own prefix plus provisional cleanup does not advance retry revision");
    phase.lease.reset();
    require(budget.snapshot().admissionRevision == watermark,
            "whole failed attempt has exact R+1 despite earlier prefix promotion");
    // Last good independent image still retained; its true disposal must wake the refused intent.
    external.token.reset();
    require(budget.snapshot().admissionRevision == watermark + 1,
            "independent retained release AFTER failure remains visible");
    auto recovery = budget.reserve(request());
    require(recovery.lease != nullptr, "one released count permits genuine new admission");
    recovery.lease.reset();
    blocker.lease.reset();
    empty(budget);

    externalPhase = budget.reserve(request());
    external = NativeImageStorageToken::reserve(externalPhase.lease, 50, local());
    require(external.token && token(external).promote(), "new independent publication");
    externalPhase.lease.reset();
    phase = budget.reserve(request());
    blocker = budget.reserve(request());
    ownPrefix = NativeImageStorageToken::reserve(phase.lease, 40, nonlocal());
    require(ownPrefix.token && token(ownPrefix).promote(), "second own prefix publication");
    refused = budget.reserve(request());
    require(refused.domain == GpuProcessingPhaseDomain::RequestCount, "second real later refusal");
    const auto beforeDelivery = phase.lease->revisionAfterOwnRelease();
    external.token.reset();
    require(token(ownPrefix).releaseFailedAttemptAfterBackingDestroyed(phase.lease),
            "own cleanup is explicit even with independent interleaving");
    ownPrefix.token.reset();
    phase.lease.reset();
    require(budget.snapshot().admissionRevision == beforeDelivery + 1,
            "external release BEFORE failed delivery is not hidden by own cleanup");
    blocker.lease.reset();
    empty(budget);
}
void expiredDeliveryRecovery() {
    GpuProcessingPhaseBudget budget(1);
    const auto now = Clock::now();
    budget.setHostCapacity(1000, true, now);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    const auto before = budget.snapshot();
    const auto expiredTime = now + std::chrono::nanoseconds(1);
    const auto refused = NativeImageStorageToken::reserve(phase.lease, 40, nonlocal(), expiredTime);
    require(refused.status == GpuProcessingPhaseAdmission::AwaitingCapacity && !refused.token,
            "expired additive host claim emits real typed awaiting refusal");
    const auto closed = budget.snapshot();
    require(!closed.hostReady && closed.admissionRevision == before.admissionRevision + 1 &&
                closed.totalDeviceClaimBytes == before.totalDeviceClaimBytes &&
                closed.totalHostClaimBytes == before.totalHostClaimBytes,
            "expiry closes readiness once without dropping held claims");
    const auto repeated =
        NativeImageStorageToken::reserve(phase.lease, 40, nonlocal(), expiredTime);
    require(repeated.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                budget.snapshot().admissionRevision == closed.admissionRevision,
            "repeated expired refusal cannot spin admission revision");
    phase.lease.reset();
    require(!budget.snapshot().hostReady,
            "own final release alone cannot reopen stale readiness for controller retry");
    const auto afterOwn = budget.snapshot().admissionRevision;
    budget.setHostCapacity(1000, true, now + std::chrono::seconds(10));
    require(budget.snapshot().hostReady && budget.snapshot().admissionRevision == afterOwn + 1 &&
                budget.snapshot().admissionRevision > refused.revisionAfterOwnRelease,
            "fresh same-capacity delivery visibly reopens typed-refusal recovery");
    auto recovery = budget.reserve(request());
    auto admitted = NativeImageStorageToken::reserve(recovery.lease, 40, nonlocal(), expiredTime);
    require(admitted.token.has_value(), "same bytes admit after actual fresh delivery");
    admitted.token.reset();
    recovery.lease.reset();
    empty(budget);
}
void identityUnknownAndLifetime() {
    auto budget = std::make_unique<GpuProcessingPhaseBudget>();
    budget->setHostCapacity(1000, true);
    budget->setDeviceCapacity(1000, true);
    auto phase = budget->reserve(request());
    auto other = budget->reserve(request());
    GpuProcessingPhaseBudget foreign;
    foreign.setHostCapacity(1000, true);
    foreign.setDeviceCapacity(1000, true);
    auto foreignPhase = foreign.reserve(request());
    auto image = NativeImageStorageToken::reserve(phase.lease, 40, local());
    require(image.token && token(image).promote(), "image published before ownership test");
    require(!token(image).releaseFailedAttemptAfterBackingDestroyed(other.lease) &&
                !token(image).releaseFailedAttemptAfterBackingDestroyed(foreignPhase.lease) &&
                token(image).bytes() == 40,
            "wrong phase id and foreign root cannot return published credit");
    auto wrongHeap = nonlocal();
    ++wrongHeap.heap;
    auto scratch = NativeImageStorageToken::reserve(phase.lease, 30, nonlocal());
    require(scratch.token && !token(scratch).reconcile(20, wrongHeap),
            "different actual heap cannot reconcile claimed domain");
    auto wrongType = nonlocal();
    ++wrongType.memoryType;
    require(!token(scratch).reconcile(20, wrongType),
            "different actual memory type rejected with same heap and domain");
    phase.lease->markUnproven();
    require(!token(scratch).promote() &&
                !token(image).releaseFailedAttemptAfterBackingDestroyed(phase.lease),
            "promotion or failed disposal cannot clear unknown phase protection");
    scratch.token.reset();
    require(budget->snapshot().unprovenPhases == 1 && budget->snapshot().admittedPhases == 2,
            "neutral scratch disposal leaves independent sticky phase intact");
    // Actual native integration must retain backing through unknown work. This neutral test only
    // observes that the independently held lease's latch survives token operations.
    phase.lease.reset();
    other.lease.reset();
    foreignPhase.lease.reset();
    require(budget->snapshot().ownedNativeImageBytes == 40 &&
                budget->snapshot().totalDeviceClaimBytes == 40 &&
                budget->snapshot().admittedPhases == 0,
            "published allocation does not pin phase/count");
    auto retained = std::make_shared<NativeImageStorageToken>(std::move(token(image)));
    image.token.reset();
    std::vector<std::shared_ptr<NativeImageStorageToken>> aliases(256, retained);
    require(budget->snapshot().ownedNativeImages == 1 &&
                budget->snapshot().ownedNativeImageBytes == 40,
            "256 aliases/move retain exactly one suballocation claim");
    const auto releaseRevision = budget->snapshot().admissionRevision;
    aliases.clear();
    std::thread releaser([token = std::move(retained)]() mutable { token.reset(); });
    releaser.join();
    require(budget->snapshot().admissionRevision == releaseRevision + 1,
            "true independent last release revises once");
    empty(*budget);
    phase = budget->reserve(request());
    auto surviving = NativeImageStorageToken::reserve(phase.lease, 40, nonlocal());
    require(surviving.token && token(surviving).promote(), "root lifetime witness publication");
    phase.lease.reset();
    budget.reset();
    surviving.token.reset(); // Neutral state survives root facade; no device/worker owner retained.
    empty(foreign);
}
void publishedPeakTurnover() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    require(phase.lease != nullptr, "turnover neutral phase admitted");
    for (int i = 0; i != 3; ++i) {
        auto image = NativeImageStorageToken::reserve(phase.lease, 100, nonlocal());
        require(image.token.has_value(), "same peak envelope admits nonoverlapping image");
        require(token(image).promote(), "turnover image published without a strong phase pin");
        const auto revision = budget.snapshot().admissionRevision;
        image.token.reset(); // Caller has actually destroyed backing in native integration.
        const auto s = budget.snapshot();
        require(s.deviceReservedBytes == 100 && s.totalDeviceClaimBytes == 100 &&
                    s.totalHostClaimBytes == 10 && s.ownedNativeImages == 0 &&
                    s.admissionRevision == revision + 1,
                "published destruction restores only exact live grant and independent revision");
    }
    auto retained = NativeImageStorageToken::reserve(phase.lease, 100, local());
    require(retained.token && token(retained).promote(), "last retained image published");
    phase.lease.reset();
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().totalDeviceClaimBytes == 100,
            "weak origin cannot retain phase/count after facade releases it");
    retained.token.reset();
    empty(budget);
}
void turnoverRefusalWatermark() {
    for (const bool externalBeforeDelivery : {false, true}) {
        GpuProcessingPhaseBudget budget;
        budget.setHostCapacity(1000, true);
        budget.setDeviceCapacity(1000, true);
        auto externalPhase = budget.reserve(request());
        auto external = NativeImageStorageToken::reserve(externalPhase.lease, 50, local());
        require(external.token && token(external).promote(), "watermark external image retained");
        externalPhase.lease.reset();
        auto phase = budget.reserve(request());
        auto turnover = NativeImageStorageToken::reserve(phase.lease, 100, local());
        require(turnover.token && token(turnover).promote(), "watermark prior image promoted");
        turnover.token.reset();
        auto prefix = NativeImageStorageToken::reserve(phase.lease, 80, local());
        require(prefix.token && token(prefix).promote(), "watermark own prefix retained");
        const auto atRefusal = budget.snapshot().admissionRevision;
        const auto refused = NativeImageStorageToken::reserve(phase.lease, 21, local());
        require(!refused.token && refused.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                    refused.revisionAfterOwnRelease == atRefusal + 1 &&
                    refused.revisionAfterOwnRelease > phase.lease->revisionAfterOwnRelease(),
                "actual later native refusal captures current revision after past turnover");
        if (externalBeforeDelivery)
            external.token.reset();
        require(token(prefix).releaseFailedAttemptAfterBackingDestroyed(phase.lease),
                "only exact destroyed own prefix folds back quietly");
        prefix.token.reset();
        phase.lease.reset();
        require(budget.snapshot().admissionRevision ==
                    refused.revisionAfterOwnRelease + (externalBeforeDelivery ? 1 : 0),
                "own cleanup cannot self-retry; post-refusal external release remains visible");
        external.token.reset();
        empty(budget);
    }
}
void overflowBound() {
    GpuProcessingPhaseBudget budget;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    budget.setHostCapacity(max, true);
    budget.setDeviceCapacity(max, true);
    GpuProcessingPhaseRequest all;
    all.sourceFree = true;
    all.fixedHostBytes = 1;
    all.deviceWorkingBytes = max;
    auto phase = budget.reserve(all);
    require(phase.lease != nullptr, "maximum native envelope is representable");
    const auto denied = NativeImageStorageToken::reserve(phase.lease, max, nonlocal());
    require(!denied.token && budget.snapshot().ownedNativeImages == 0 &&
                budget.snapshot().totalDeviceClaimBytes == max,
            "supplemental host overflow refused before native half changes");
    auto admitted = NativeImageStorageToken::reserve(phase.lease, max, local());
    require(admitted.token && token(admitted).reconcile(max - 1, local()) &&
                token(admitted).promote(),
            "checked local max handoff/reconcile/publication does not overflow");
    phase.lease.reset();
    require(budget.snapshot().totalDeviceClaimBytes == max - 1,
            "remaining phase and owned allocation remain disjoint");
    admitted.token.reset();
    empty(budget);
}
} // namespace
int main() {
    try {
        atomicAdmissionAndFreshness();
        promotedPrefixFailureAndExternalRelease();
        expiredDeliveryRecovery();
        identityUnknownAndLifetime();
        publishedPeakTurnover();
        turnoverRefusalWatermark();
        overflowBound();
        std::cout << "neutral native image claims/foldback/publication/freshness PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
