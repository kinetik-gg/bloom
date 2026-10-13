#include <bloom/render/native_buffer_storage.hpp>
#include <bloom/render/native_image_storage.hpp>

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace bloom::render;
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
NativeImageStorageIdentity image(bool local = true) { return {7, 2, 1, local}; }
NativeBufferStorageIdentity buffer(bool local = true) { return {7, 2, 1, local}; }
void independentClaims() {
    GpuProcessingPhaseBudget budget(1);
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto phase =
        budget.reserve({.fixedHostBytes = 100, .deviceWorkingBytes = 100, .sourceFree = true});
    require(phase.lease && phase.lease->rootIdentity() == budget.rootIdentity(),
            "stable exact root");
    auto texture =
        NativeImageStorageToken::reservePresentation(budget.rootIdentity(), 400, image());
    auto params =
        NativeBufferStorageToken::reservePresentation(budget.rootIdentity(), 200, buffer(false));
    if (!texture.token || !params.token)
        throw std::runtime_error("independent presentation claims were refused");
    require(
        texture.token && params.token && budget.snapshot().deviceReservedBytes == 100 &&
            budget.snapshot().hostReservedBytes == 100 &&
            budget.snapshot().totalDeviceClaimBytes == 700 &&
            budget.snapshot().totalHostClaimBytes == 300 && budget.snapshot().admittedPhases == 1,
        "presentation adds exact independent paired claims without consuming active phase debit");
    require(texture.token->reconcile(350, image()) && params.token->reconcile(150, buffer(false)),
            "exact provisional presentation identity permits downward reconciliation");
    auto denied =
        NativeBufferStorageToken::reservePresentation(budget.rootIdentity(), 401, buffer());
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Device &&
                budget.snapshot().ownedPresentationBufferBytes == 150 &&
                budget.snapshot().ownedPresentationImageBytes == 350,
            "one-byte shortage leaves both presentation subsets unchanged");
    require(params.token->promote() && !params.token->reconcile(149, buffer(false)) &&
                !params.token->releaseFailedAttemptAfterBackingDestroyed(phase.lease),
            "published presentation cannot reconcile or fold into an unrelated processing phase");
    phase.lease.reset();
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().deviceReservedBytes == 0 &&
                budget.snapshot().totalDeviceClaimBytes == 500 &&
                budget.snapshot().totalHostClaimBytes == 150,
            "presentation lifetime pins no whole processing phase");
    const auto revision = budget.snapshot().admissionRevision;
    auto moved = std::move(*texture.token);
    texture.token.reset();
    params.token.reset();
    moved = {};
    const auto settled = budget.snapshot();
    require(settled.totalDeviceClaimBytes == 0 && settled.totalHostClaimBytes == 0 &&
                settled.ownedPresentationImages == 0 && settled.ownedPresentationBuffers == 0 &&
                settled.admissionRevision == revision + 2,
            "backing owners independently return both claims once and expose real release pulses");
}
void readinessAndBoundaries() {
    GpuProcessingPhaseBudget budget(1);
    const auto now = Clock::now();
    budget.setHostCapacity(1000, true, now);
    budget.setDeviceCapacity(1000, true);
    auto denied = NativeImageStorageToken::reservePresentation(budget.rootIdentity(), 1, image(),
                                                               now + std::chrono::seconds(1));
    require(!denied.token && denied.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                denied.domain == GpuProcessingPhaseDomain::Host && !budget.snapshot().hostReady,
            "new presentation backing cannot consume stale host readiness even on a local heap");
    budget.setHostCapacity(1, true);
    auto hostDenied =
        NativeBufferStorageToken::reservePresentation(budget.rootIdentity(), 2, buffer(false));
    require(!hostDenied.token && hostDenied.domain == GpuProcessingPhaseDomain::Host &&
                budget.snapshot().totalDeviceClaimBytes == 0,
            "nonlocal host refusal commits no native half-claim");
    budget.setHostCapacity(std::numeric_limits<std::uint64_t>::max(), true);
    budget.setDeviceCapacity(std::numeric_limits<std::uint64_t>::max(), true);
    auto maximum = NativeBufferStorageToken::reservePresentation(
        budget.rootIdentity(), std::numeric_limits<std::uint64_t>::max(), buffer(false));
    require(maximum.token.has_value(), "maximum independent presentation claim is representable");
    denied = NativeImageStorageToken::reservePresentation(budget.rootIdentity(), 1, image(false));
    require(!denied.token && denied.status == GpuProcessingPhaseAdmission::BudgetRefused,
            "presentation additions reject overflow before either domain changes");
    maximum.token.reset();
    require(budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().totalHostClaimBytes == 0,
            "integer-boundary destruction conserves both constraints");
}
} // namespace
int main() {
    try {
        independentClaims();
        readinessAndBoundaries();
        std::cout << "PASS: independent rooted presentation storage\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
