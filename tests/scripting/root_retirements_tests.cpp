#include <root_retirements_private.hpp>

#include <bloom/render/packed_rgba8_storage.hpp>
#include <packed_host_retirement_private.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace bloom::scripting::python {
struct PythonRootRetirementsTestAccess final {
    static std::shared_ptr<std::timed_mutex> bootstrapGate(PythonRootRetirements& registry) {
        return registry.impl_->bootstrapGate;
    }
    static void beforePublish(PythonRootRetirements& registry, void (*callback)(void*),
                              void* data) {
        registry.impl_->beforeRegistration = callback;
        registry.impl_->testContext = data;
    }
};
} // namespace bloom::scripting::python
namespace bloom::render::detail {
struct PackedHostRetirementTestAccess final {
    static void beforeFree(PackedHostRetirementOwner& owner, void (*callback)(void*) noexcept,
                           void* data) {
        owner.impl_->state->beforeFree = callback;
        owner.impl_->state->testContext = data;
    }
};
} // namespace bloom::render::detail
namespace {
using namespace bloom;
using namespace bloom::scripting::python;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct RegistryFixture final {
    std::shared_ptr<PythonRootRetirements> registry = PythonRootRetirements::create();
    ~RegistryFixture() {
        if (!registry->shutdownAndWait(2s))
            std::terminate();
    }
};
struct Gate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    ~Gate() { release(); }
    void hold() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void await() {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 2s, [&] { return entered; }), "bounded gate reached");
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
    static void callback(void* data) noexcept { static_cast<Gate*>(data)->hold(); }
};
auto cpuProvider() {
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false;
    return host::GpuExportProvider::create(std::move(options));
}
void ownedTimeoutAndGcHandoff() {
    RegistryFixture fixture;
    Gate gate;
    auto provider = cpuProvider();
    std::weak_ptr<host::GpuExportProvider> weakProvider = provider;
    auto participant = fixture.registry->registerHost(provider, true);
    auto owner = provider->ownedPackedHostRetirementOwner();
    require(owner && owner->prepare(), "owned null-policy provider has explicit live owner");
    provider->refreshProcessingCapacity();
    require(provider->processingPhasePolicy()->snapshot().sampleSerial > 0,
            "owned provider preserves worker process sampling");
    render::detail::PackedHostRetirementTestAccess::beforeFree(*owner, &Gate::callback, &gate);
    render::GpuProcessingPhaseBudget budget(1, owner);
    budget.setHostCapacity(1024, true);
    auto grant = budget.reserve({.fixedHostBytes = 1024, .sourceFree = true, .native = false});
    require(grant.lease != nullptr, "test attempt really admitted");
    auto storage = render::PackedRgba8Storage::create(32, grant.lease, 128);
    require(storage.hasValue() && storage.value()->promote(), "actual rooted published allocation");
    grant.lease.reset();
    const auto revision = budget.snapshot().admissionRevision;
    participant->release(provider); // Exactly the GC handoff before Host drops provider.
    provider.reset();
    require(!weakProvider.expired() && participant->closing(), "closing record retains provider");
    storage.value()->reset();
    gate.await();
    require(!fixture.registry->shutdownAndWait(2ms) && !weakProvider.expired() &&
                fixture.registry->snapshot().participants == 1 &&
                budget.snapshot().ownedPackedHostBytes == 128 &&
                budget.snapshot().admissionRevision == revision && !owner->exitAcknowledged(),
            "false timeout retains provider, scheduler and charged real backing; no unload proof");
    bool rejected = false;
    try {
        static_cast<void>(participant->acquire());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "retained Host record safely rejects scheduler calls after close");
    gate.release();
    require(fixture.registry->shutdownAndWait(2s) && weakProvider.expired() &&
                owner->snapshot().collected && budget.snapshot().totalHostClaimBytes == 0 &&
                budget.snapshot().admissionRevision == revision + 1 &&
                fixture.registry->snapshot().roots == 0,
            "actual free then exit+collection prunes light closing records exactly once");
}
void activeCallAndSharedRoot() {
    RegistryFixture fixture;
    auto provider = cpuProvider();
    auto first = fixture.registry->registerHost(provider, true);
    auto second = fixture.registry->registerHost(provider, true);
    auto owner = provider->ownedPackedHostRetirementOwner();
    require(owner && owner->prepare() && fixture.registry->snapshot().roots == 1 &&
                fixture.registry->snapshot().participants == 2,
            "same root, separate schedulers");
    {
        auto call = first->acquire();
        first->release(provider);
        require(!fixture.registry->collectCompleted() &&
                    fixture.registry->snapshot().activeCalls == 1 && !owner->snapshot().closing,
                "first GC cannot close root or collect active scheduler call");
        static_cast<void>(
            call.scheduler().snapshots()); // Valid even while caller releases GIL/closes.
    }
    static_cast<void>(fixture.registry->collectCompleted());
    require(fixture.registry->snapshot().participants == 1 && !owner->snapshot().closing,
            "finished first participant retires without closing shared root");
    {
        auto call = second->acquire();
        fixture.registry->beginShutdown();
        require(!fixture.registry->shutdownAndWait(1ms) && call.closing() &&
                    fixture.registry->snapshot().activeCalls == 1,
                "close-all cannot collect a live direct Host call");
        static_cast<void>(call.scheduler().snapshots());
    }
    require(fixture.registry->shutdownAndWait(2s) && owner->snapshot().collected,
            "last call ends before scheduler/native/host collection");
    second->release(provider); // User globals can retain and later GC a closed Host.
}
void borrowedRoot() {
    RegistryFixture fixture;
    auto owner = render::PackedHostRetirementOwner::create();
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false;
    options.processingPhasePolicy = std::make_shared<runtime::GpuProcessingPhasePolicy>(32, owner);
    auto provider = host::GpuExportProvider::create(options);
    auto participant = fixture.registry->registerHost(provider, false);
    require(!provider->ownedPackedHostRetirementOwner() && owner->prepare(),
            "injected application root remains borrowed");
    participant->release(provider);
    require(fixture.registry->shutdownAndWait(2s) && !owner->snapshot().closing,
            "borrowed Host only retires its own scheduler, never application owner");
    {
        runtime::TaskScheduler scheduler;
        provider->prepare(scheduler);
        require(provider->shutdownAndWait(2s),
                "application provider admission still usable after Host GC");
    }
    owner->beginShutdown();
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(1ms);
    require(owner->collect(), "owning app performs separate actual host collection");
}
template <class Predicate> void await(Predicate predicate, const char* message) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(1ms);
    require(predicate(), message);
}
void sharedBootstrapGateCancellation() {
    RegistryFixture fixture;
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.loaderPath = "__bloom_test_missing_native_loader__";
    auto cancelled = fixture.registry->createOwnedProvider(options);
    auto surviving = fixture.registry->createOwnedProvider(options);
    auto first = fixture.registry->registerHost(cancelled, true);
    auto second = fixture.registry->registerHost(surviving, true);
    auto gate = PythonRootRetirementsTestAccess::bootstrapGate(*fixture.registry);
    std::unique_lock held(*gate);
    auto firstCall = first->acquire();
    auto secondCall = second->acquire();
    cancelled->prepare(firstCall.scheduler());
    surviving->prepare(secondCall.scheduler());
    auto reached = [](host::GpuExportProvider& provider, runtime::TaskScheduler& scheduler) {
        const auto tasks = scheduler.snapshots();
        return provider.processingPhasePolicy()->snapshot().sampleSerial > 0 &&
               std::any_of(tasks.begin(), tasks.end(), [](const auto& task) {
                   return task.name == "Bootstrap the GPU final-render evaluator" &&
                          task.state == runtime::TaskState::Running;
               });
    };
    await(
        [&] {
            return reached(*cancelled, firstCall.scheduler()) &&
                   reached(*surviving, secondCall.scheduler());
        },
        "both actual bootstrap workers enter the shared held construction gate");
    require(!cancelled->prepared() && !surviving->prepared() &&
                cancelled->processingPhasePolicy()->snapshot().deviceReports == 0 &&
                surviving->processingPhasePolicy()->snapshot().deviceReports == 0,
            "held gate prevents native construction/publication, not worker sampling");
    first->release(cancelled);
    await([&] { return cancelled->retirementComplete(); },
          "waiting bootstrap cancels and terminalizes while another generation keeps gate held");
    require(cancelled->evaluator() == nullptr &&
                cancelled->processingPhasePolicy()->snapshot().deviceReports == 0,
            "cancelled gate waiter publishes no evaluator or device");
    held.unlock();
    await([&] { return surviving->prepared(); }, "surviving bootstrap proceeds after gate unlock");
    require(surviving->evaluator() != nullptr && !surviving->evaluator()->gpuAvailable() &&
                surviving->evaluator()->availabilityDiagnostic().code ==
                    runtime::GpuProcessFrameDiagnosticCode::DeviceUnavailable &&
                !surviving->evaluator()->availabilityDiagnostic().message.empty() &&
                surviving->processingPhasePolicy()->snapshot().deviceReports == 0,
            "real missing-loader construction is explicitly unavailable, never GPU success");
    // Keep checked call leases until the construction/cancellation assertions have finished.
}
void constructorFailureAndCloseRace() {
    {
        RegistryFixture fixture;
        auto provider = cpuProvider();
        PythonRootRetirementsTestAccess::beforePublish(
            *fixture.registry,
            [](void*) { throw std::runtime_error("post-scheduler construction fault"); }, nullptr);
        bool failed = false;
        try {
            static_cast<void>(fixture.registry->registerHost(provider, true));
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(
            failed && fixture.registry->snapshot().participants == 0 &&
                fixture.registry->snapshot().roots == 0,
            "normal construction exception actually joins created scheduler then removes records");
    }
    {
        RegistryFixture fixture;
        Gate gate;
        auto provider = cpuProvider();
        PythonRootRetirementsTestAccess::beforePublish(*fixture.registry, &Gate::callback, &gate);
        auto registration = std::async(std::launch::async, [&] {
            try {
                static_cast<void>(fixture.registry->registerHost(provider, true));
                return false;
            } catch (const std::runtime_error&) {
                return true;
            }
        });
        gate.await();
        require(!fixture.registry->shutdownAndWait(1ms) &&
                    fixture.registry->snapshot().activeCalls == 1,
                "close cannot report complete while registered scheduler constructor is in flight");
        gate.release();
        require(registration.get() && fixture.registry->shutdownAndWait(2s),
                "startup race rejects Host and joins before successful closure");
    }
}
} // namespace
int main() {
    try {
        ownedTimeoutAndGcHandoff();
        activeCallAndSharedRoot();
        borrowedRoot();
        constructorFailureAndCloseRace();
        sharedBootstrapGateCancellation();
        std::cout << "Python root retirement ownership/startup/active-call/timeout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
