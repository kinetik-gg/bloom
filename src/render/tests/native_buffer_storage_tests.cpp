#include <bloom/render/native_buffer_storage.hpp>
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
NativeBufferStorageToken& token(NativeBufferStorageGrant& grant) {
    if (!grant.token)
        throw std::runtime_error("expected an admitted buffer token");
    return *grant.token;
}
NativeBufferStorageIdentity local() {
    return {.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
}
NativeBufferStorageIdentity nonlocal() {
    return {.allocatorGeneration = 7, .memoryType = 3, .heap = 0, .deviceLocal = false};
}
GpuProcessingPhaseRequest request(std::uint64_t host = 80, std::uint64_t device = 100) {
    return {.fixedHostBytes = host, .deviceWorkingBytes = device, .sourceFree = true};
}
void empty(const GpuProcessingPhaseBudget& budget) {
    const auto s = budget.snapshot();
    require(s.hostReservedBytes == 0 && s.deviceReservedBytes == 0 && s.totalHostClaimBytes == 0 &&
                s.totalDeviceClaimBytes == 0 && s.ownedNativeBufferBytes == 0 &&
                s.ownedNonlocalNativeBufferHostBytes == 0 && s.ownedNativeBuffers == 0 &&
                s.admittedPhases == 0 && s.unprovenPhases == 0,
            "buffer and phase claims return to zero");
}
void pairedAdmissionAndReconcile() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    require(phase.lease != nullptr, "paired phase admitted");
    auto device = NativeBufferStorageToken::reserve(phase.lease, 40, local());
    auto host = NativeBufferStorageToken::reserve(phase.lease, 60, nonlocal());
    require(device.token && host.token, "both actual heap domains transfer existing debit");
    const auto s = budget.snapshot();
    require(s.ownedNativeBufferBytes == 100 && s.ownedNativeBuffers == 2 &&
                s.ownedNonlocalNativeBufferHostBytes == 60 && s.hostReservedBytes == 20 &&
                s.deviceReservedBytes == 0 && s.totalHostClaimBytes == 80 &&
                s.totalDeviceClaimBytes == 100 && s.ownedNativeImages == 0,
            "paired handoff preserves both totals and separate image counts");
    auto denied = NativeBufferStorageToken::reserve(phase.lease, 1, nonlocal());
    require(!denied.token && denied.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                denied.domain == GpuProcessingPhaseDomain::Device &&
                budget.snapshot().hostReservedBytes == s.hostReservedBytes,
            "one-byte native refusal never admits the host half");
    for (unsigned field = 0; field != 4; ++field) {
        auto wrong = nonlocal();
        if (field == 0)
            ++wrong.allocatorGeneration;
        else if (field == 1)
            ++wrong.memoryType;
        else if (field == 2)
            ++wrong.heap;
        else
            wrong.deviceLocal = true;
        require(!token(host).reconcile(50, wrong) && token(host).bytes() == 60,
                "each wrong identity field independently rejects unchanged");
    }
    require(!token(host).reconcile(61, nonlocal()) && !token(host).reconcile(0, nonlocal()) &&
                token(host).reconcile(50, nonlocal()),
            "reconciliation only shrinks a positive exact allocation");
    const auto reconciled = budget.snapshot();
    require(reconciled.deviceReservedBytes == 10 && reconciled.hostReservedBytes == 30 &&
                reconciled.ownedNativeBufferBytes == 90 &&
                reconciled.ownedNonlocalNativeBufferHostBytes == 50 &&
                reconciled.totalDeviceClaimBytes == 100 && reconciled.totalHostClaimBytes == 80 &&
                phase.lease->hostBytes() == 80 && phase.lease->deviceBytes() == 100,
            "downward reconciliation restores both remaining debits, never immutable limits");
    auto moved = std::move(token(host));
    host.token.reset();
    require(budget.snapshot().ownedNativeBuffers == 2,
            "move and moved-from destruction preserve one token per suballocation");
    const auto watermark = denied.revisionAfterOwnRelease;
    // Neutral controls have no native backing; real backing-before-reset proof belongs to adoption.
    moved = {};
    device.token.reset();
    require(budget.snapshot().admissionRevision + 1 == watermark &&
                budget.snapshot().hostReservedBytes == 80 &&
                budget.snapshot().deviceReservedBytes == 100,
            "provisional foldback is quiet and restores the whole admitted envelope");
    phase.lease.reset();
    require(budget.snapshot().admissionRevision == watermark,
            "only actual final phase release advances own R+1");
    empty(budget);

    phase = budget.reserve(request(5, 10));
    require(phase.lease != nullptr, "small host grant admitted");
    const auto before = budget.snapshot();
    denied = NativeBufferStorageToken::reserve(phase.lease, 6, nonlocal());
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Host &&
                budget.snapshot().deviceReservedBytes == before.deviceReservedBytes &&
                budget.snapshot().ownedNativeBufferBytes == 0,
            "one-byte host refusal never consumes native debit or adds supplemental capacity");
    phase.lease.reset();
    empty(budget);
}
void grandfatheredConversionAndLifetime() {
    GpuProcessingPhaseBudget budget;
    const auto now = Clock::now();
    budget.setHostCapacity(1000, true, now + std::chrono::seconds(10));
    budget.setDeviceCapacity(1000, true);
    auto phase = budget.reserve(request());
    require(phase.lease != nullptr, "original grant admitted from a fresh delivery");
    budget.setHostCapacity(0, false, now - std::chrono::seconds(1));
    budget.setDeviceCapacity(0, false);
    auto admitted = NativeBufferStorageToken::reserve(phase.lease, 40, nonlocal());
    require(admitted.token && budget.snapshot().totalHostClaimBytes == 80 &&
                budget.snapshot().totalDeviceClaimBytes == 100,
            "already reserved paired conversion survives staleness and quota shrink");
    auto additive = NativeImageStorageToken::reserve(
        phase.lease, 1,
        {.allocatorGeneration = 7, .memoryType = 3, .heap = 0, .deviceLocal = false},
        now + std::chrono::seconds(11));
    require(!additive.token && additive.status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "a genuinely additive image host claim still requires fresh readiness");
    struct Holder final {
        NativeBufferStorageToken token;
    };
    auto holder = std::make_shared<Holder>();
    holder->token = std::move(token(admitted));
    admitted.token.reset();
    std::vector<std::shared_ptr<Holder>> aliases(256, holder);
    auto weak = std::weak_ptr<GpuProcessingPhaseLease>(phase.lease);
    phase.lease->markUnproven();
    require(!NativeBufferStorageToken::reserve(phase.lease, 1, local()).token,
            "marked-unproven authority cannot create another buffer");
    phase.lease.reset();
    holder.reset();
    require(!weak.expired() && budget.snapshot().ownedNativeBuffers == 1 &&
                budget.snapshot().ownedNativeBufferBytes == 40 &&
                budget.snapshot().ownedNonlocalNativeBufferHostBytes == 40 &&
                budget.snapshot().unprovenPhases == 1,
            "aliases retain one transient token and exact sticky attempt phase");
    std::thread release([held = std::move(aliases)]() mutable { held.clear(); });
    release.join();
    require(weak.expired(), "last token drops the phase only after leaving the neutral mutex");
    empty(budget);
}
void maximumConservation() {
    GpuProcessingPhaseBudget budget;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    budget.setHostCapacity(maximum, true);
    budget.setDeviceCapacity(maximum, true);
    auto phase = budget.reserve(request(maximum, maximum));
    require(phase.lease != nullptr, "maximum finite paired grant is representable");
    auto admitted = NativeBufferStorageToken::reserve(phase.lease, maximum, nonlocal());
    require(admitted.token && budget.snapshot().totalHostClaimBytes == maximum &&
                budget.snapshot().totalDeviceClaimBytes == maximum,
            "transfer additions cannot overflow or double total claims");
    require(!NativeBufferStorageToken::reserve(phase.lease, 1, nonlocal()).token,
            "exhausted remaining debit rejects before addition");
    admitted.token.reset();
    phase.lease.reset();
    empty(budget);
}
void nonemptyCrossRootMove() {
    GpuProcessingPhaseBudget first;
    GpuProcessingPhaseBudget second;
    for (auto* budget : {&first, &second}) {
        budget->setHostCapacity(1000, true);
        budget->setDeviceCapacity(1000, true);
    }
    auto oldPhase = first.reserve(request(30, 30));
    auto newPhase = second.reserve(request(40, 50));
    require(oldPhase.lease && newPhase.lease, "two distinct roots admit independent phases");
    auto old = NativeBufferStorageToken::reserve(oldPhase.lease, 20, nonlocal());
    auto incoming = NativeBufferStorageToken::reserve(newPhase.lease, 30, local());
    const auto oldRevision = first.snapshot().admissionRevision;
    const auto newRevision = second.snapshot().admissionRevision;
    // No native backing exists in this neutral control; real caller must destroy old backing first.
    token(old) = std::move(token(incoming));
    incoming.token.reset();
    require(first.snapshot().ownedNativeBuffers == 0 && first.snapshot().hostReservedBytes == 30 &&
                first.snapshot().deviceReservedBytes == 30 &&
                first.snapshot().totalHostClaimBytes == 30 &&
                first.snapshot().totalDeviceClaimBytes == 30 &&
                first.snapshot().admissionRevision == oldRevision,
            "nonempty replacement quietly folds the previous exact-root debit once");
    require(second.snapshot().ownedNativeBuffers == 1 &&
                second.snapshot().ownedNativeBufferBytes == 30 &&
                second.snapshot().totalDeviceClaimBytes == 50 &&
                second.snapshot().totalHostClaimBytes == 40 &&
                second.snapshot().admissionRevision == newRevision,
            "moved new root remains charged independently of moved-from destruction");
    oldPhase.lease.reset();
    empty(first);
    require(first.snapshot().admissionRevision == oldRevision + 1,
            "replaced origin retains only its own final phase release");
    auto weak = std::weak_ptr<GpuProcessingPhaseLease>(newPhase.lease);
    newPhase.lease.reset();
    require(!weak.expired() && second.snapshot().ownedNativeBufferBytes == 30,
            "nonempty move retains the incoming exact attempt until actual token disposal");
    old.token.reset();
    require(weak.expired() && second.snapshot().admissionRevision == newRevision + 1,
            "incoming final disposal releases its own phase once outside the mutex");
    empty(second);
}
} // namespace

int main() {
    try {
        pairedAdmissionAndReconcile();
        grandfatheredConversionAndLifetime();
        maximumConservation();
        nonemptyCrossRootMove();
        std::cout << "PASS: neutral transient native buffer storage\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
