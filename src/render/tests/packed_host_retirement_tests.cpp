#include <packed_host_retirement_private.hpp>

#include <bloom/render/packed_rgba8_storage.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

namespace bloom::render::detail {
struct PackedHostRetirementTestAccess final {
    static void beforeStart(PackedHostRetirementOwner& owner, void (*callback)(void*), void* data) {
        owner.impl_->state->beforeStart = callback;
        owner.impl_->state->testContext = data;
    }
    static void beforeFree(PackedHostRetirementOwner& owner, void (*callback)(void*) noexcept,
                           void* data) {
        owner.impl_->state->beforeFree = callback;
        owner.impl_->state->testContext = data;
    }
    static void beforeAck(PackedHostRetirementOwner& owner, void (*callback)(void*) noexcept) {
        owner.impl_->state->beforeAckRegistration = callback;
    }
    static void beforeCollect(PackedHostRetirementOwner& owner, void (*callback)(void*) noexcept,
                              void* data) {
        owner.impl_->state->beforeCollect = callback;
        owner.impl_->state->testContext = data;
    }
};
} // namespace bloom::render::detail
namespace {
using namespace bloom::render;
using namespace std::chrono_literals;
thread_local bool failNextAllocation = false;
std::atomic_size_t actualAllocationFailures{0};
std::atomic<std::size_t> watchedSize{0}, watchedAllocations{0};
std::atomic<void*> watchedBlock{nullptr};
std::atomic_bool observedFree{false}, freeWhileCharged{false};
std::thread::id observedFreeThread;
GpuProcessingPhaseBudget* observedBudget = nullptr;
void require(const bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
template <class Predicate> bool wait(Predicate check) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!check() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(1ms);
    }
    return check();
}
struct OwnerFixture final {
    std::shared_ptr<PackedHostRetirementOwner> owner = PackedHostRetirementOwner::create();
    ~OwnerFixture() {
        owner->beginShutdown();
        if (!wait([&] { return owner->exitAcknowledged(); }) || !owner->collect()) {
            std::terminate();
        }
    }
};
struct Gate final {
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false;
    bool released = false;
    ~Gate() { release(); }
    void enter() {
        std::unique_lock lock(mutex);
        entered = true;
        ready.notify_all();
        ready.wait(lock, [&] { return released; });
    }
    bool hasEntered() {
        const std::lock_guard lock(mutex);
        return entered;
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        ready.notify_all();
    }
    static void hold(void* context) noexcept { static_cast<Gate*>(context)->enter(); }
};
std::shared_ptr<GpuProcessingPhaseLease> grant(GpuProcessingPhaseBudget& budget,
                                               const std::uint64_t bytes) {
    auto result = budget.reserve({.fixedHostBytes = bytes, .sourceFree = true, .native = false});
    require(result.lease != nullptr, "real host phase admitted");
    return std::move(result.lease);
}
void observe(const std::size_t payload, GpuProcessingPhaseBudget* budget) {
    watchedSize = detail::packedHostAllocationMetadataBytes() + payload;
    watchedAllocations = 0;
    watchedBlock = nullptr;
    observedFree = false;
    freeWhileCharged = false;
    observedBudget = budget;
}
void stopObservation() {
    watchedSize = 0;
    observedBudget = nullptr;
    watchedBlock = nullptr;
}
void missingOwnerAndMetadataBound() {
    GpuProcessingPhaseBudget ownerless;
    ownerless.setHostCapacity(1024, true);
    auto phase = grant(ownerless, 1024);
    auto refused = PackedRgba8Storage::create(8, phase, 32);
    require(!refused && refused.error()->code == ImageErrorCode::InvalidState &&
                ownerless.snapshot().ownedPackedHostAllocations == 0,
            "ownerless phase remains usable but rooted packed allocation refuses explicitly");

    OwnerFixture fixture;
    require(fixture.owner->prepare(), "off-UI worker prepares");
    GpuProcessingPhaseBudget budget(1, fixture.owner);
    budget.setHostCapacity(128, true);
    phase = grant(budget, 128);
    observe(128, &budget);
    refused = PackedRgba8Storage::create(32, phase, 128);
    require(!refused && refused.error()->code == ImageErrorCode::PixelStorageBudgetExceeded &&
                watchedAllocations == 0 && budget.snapshot().totalHostClaimBytes == 128 &&
                budget.snapshot().ownedPackedHostMetadataBytes == 0 &&
                fixture.owner->snapshot().allocations == 0,
            "payload fitting alone cannot bypass exact header/node admission BEFORE new");
    stopObservation();
}
void publishedHandoffAndActualFree() {
    OwnerFixture fixture;
    Gate freeGate;
    detail::PackedHostRetirementTestAccess::beforeFree(*fixture.owner, &Gate::hold, &freeGate);
    GpuProcessingPhaseBudget budget(1, fixture.owner);
    budget.setHostCapacity(1024, true);
    auto phase = grant(budget, 1024);
    const auto ownWatermark = phase->revisionAfterOwnRelease();
    require(fixture.owner->prepare(), "worker prepared outside dropping thread");
    observe(128, &budget);
    auto storage = PackedRgba8Storage::create(32, phase, 128);
    require(storage.hasValue() && watchedAllocations == 1 && storage.value()->promote(),
            "real bounded payload and exact metadata allocation promote");
    phase.reset();
    const auto metadata = detail::packedHostAllocationMetadataBytes();
    require(budget.snapshot().hostReservedBytes == 0 &&
                budget.snapshot().ownedPackedHostMetadataBytes == metadata &&
                budget.snapshot().totalHostClaimBytes == 128 + metadata &&
                budget.snapshot().admissionRevision == ownWatermark,
            "published payload plus exact metadata survive one phase release");
    const auto droppingThread = std::this_thread::get_id();
    failNextAllocation = true;
    storage.value()->reset();
    require(failNextAllocation, "last-alias enqueue makes no allocator request");
    failNextAllocation = false;
    require(wait([&] { return freeGate.hasEntered(); }) && !observedFree &&
                budget.snapshot().ownedPackedHostBytes == 128 &&
                budget.snapshot().ownedPackedHostMetadataBytes == metadata &&
                budget.snapshot().admissionRevision == ownWatermark,
            "held worker keeps actual queued backing and metadata charged without revision");
    auto blocked =
        budget.reserve({.fixedHostBytes = 1024 - 128, .sourceFree = true, .native = false});
    require(!blocked.lease && blocked.domain == GpuProcessingPhaseDomain::Host,
            "new admission includes queued payload AND metadata");
    fixture.owner->beginShutdown();
    require(!fixture.owner->exitAcknowledged() && !fixture.owner->collect(),
            "shutdown and timeout/poll are not proof while actual free is held");
    freeGate.release();
    require(wait([&] { return fixture.owner->exitAcknowledged(); }) && fixture.owner->collect() &&
                observedFree && freeWhileCharged && observedFreeThread != droppingThread &&
                budget.snapshot().ownedPackedHostBytes == 0 &&
                budget.snapshot().ownedPackedHostMetadataBytes == 0 &&
                budget.snapshot().totalHostClaimBytes == 0 &&
                budget.snapshot().admissionRevision == ownWatermark + 1,
            "actual off-thread backing free precedes one atomic payload/metadata release and exit");
    stopObservation();
}
void provisionalFailureWatermark() {
    for (const bool externalFirst : {false, true}) {
        OwnerFixture fixture;
        GpuProcessingPhaseBudget budget(2, fixture.owner);
        budget.setHostCapacity(2048, true);
        auto phase = grant(budget, 1024);
        auto external = grant(budget, 1024);
        const auto own = phase->revisionAfterOwnRelease();
        auto storage = PackedRgba8Storage::create(32, phase, 128);
        require(storage.hasValue(), "capacity failure occurs after actual admitted storage");
        const auto refused =
            budget.reserve({.fixedHostBytes = 1, .sourceFree = true, .native = false});
        require(!refused.lease && refused.domain == GpuProcessingPhaseDomain::RequestCount,
                "real typed capacity refusal follows provisional allocation");
        phase.reset();
        if (externalFirst) {
            external.reset();
        }
        storage.value()->reset();
        require(budget.snapshot().admissionRevision == own + (externalFirst ? 1U : 0U) &&
                    budget.snapshot().ownedPackedHostMetadataBytes == 0 &&
                    budget.snapshot().ownedPackedHostBytes == 0 &&
                    fixture.owner->snapshot().allocations == 0,
                "attempt-worker free folds all metadata/payload before exact own R+1");
    }
}
void noFixedAllocationCapAndRootLifetime() {
    OwnerFixture fixture;
    constexpr std::size_t count = 1500;
    const auto charge = count * (4 + detail::packedHostAllocationMetadataBytes());
    auto budget = std::make_unique<GpuProcessingPhaseBudget>(1, fixture.owner);
    budget->setHostCapacity(charge, true);
    auto phase = grant(*budget, charge);
    std::vector<PackedRgba8Storage> pins;
    pins.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto storage = PackedRgba8Storage::create(1, phase, 4);
        require(storage.hasValue() && storage.value()->promote(),
                "tiny retained frames are bounded by admitted bytes, no arbitrary live cap");
        pins.push_back(std::move(*storage.value()));
    }
    phase.reset();
    require(budget->snapshot().totalHostClaimBytes == charge &&
                budget->snapshot().ownedPackedHostAllocations == count &&
                fixture.owner->snapshot().allocations == count,
            "1500 live allocations share one phase and conserve actual payload/node bytes");
    budget.reset();
    fixture.owner->beginShutdown();
    require(!fixture.owner->exitAcknowledged(),
            "facade shutdown cannot drop pinned neutral root or pretend worker exit");
    pins.clear();
    require(wait([&] { return fixture.owner->exitAcknowledged(); }) && fixture.owner->collect() &&
                fixture.owner->snapshot().allocations == 0,
            "external last aliases retire after root facade destruction with portable exit proof");
}
void startupFailureAndShutdownRace() {
    {
        OwnerFixture fixture;
        detail::PackedHostRetirementTestAccess::beforeStart(
            *fixture.owner, [](void*) { throw std::bad_alloc{}; }, nullptr);
        GpuProcessingPhaseBudget budget(1, fixture.owner);
        budget.setHostCapacity(1024, true);
        auto phase = grant(budget, 1024);
        auto storage = PackedRgba8Storage::create(32, phase, 128);
        require(!storage && storage.error()->code == ImageErrorCode::AllocationFailure &&
                    fixture.owner->snapshot().allocations == 0 &&
                    !fixture.owner->snapshot().started &&
                    budget.snapshot().hostReservedBytes == 1024 &&
                    budget.snapshot().ownedPackedHostMetadataBytes == 0,
                "worker startup failure leaves no worker, registration or consumed debit");
    }
    OwnerFixture fixture;
    Gate starting;
    detail::PackedHostRetirementTestAccess::beforeStart(*fixture.owner, &Gate::hold, &starting);
    auto prepared = std::async(std::launch::async, [&] { return fixture.owner->prepare(); });
    const auto releaseStarting = [](Gate* gate) { gate->release(); };
    const std::unique_ptr<Gate, decltype(releaseStarting)> releaseBeforeFutureDestruction(
        &starting, releaseStarting);
    require(wait([&] { return starting.hasEntered(); }), "actual startup enters held boundary");
    fixture.owner->beginShutdown();
    require(fixture.owner->snapshot().starting && !fixture.owner->exitAcknowledged() &&
                !fixture.owner->collect(),
            "close while Starting neither blocks nor treats unpublished thread as Dormant");
    starting.release();
    require(!prepared.get() && wait([&] { return fixture.owner->exitAcknowledged(); }) &&
                fixture.owner->collect() && fixture.owner->snapshot().allocations == 0,
            "startup/shutdown race retains closing state and collects only actual exited thread");
}
void actualAckAllocationFailureAndRecovery() {
    {
        OwnerFixture fixture;
        detail::PackedHostRetirementTestAccess::beforeAck(
            *fixture.owner, [](void*) noexcept { failNextAllocation = true; });
        GpuProcessingPhaseBudget budget(1, fixture.owner);
        budget.setHostCapacity(1024, true);
        auto phase = grant(budget, 1024);
        const auto own = phase->revisionAfterOwnRelease();
        const auto before = actualAllocationFailures.load();
        auto storage = PackedRgba8Storage::create(32, phase, 128);
        require(!storage && storage.error()->code == ImageErrorCode::AllocationFailure &&
                    actualAllocationFailures == before + 1 && fixture.owner->exitAcknowledged() &&
                    !fixture.owner->snapshot().started &&
                    fixture.owner->snapshot().allocations == 0 &&
                    budget.snapshot().hostReservedBytes == 1024 &&
                    budget.snapshot().ownedPackedHostMetadataBytes == 0,
                "actual child TLS allocator failure at exit registration joins failed startup");
        phase.reset();
        require(budget.snapshot().totalHostClaimBytes == 0 &&
                    budget.snapshot().admissionRevision == own,
                "failed actual exit registration conserves exact own cleanup watermark");
    }
    OwnerFixture recovered;
    GpuProcessingPhaseBudget budget(1, recovered.owner);
    budget.setHostCapacity(1024, true);
    auto phase = grant(budget, 1024);
    auto storage = PackedRgba8Storage::create(32, phase, 128);
    require(storage.hasValue() && storage.value()->promote(),
            "fresh owner recovers real packed allocation after failed exit registration");
    phase.reset();
    storage.value()->reset();
    recovered.owner->beginShutdown();
    require(wait([&] { return recovered.owner->exitAcknowledged(); }) &&
                recovered.owner->collect() && budget.snapshot().totalHostClaimBytes == 0,
            "recovered owner performs real worker free and shutdown exit acknowledgment");
}
void collectionDoesNotLockUiPolls() {
    OwnerFixture fixture;
    require(fixture.owner->prepare(), "collection fixture starts actual worker");
    fixture.owner->beginShutdown();
    require(wait([&] { return fixture.owner->exitAcknowledged(); }),
            "collection fixture has actual at-thread-exit proof");
    Gate collecting;
    detail::PackedHostRetirementTestAccess::beforeCollect(*fixture.owner, &Gate::hold, &collecting);
    auto collect = std::async(std::launch::async, [&] { return fixture.owner->collect(); });
    const auto release = [](Gate* gate) { gate->release(); };
    const std::unique_ptr<Gate, decltype(release)> releaseBeforeFutureDestruction(&collecting,
                                                                                  release);
    require(wait([&] { return collecting.hasEntered(); }), "off-thread collection boundary held");
    auto uiPoll = std::async(std::launch::async, [&] {
        return fixture.owner->snapshot().exitAcknowledged && fixture.owner->exitAcknowledged();
    });
    const bool pollReady = uiPoll.wait_for(50ms) == std::future_status::ready;
    auto secondCollector = std::async(std::launch::async, [&] { return fixture.owner->collect(); });
    const bool secondReady = secondCollector.wait_for(50ms) == std::future_status::ready;
    collecting.release();
    require(pollReady && secondReady && uiPoll.get() && !secondCollector.get() && collect.get(),
            "UI snapshot/exit polling does not share collection wait and collectors serialize");
}
} // namespace
void* operator new(const std::size_t bytes) {
    if (std::exchange(failNextAllocation, false)) {
        ++actualAllocationFailures;
        throw std::bad_alloc{};
    }
    void* block = std::malloc(bytes == 0 ? 1 : bytes);
    if (!block) {
        throw std::bad_alloc{};
    }
    if (bytes != 0 && bytes == watchedSize) {
        ++watchedAllocations;
        watchedBlock = block;
    }
    return block;
}
void operator delete(void* block) noexcept {
    const bool watched = block && block == watchedBlock;
    std::free(block);
    if (watched && observedBudget) {
        const auto state = observedBudget->snapshot();
        freeWhileCharged =
            state.ownedPackedHostBytes != 0 && state.ownedPackedHostMetadataBytes != 0;
        observedFreeThread = std::this_thread::get_id();
        observedFree = true;
    }
}
void operator delete(void* block, std::size_t) noexcept { ::operator delete(block); }
int main() {
    try {
        missingOwnerAndMetadataBound();
        publishedHandoffAndActualFree();
        provisionalFailureWatermark();
        noFixedAllocationCapAndRootLifetime();
        startupFailureAndShutdownRace();
        actualAckAllocationFailureAndRecovery();
        collectionDoesNotLockUiPolls();
        std::cout << "packed-host retirement actual free/lifecycle/metadata/no-frame-cap PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
