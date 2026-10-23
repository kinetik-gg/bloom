#include "root_retirements_private.hpp"

#include <bloom/runtime/packed_host_retirement.hpp>

#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::scripting::python {
PythonHostRetirement::PythonHostRetirement() : impl_(std::make_unique<Impl>()) {}
PythonHostRetirement::~PythonHostRetirement() {
    // Registered schedulers must be collected explicitly outside the GIL before module unload.
    // Unregistered construction failures are destroyed by registerHost on that same off-GIL lane.
}
PythonHostRetirement::Call::Call(std::shared_ptr<PythonHostRetirement> participant,
                                 runtime::TaskScheduler* scheduler) noexcept
    : participant_(std::move(participant)), scheduler_(scheduler) {}
PythonHostRetirement::Call::Call(Call&& other) noexcept
    : participant_(std::move(other.participant_)),
      scheduler_(std::exchange(other.scheduler_, nullptr)) {}
PythonHostRetirement::Call::~Call() {
    if (participant_)
        participant_->finishCall();
}
runtime::TaskScheduler& PythonHostRetirement::Call::scheduler() const noexcept {
    return *scheduler_;
}
bool PythonHostRetirement::Call::closing() const noexcept { return participant_->closing(); }
PythonHostRetirement::Call PythonHostRetirement::acquire() {
    auto call = tryAcquire();
    if (!call)
        throw std::runtime_error("Bloom host rendering is shutting down");
    return std::move(*call);
}
std::optional<PythonHostRetirement::Call> PythonHostRetirement::tryAcquire() noexcept {
    auto self = weak_from_this().lock();
    if (!self)
        return std::nullopt;
    const std::lock_guard lock(impl_->mutex);
    if (impl_->closing.load() || !impl_->scheduler)
        return std::nullopt;
    ++impl_->activeCalls;
    return Call(std::move(self), impl_->scheduler.get());
}
void PythonHostRetirement::finishCall() noexcept {
    const std::lock_guard lock(impl_->mutex);
    --impl_->activeCalls;
}
bool PythonHostRetirement::closing() const noexcept { return impl_->closing.load(); }
void PythonHostRetirement::release(
    const std::shared_ptr<host::GpuExportProvider>& provider) noexcept {
    const auto self = weak_from_this().lock();
    if (const auto registry = impl_->registry.lock(); registry && self)
        registry->release(self, provider);
}
PythonRootRetirements::PythonRootRetirements() : impl_(std::make_unique<Impl>()) {}
PythonRootRetirements::~PythonRootRetirements() {
    // A live registry is retained by the module/interpreter and Hosts until explicit collection.
    // Module destruction must never silently join a worker while Python finalizes under the GIL.
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->participants.empty() || !impl_->roots.empty())
        std::terminate();
}
std::shared_ptr<PythonRootRetirements> PythonRootRetirements::create() {
    return std::shared_ptr<PythonRootRetirements>(new PythonRootRetirements());
}
std::shared_ptr<host::GpuExportProvider>
PythonRootRetirements::createOwnedProvider(runtime::GpuProcessFrameEvaluatorOptions options) {
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closing)
            throw std::runtime_error("Bloom Python rendering is shutting down");
    }
    return host::GpuExportProvider::create(std::move(options), impl_->bootstrapGate);
}
std::shared_ptr<host::GpuExportProvider> PythonRootRetirements::sharedProvider() {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->closing)
        throw std::runtime_error("Bloom Python rendering is shutting down");
    if (!impl_->sharedProvider)
        impl_->sharedProvider = host::GpuExportProvider::create({}, impl_->bootstrapGate);
    return impl_->sharedProvider;
}
std::shared_ptr<PythonHostRetirement>
PythonRootRetirements::registerHost(const std::shared_ptr<host::GpuExportProvider>& provider,
                                    const bool ownsProvider) {
    if (!provider)
        throw std::invalid_argument("Python rendering requires an explicit provider");
    // Host creation is already off-GIL/nonUI. Reap proven earlier GC records on this existing
    // lane instead of retaining finished per-Host worker pools until interpreter shutdown.
    static_cast<void>(collectCompleted());
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closing)
            throw std::runtime_error("Bloom Python rendering is shutting down");
    }
    auto participant = std::shared_ptr<PythonHostRetirement>(new PythonHostRetirement());
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->closing)
            throw std::runtime_error("Bloom Python rendering is shutting down");
        std::shared_ptr<PythonProviderRetirement> root;
        for (const auto& existing : impl_->roots)
            if (existing->provider.lock() == provider) {
                root = existing;
                break;
            }
        if (root && root->closing)
            throw std::runtime_error("Bloom shared rendering root is shutting down");
        if (root && root->ownsProvider != ownsProvider)
            throw std::invalid_argument("Bloom shared rendering root ownership must agree");
        if (!root) {
            root = std::make_shared<PythonProviderRetirement>();
            root->provider = provider;
            root->ownsProvider = ownsProvider;
            root->processLifetime = provider == impl_->sharedProvider;
            if (ownsProvider)
                root->hostOwner = provider->ownedPackedHostRetirementOwner();
            impl_->roots.push_back(root);
        }
        participant->impl_->root = root;
        participant->impl_->registry = shared_from_this();
        participant->impl_->activeCalls = 1; // Startup registered BEFORE any worker construction.
        try {
            impl_->participants.push_back(participant);
        } catch (...) {
            if (root->memberships == 0)
                impl_->roots.remove(root);
            throw;
        }
        ++root->memberships;
    }
    std::unique_ptr<runtime::TaskScheduler> created;
    try {
        created = std::make_unique<runtime::TaskScheduler>();
        if (impl_->beforeRegistration)
            impl_->beforeRegistration(impl_->testContext);
        const std::lock_guard lock(participant->impl_->mutex);
        participant->impl_->scheduler = std::move(created);
        participant->impl_->starting = false;
        --participant->impl_->activeCalls;
        if (participant->impl_->closing.load())
            participant->impl_->scheduler->beginShutdown();
    } catch (...) {
        created.reset(); // Failed constructed workers actually join off-GIL, outside all locks.
        release(participant, provider);
        {
            const std::lock_guard lock(participant->impl_->mutex);
            participant->impl_->starting = false;
            --participant->impl_->activeCalls;
            participant->impl_->collected = true; // No scheduler remains after actual cleanup.
        }
        static_cast<void>(collectCompleted());
        throw;
    }
    if (participant->closing()) {
        release(participant, provider);
        static_cast<void>(collectCompleted());
        throw std::runtime_error("Bloom Python rendering closed during host construction");
    }
    return participant;
}

