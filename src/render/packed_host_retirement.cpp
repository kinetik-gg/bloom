#include "packed_host_retirement_private.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <thread>

namespace bloom::render {
namespace {
void runRetirement(const std::shared_ptr<detail::PackedHostRetirementState>& state) noexcept {
    try {
        if (state->beforeAckRegistration) {
            state->beforeAckRegistration(state->testContext);
        }
        // Some implementations allocate their at-thread-exit registration. Admit no backing
        // until this succeeds; normal shutdown then needs no fallible registration operation.
        state->exitPromise.set_value_at_thread_exit();
        {
            const std::lock_guard lock(state->mutex);
            state->ackRegistered = true;
        }
    } catch (...) {
        {
            const std::lock_guard lock(state->mutex);
            state->ackRegistrationFailed = true;
        }
        state->wake.notify_all();
        return;
    }
    state->wake.notify_all();
    for (;;) {
        detail::PackedHostRetirementNode* node = nullptr;
        {
            std::unique_lock lock(state->mutex);
            state->wake.wait(
                lock, [&] { return state->first || (state->closing && state->allocations == 0); });
            if (!state->first) {
                break;
            }
            node = state->first;
            state->first = node->next;
            if (!state->first) {
                state->last = nullptr;
            }
            --state->queued;
        }
        // Neither backing free nor ledger release holds the queue mutex. UI enqueue cannot
        // contend with the potentially large allocator operation.
        if (state->beforeFree) {
            state->beforeFree(state->testContext);
        }
        node->destroy(node);
        detail::releasePackedHostRegistration(state);
    }
}
} // namespace
PackedHostRetirementOwner::PackedHostRetirementOwner() : impl_(std::make_unique<Impl>()) {}
std::shared_ptr<PackedHostRetirementOwner> PackedHostRetirementOwner::create() {
    auto owner = std::shared_ptr<PackedHostRetirementOwner>(new PackedHostRetirementOwner);
    owner->impl_->state->owner = owner;
    return owner;
}
PackedHostRetirementOwner::~PackedHostRetirementOwner() {
    if (impl_->worker.joinable()) {
        // A frontend must keep this lifecycle owner alive while it reports pending retirement.
        // Collection is explicit even after exit: a std::thread join may have an OS tail and must
        // not be hidden in a destructor that can run on an active UI event loop.
        std::terminate();
    }
}
bool PackedHostRetirementOwner::prepare() noexcept {
    const auto state = impl_->state;
    {
        std::unique_lock lock(state->mutex);
        state->wake.wait(lock, [&] {
            return state->closing || state->worker != detail::PackedHostWorkerState::Starting;
        });
        if (state->closing || state->worker == detail::PackedHostWorkerState::Failed) {
            return false;
        }
        if (state->worker == detail::PackedHostWorkerState::Running) {
            return true;
        }
        state->worker = detail::PackedHostWorkerState::Starting;
    }
    try {
        if (state->beforeStart) {
            state->beforeStart(state->testContext);
        }
        // Thread creation holds no mutex shared with UI beginShutdown/poll/enqueue.
        impl_->worker = std::thread([state] { runRetirement(state); });
        bool accepting = false;
        {
            std::unique_lock lock(state->mutex);
            state->wake.wait(lock,
                             [&] { return state->ackRegistered || state->ackRegistrationFailed; });
            if (state->ackRegistrationFailed) {
                lock.unlock();
                impl_->worker.join(); // Failed startup collection runs on this off-UI caller.
                lock.lock();
                state->worker = detail::PackedHostWorkerState::Failed;
                lock.unlock();
                state->wake.notify_all();
                return false;
            }
            state->worker = detail::PackedHostWorkerState::Running;
            accepting = !state->closing;
        }
        state->wake.notify_all();
        return accepting;
    } catch (...) {
        if (impl_->worker.joinable()) {
            {
                const std::lock_guard lock(state->mutex);
                state->closing = true;
            }
            state->wake.notify_all();
            impl_->worker.join(); // No admitted allocation exists during failed startup.
        }
        {
            const std::lock_guard lock(state->mutex);
            state->worker = detail::PackedHostWorkerState::Failed;
        }
        state->wake.notify_all();
        return false;
    }
}
void PackedHostRetirementOwner::beginShutdown() noexcept {
    {
        const std::lock_guard lock(impl_->state->mutex);
        impl_->state->closing = true;
    }
    impl_->state->wake.notify_all();
}
bool PackedHostRetirementOwner::exitAcknowledged() const noexcept {
    const std::lock_guard lock(impl_->state->mutex);
    const auto worker = impl_->state->worker;
    if (worker == detail::PackedHostWorkerState::Dormant ||
        worker == detail::PackedHostWorkerState::Failed) {
        return true;
    }
    return worker == detail::PackedHostWorkerState::Running &&
           impl_->state->exit.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
bool PackedHostRetirementOwner::collect() noexcept {
    const auto retainOwner = weak_from_this().lock();
    if (!retainOwner) {
        return false;
    }
    if (!exitAcknowledged()) {
        return false;
    }
    {
        const std::lock_guard lock(impl_->state->mutex);
        if (!impl_->state->closing || impl_->state->collecting) {
            return false;
        }
        if (impl_->state->collected) {
            return true;
        }
        impl_->state->collecting = true;
    }
    if (impl_->state->beforeCollect) {
        impl_->state->beforeCollect(impl_->state->testContext);
    }
    if (impl_->worker.joinable()) {
        // OS tail collection must not hold a mutex shared with UI polling/enqueue.
        impl_->worker.join();
    }
    {
        const std::lock_guard lock(impl_->state->mutex);
        impl_->state->collected = true;
        impl_->state->collecting = false;
    }
    return true;
}
PackedHostRetirementSnapshot PackedHostRetirementOwner::snapshot() const noexcept {
    const bool exited = exitAcknowledged();
    const std::lock_guard lock(impl_->state->mutex);
    const auto& state = *impl_->state;
    return {.allocations = state.allocations,
            .queuedAllocations = state.queued,
            .starting = state.worker == detail::PackedHostWorkerState::Starting,
            .started = state.worker == detail::PackedHostWorkerState::Running,
            .closing = state.closing,
            .exitAcknowledged = exited,
            .collected = state.collected};
}
std::shared_ptr<detail::PackedHostRetirementState>
PackedHostRetirementOwner::producerState() const noexcept {
    return impl_->state;
}
bool detail::preparePackedHostRetirement(
    const std::shared_ptr<PackedHostRetirementState>& state) noexcept {
    const auto owner = state ? state->owner.lock() : nullptr;
    return owner && owner->prepare();
}
bool detail::packedHostRetirementAvailable(
    const std::shared_ptr<PackedHostRetirementState>& state) noexcept {
    if (!state) {
        return false;
    }
    const std::lock_guard lock(state->mutex);
    return !state->closing && !state->owner.expired();
}
bool detail::registerPackedHostAllocation(
    const std::shared_ptr<PackedHostRetirementState>& state) noexcept {
    if (!state) {
        return false;
    }
    const std::lock_guard lock(state->mutex);
    if (state->closing || state->worker != PackedHostWorkerState::Running) {
        return false;
    }
    ++state->allocations;
    return true;
}
void detail::releasePackedHostRegistration(
    const std::shared_ptr<PackedHostRetirementState>& state) noexcept {
    {
        const std::lock_guard lock(state->mutex);
        --state->allocations;
    }
    state->wake.notify_all();
}
void detail::enqueuePackedHostRetirement(const std::shared_ptr<PackedHostRetirementState>& state,
                                         PackedHostRetirementNode* const node) noexcept {
    {
        const std::lock_guard lock(state->mutex);
        node->next = nullptr;
        if (state->last) {
            state->last->next = node;
        } else {
            state->first = node;
        }
        state->last = node;
        ++state->queued;
    }
    state->wake.notify_one();
}
} // namespace bloom::render