void PythonRootRetirements::release(
    const std::shared_ptr<PythonHostRetirement>& participant,
    const std::shared_ptr<host::GpuExportProvider>& provider) noexcept {
    std::shared_ptr<host::GpuExportProvider> ownedClose;
    {
        const std::lock_guard registryLock(impl_->mutex);
        const std::lock_guard participantLock(participant->impl_->mutex);
        auto& part = *participant->impl_;
        if (part.released)
            return;
        part.released = true;
        part.closing.store(true);
        auto& root = *part.root;
        if (root.ownsProvider && !root.collected)
            root.closingProvider = provider; // Existing slot, no allocation before Host drops it.
        if (root.memberships > 0)
            --root.memberships;
        if (root.memberships == 0 && !root.processLifetime) {
            root.closing = true;
            if (root.ownsProvider)
                ownedClose = root.closingProvider;
        }
        if (part.scheduler)
            part.scheduler->beginShutdown();
    }
    if (ownedClose)
        ownedClose->beginShutdown();
}
void PythonRootRetirements::beginShutdown() noexcept {
    const std::lock_guard registryLock(impl_->mutex);
    impl_->closing = true;
    for (const auto& root : impl_->roots) {
        root->closing = true;
        if (root->ownsProvider) {
            if (!root->closingProvider)
                root->closingProvider = root->provider.lock();
            if (root->closingProvider)
                root->closingProvider->beginShutdown();
            else if (root->hostOwner)
                root->hostOwner->beginShutdown();
        }
    }
    for (const auto& participant : impl_->participants) {
        const std::lock_guard participantLock(participant->impl_->mutex);
        participant->impl_->closing.store(true);
        if (participant->impl_->scheduler)
            participant->impl_->scheduler->beginShutdown();
    }
    // The shared root's closingProvider (taken above) now carries the provider to retirement.
    impl_->sharedProvider.reset();
}
bool PythonRootRetirements::collectCompleted() noexcept {
    // Snapshotting is off-GIL. If metadata allocation fails, retain the original records and
    // return incomplete; no lifecycle owner or charge is abandoned to make shutdown appear done.
    std::vector<std::shared_ptr<PythonHostRetirement>> participants;
    std::vector<std::shared_ptr<PythonProviderRetirement>> roots;
    try {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->collecting)
            return false;
        participants.assign(impl_->participants.begin(), impl_->participants.end());
        roots.assign(impl_->roots.begin(), impl_->roots.end());
        impl_->collecting = true;
    } catch (...) {
        return false;
    }
    for (const auto& participant : participants) {
        // An owned root's incomplete native/host proof retains its scheduler too. A partial GC
        // of a shared root retires only this participant, never the still-live shared provider.
        std::shared_ptr<host::GpuExportProvider> provider;
        bool waitForOwnedRoot = false;
        {
            const std::lock_guard lock(impl_->mutex);
            const auto& root = *participant->impl_->root;
            waitForOwnedRoot = root.ownsProvider && root.closing;
            provider = root.closingProvider;
        }
        if (waitForOwnedRoot && ((provider && !provider->retirementComplete()) ||
                                 (participant->impl_->root->hostOwner &&
                                  !participant->impl_->root->hostOwner->exitAcknowledged())))
            continue;
        std::unique_ptr<runtime::TaskScheduler> scheduler;
        {
            const std::lock_guard lock(participant->impl_->mutex);
            auto& part = *participant->impl_;
            if (!part.closing.load() || part.starting || part.activeCalls != 0 || part.collected ||
                !part.scheduler || !part.scheduler->isQuiescent())
                continue;
            part.collecting = true;
            scheduler = std::move(part.scheduler);
        }
        scheduler.reset(); // Actual worker joins outside registry/participant locks and GIL.
        {
            const std::lock_guard lock(participant->impl_->mutex);
            participant->impl_->collecting = false;
            participant->impl_->collected = true;
        }
    }
    for (const auto& root : roots) {
        bool allCallsAndSchedulersDone = true;
        std::shared_ptr<host::GpuExportProvider> provider;
        bool closing = false;
        {
            const std::lock_guard lock(impl_->mutex);
            closing = root->closing;
            provider = root->closingProvider;
        }
        if (!closing)
            continue;
        for (const auto& participant : participants)
            if (participant->impl_->root == root) {
                const std::lock_guard lock(participant->impl_->mutex);
                if (!participant->impl_->collected)
                    allCallsAndSchedulersDone = false;
            }
        if (!allCallsAndSchedulersDone)
            continue;
        if (root->ownsProvider) {
            if (provider && !provider->shutdownAndWait(std::chrono::milliseconds(0)))
                continue;
            if (!provider && root->hostOwner &&
                (!root->hostOwner->exitAcknowledged() || !root->hostOwner->collect()))
                continue;
        }
        {
            const std::lock_guard lock(impl_->mutex);
            root->collected = true;
            root->closingProvider.reset(); // provider local retains final release outside lock.
        }
    }
    bool complete = false;
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->participants.remove_if([](const auto& participant) {
            const std::lock_guard participantLock(participant->impl_->mutex);
            return participant->impl_->collected;
        });
        impl_->roots.remove_if([](const auto& root) { return root->collected; });
        impl_->collecting = false;
        complete = impl_->participants.empty() && impl_->roots.empty();
    }
    return complete;
}
bool PythonRootRetirements::shutdownAndWait(const std::chrono::milliseconds timeout) noexcept {
    beginShutdown();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (collectCompleted())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
PythonRootRetirementSnapshot PythonRootRetirements::snapshot() const noexcept {
    const std::lock_guard registryLock(impl_->mutex);
    PythonRootRetirementSnapshot result{.roots = impl_->roots.size(),
                                        .participants = impl_->participants.size(),
                                        .closing = impl_->closing};
    for (const auto& participant : impl_->participants) {
        const std::lock_guard participantLock(participant->impl_->mutex);
        result.activeCalls += participant->impl_->activeCalls;
    }
    return result;
}
} // namespace bloom::scripting::python
