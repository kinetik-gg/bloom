// The live GUI session for off-thread clients (ADR 0034, section 4): one mirror, one bounded commit
// marshal, one generation counter and one revocation rule, shared by the Python console and MCP.
#include <bloom/scripting/live_session_host.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace bloom::scripting {
namespace {

using Clock = std::chrono::steady_clock;
using Apply = std::function<SessionCommandResult(const LiveSessionCommitter&)>;

SessionCommandResult refused(const LiveCommitStatus status) {
    SessionCommandResult result;
    result.status = host::ProjectSessionCommandStatus::InvalidSession;
    std::string message;
    switch (status) {
    case LiveCommitStatus::Completed:
        break;
    case LiveCommitStatus::QueueFull:
        message = "The live commit queue is full; nothing was queued";
        break;
    case LiveCommitStatus::ProjectReplaced:
        message = "The project was replaced before the commit applied; nothing was applied";
        break;
    case LiveCommitStatus::Cancelled:
        message = "The commit was cancelled before it applied; nothing was applied";
        break;
    case LiveCommitStatus::DeadlineExceeded:
        message = "The commit waited past its deadline; nothing was applied";
        break;
    case LiveCommitStatus::GestureOpen:
        message = "An interactive edit stayed open past the commit's deadline; nothing was applied";
        break;
    case LiveCommitStatus::ShuttingDown:
        message = "Bloom is shutting down; nothing was applied";
        break;
    }
    result.diagnostic = OperationDiagnostic{std::string(liveCommitDiagnosticCode(status)), "", "",
                                            std::move(message)};
    return result;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Commit subscription

namespace {
struct CommitWake {
    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    std::uint64_t serial = 0;
    bool closed = false;
    void advance() {
        {
            const std::lock_guard lock(mutex);
            ++serial;
        }
        changed.notify_all();
    }
};
} // namespace

struct LiveCommitSubscription::State final : CommitWake {};

LiveCommitSubscription::LiveCommitSubscription(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}
LiveCommitSubscription::LiveCommitSubscription(LiveCommitSubscription&&) noexcept = default;
LiveCommitSubscription&
LiveCommitSubscription::operator=(LiveCommitSubscription&&) noexcept = default;
LiveCommitSubscription::~LiveCommitSubscription() = default;

std::uint64_t LiveCommitSubscription::serial() const noexcept {
    if (!state_)
        return 0;
    const std::lock_guard lock(state_->mutex);
    return state_->serial;
}

std::optional<std::uint64_t>
LiveCommitSubscription::waitForChange(const std::uint64_t observed,
                                      const std::stop_token& stop) const {
    if (!state_)
        return std::nullopt;
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, stop, [&] { return state_->closed || state_->serial != observed; });
    if (state_->closed || stop.stop_requested())
        return std::nullopt;
    return state_->serial;
}

void LiveCommitSubscription::close() noexcept {
    if (!state_)
        return;
    {
        const std::lock_guard lock(state_->mutex);
        state_->closed = true;
    }
    state_->changed.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Host state (shared with leases, which outlive neither the process nor their hold on it)

namespace {
struct HostCore {
    struct Commit {
        std::uint64_t generation = 0;
        LiveCallControl control;
        Apply apply;
        enum class Step : std::uint8_t { Queued, Applying, Done } step = Step::Queued;
        bool waitedForGesture = false;
        SessionCommandResult result;
    };

    mutable std::mutex mutex;
    std::condition_variable_any finished;
    std::uint64_t generation = 0;
    LiveSessionAvailability availability = LiveSessionAvailability::Replacing;
    std::shared_ptr<const LiveSessionState> mirror;
    LiveSessionServices services;
    std::deque<std::shared_ptr<Commit>> queue;
    std::vector<std::shared_ptr<CommitWake>> subscribers;

    void wakeOwner() {
        // Called with `mutex` held; subscriber locks are leaf locks.
        std::erase_if(subscribers, [](const auto& subscriber) {
            const std::lock_guard lock(subscriber->mutex);
            return subscriber->closed;
        });
        for (const auto& subscriber : subscribers) {
            {
                const std::lock_guard lock(subscriber->mutex);
                ++subscriber->serial;
            }
            subscriber->changed.notify_all();
        }
    }

    void complete(Commit& commit, SessionCommandResult result) {
        commit.result = std::move(result);
        commit.step = Commit::Step::Done;
    }

    // Withdraws every queued commit (the owner keeps applying nothing from them).
    void withdrawAll(const LiveCommitStatus status) {
        for (auto& commit : queue)
            complete(*commit, refused(status));
        queue.clear();
        finished.notify_all();
        wakeOwner();
    }

    SessionCommandResult enqueue(const std::uint64_t expected, const LiveCallControl& control,
                                 Apply apply) {
        auto commit = std::make_shared<Commit>();
        commit->generation = expected;
        commit->control = control;
        commit->apply = std::move(apply);
        std::unique_lock lock(mutex);
        if (availability == LiveSessionAvailability::ShuttingDown)
            return refused(LiveCommitStatus::ShuttingDown);
        if (generation != expected || availability != LiveSessionAvailability::Bound)
            return refused(LiveCommitStatus::ProjectReplaced);
        if (control.stop.stop_requested())
            return refused(LiveCommitStatus::Cancelled);
        if (queue.size() >= kLiveCommitQueueCapacity)
            return refused(LiveCommitStatus::QueueFull);
        queue.push_back(commit);
        wakeOwner();
        const auto done = [&] { return commit->step == Commit::Step::Done; };
        while (!done()) {
            if (commit->step == Commit::Step::Queued) {
                const bool expired = Clock::now() >= control.deadline;
                if (control.stop.stop_requested() || expired) {
                    std::erase(queue, commit);
                    complete(*commit,
                             refused(control.stop.stop_requested() ? LiveCommitStatus::Cancelled
                                     : commit->waitedForGesture
                                         ? LiveCommitStatus::GestureOpen
                                         : LiveCommitStatus::DeadlineExceeded));
                    wakeOwner();
                    break;
                }
                if (control.deadline == Clock::time_point::max())
                    finished.wait(lock, control.stop, done);
                else
                    finished.wait_until(lock, control.stop, control.deadline, done);
            } else {
                // A commit that started applying finishes atomically, whatever the caller's stop
                // token or deadline says.
                finished.wait(lock, done);
            }
        }
        return std::move(commit->result);
    }
};
} // namespace

struct LiveSessionHost::State final : HostCore {};

// ---------------------------------------------------------------------------------------------
// Lease

struct LiveSessionLease::Hold {
    std::shared_ptr<HostCore> host;
    std::uint64_t generation = 0;
    std::shared_ptr<const LiveSessionState> taken;
    mutable std::mutex controlMutex;
    LiveCallControl control;

    [[nodiscard]] bool valid() const {
        const std::lock_guard lock(host->mutex);
        return host->generation == generation &&
               host->availability == LiveSessionAvailability::Bound;
    }
    [[nodiscard]] std::shared_ptr<const LiveSessionState> current() const {
        const std::lock_guard lock(host->mutex);
        if (host->generation != generation || host->availability != LiveSessionAvailability::Bound)
            return nullptr;
        return host->mirror;
    }
    [[nodiscard]] std::shared_ptr<const LiveSessionState> currentOrThrow() const {
        auto state = current();
        if (!state)
            throw std::runtime_error("The live project was replaced");
        return state;
    }
    [[nodiscard]] LiveCallControl callControl() const {
        const std::lock_guard lock(controlMutex);
        return control;
    }
    SessionCommandResult commit(Apply apply) const {
        return host->enqueue(generation, callControl(), std::move(apply));
    }
};

LiveSessionLease::LiveSessionLease(LiveSessionLease&&) noexcept = default;
LiveSessionLease& LiveSessionLease::operator=(LiveSessionLease&&) noexcept = default;
LiveSessionLease::~LiveSessionLease() = default;

const LiveSessionState& LiveSessionLease::state() const noexcept { return *hold_->taken; }

OperationContext LiveSessionLease::operationContext() const {
    if (!hold_)
        return {};
    auto context = state().context;
    if (session_)
        context.layerGeometry = session_->layerGeometryProvider();
    return context;
}

std::shared_ptr<const LiveSessionState> LiveSessionLease::current() const {
    return hold_ ? hold_->current() : nullptr;
}

bool LiveSessionLease::revoked() const noexcept {
    try {
        return !hold_ || !hold_->valid();
    } catch (...) {
        return true;
    }
}

void LiveSessionLease::setControl(LiveCallControl control) {
    if (!hold_)
        return;
    const std::lock_guard lock(hold_->controlMutex);
    hold_->control = std::move(control);
}

SessionCommandResult LiveSessionLease::history(const bool redo,
                                               const document::Revision expectedRevision) {
    if (!hold_)
        return refused(LiveCommitStatus::ProjectReplaced);
    return hold_->commit([redo, expectedRevision](const LiveSessionCommitter& committer) {
        if (committer.revision && committer.revision() != expectedRevision) {
            commands::CommandResult stale;
            stale.action = redo ? commands::CommandAction::Redo : commands::CommandAction::Undo;
            stale.status = commands::CommandStatus::StaleRevision;
            stale.beforeRevision = committer.revision();
            stale.afterRevision = stale.beforeRevision;
            stale.expectedRevision = expectedRevision;
            return SessionCommandResult{host::ProjectSessionCommandStatus::Completed,
                                        std::move(stale), std::nullopt};
        }
        return committer.history ? committer.history(redo) : SessionCommandResult{};
    });
}

// ---------------------------------------------------------------------------------------------
// Host

LiveSessionHost::LiveSessionHost() : state_(std::make_shared<State>()) {}

LiveSessionHost::~LiveSessionHost() {
    const std::lock_guard lock(state_->mutex);
    state_->availability = LiveSessionAvailability::ShuttingDown;
    state_->withdrawAll(LiveCommitStatus::ShuttingDown);
    for (const auto& subscriber : state_->subscribers) {
        {
            const std::lock_guard guard(subscriber->mutex);
            subscriber->closed = true;
        }
        subscriber->changed.notify_all();
    }
}

void LiveSessionHost::replace(LiveSessionServices services, LiveSessionState state) {
    const std::lock_guard lock(state_->mutex);
    if (state_->availability == LiveSessionAvailability::ShuttingDown)
        return;
    if (!state_->queue.empty())
        state_->withdrawAll(LiveCommitStatus::ProjectReplaced);
    ++state_->generation;
    state_->availability = LiveSessionAvailability::Bound;
    state_->services = std::move(services);
    state_->mirror = std::make_shared<const LiveSessionState>(std::move(state));
    state_->wakeOwner();
}

void LiveSessionHost::publish(LiveSessionState state) {
    const std::lock_guard lock(state_->mutex);
    if (state_->availability != LiveSessionAvailability::Bound)
        return;
    state_->mirror = std::make_shared<const LiveSessionState>(std::move(state));
}

void LiveSessionHost::revoke(const LiveSessionAvailability reason) {
    const std::lock_guard lock(state_->mutex);
    if (state_->availability == LiveSessionAvailability::ShuttingDown)
        return;
    if (state_->availability == LiveSessionAvailability::Bound)
        ++state_->generation;
    state_->availability =
        reason == LiveSessionAvailability::Bound ? LiveSessionAvailability::Replacing : reason;
    state_->mirror.reset();
    state_->services = {};
    state_->withdrawAll(reason == LiveSessionAvailability::ShuttingDown
                            ? LiveCommitStatus::ShuttingDown
                            : LiveCommitStatus::ProjectReplaced);
}

bool LiveSessionHost::drainOne(const LiveSessionCommitter& committer) {
    std::unique_lock lock(state_->mutex);
    while (!state_->queue.empty()) {
        auto commit = state_->queue.front();
        if (commit->generation != state_->generation ||
            state_->availability != LiveSessionAvailability::Bound) {
            state_->queue.pop_front();
            state_->complete(*commit, refused(LiveCommitStatus::ProjectReplaced));
            state_->finished.notify_all();
            return true;
        }
        if (commit->control.stop.stop_requested() || Clock::now() >= commit->control.deadline) {
            state_->queue.pop_front();
            state_->complete(
                *commit, refused(commit->control.stop.stop_requested() ? LiveCommitStatus::Cancelled
                                 : commit->waitedForGesture ? LiveCommitStatus::GestureOpen
                                                            : LiveCommitStatus::DeadlineExceeded));
            state_->finished.notify_all();
            return true;
        }
        if (committer.gestureOpen && committer.gestureOpen()) {
            // The commit waits in the queue, in order, until the gesture closes.
            commit->waitedForGesture = true;
            return false;
        }
        state_->queue.pop_front();
        commit->step = HostCore::Commit::Step::Applying;
        lock.unlock();
        SessionCommandResult result;
        try {
            result = commit->apply(committer);
        } catch (const std::exception& error) {
            result.status = host::ProjectSessionCommandStatus::InvalidSession;
            result.diagnostic =
                OperationDiagnostic{"bloom.live.commit-failed", "", "", error.what()};
        }
        lock.lock();
        state_->complete(*commit, std::move(result));
        state_->finished.notify_all();
        return true;
    }
    return false;
}

LiveCommitSubscription LiveSessionHost::subscribeCommits() {
    auto subscriber = std::make_shared<LiveCommitSubscription::State>();
    const std::lock_guard lock(state_->mutex);
    state_->subscribers.push_back(subscriber);
    return LiveCommitSubscription(subscriber);
}

LiveSessionLease LiveSessionHost::acquire(LiveCallControl control) {
    LiveSessionLease lease;
    auto hold = std::make_shared<LiveSessionLease::Hold>();
    hold->host = state_;
    hold->control = std::move(control);
    LiveSessionServices services;
    {
        const std::lock_guard lock(state_->mutex);
        lease.availability_ = state_->availability;
        lease.generation_ = state_->generation;
        if (state_->availability != LiveSessionAvailability::Bound || !state_->mirror)
            return lease;
        hold->generation = state_->generation;
        hold->taken = state_->mirror;
        services = state_->services;
    }
    SessionBinding binding;
    binding.valid = [hold] { return hold->valid(); };
    binding.snapshot = [hold] { return hold->currentOrThrow()->renderSource.snapshot; };
    binding.execute = [hold](commands::Transaction transaction) {
        auto owned = std::make_shared<commands::Transaction>(std::move(transaction));
        return hold->commit([owned](const LiveSessionCommitter& committer) {
            return committer.execute ? committer.execute(std::move(*owned))
                                     : SessionCommandResult{};
        });
    };
    binding.history = [hold](const bool redo) {
        return hold->commit([redo](const LiveSessionCommitter& committer) {
            return committer.history ? committer.history(redo) : SessionCommandResult{};
        });
    };
    binding.events = services.events ? services.events : std::make_shared<EventStream>();
    binding.publication = services.publication;
    binding.artifacts = services.artifacts;
    binding.renderSourceSnapshot = [hold] { return hold->currentOrThrow()->renderSource; };
    auto attached = Session::attach(std::move(binding));
    if (!attached) {
        lease.availability_ = LiveSessionAvailability::Replacing;
        return lease;
    }
    lease.session_ = std::shared_ptr<Session>(std::move(attached).takeSession());
    lease.hold_ = std::move(hold);
    lease.availability_ = LiveSessionAvailability::Bound;
    return lease;
}

LiveSessionAvailability LiveSessionHost::availability() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->availability;
}

std::uint64_t LiveSessionHost::generation() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->generation;
}

std::size_t LiveSessionHost::queuedCommits() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->queue.size();
}

std::shared_ptr<EventStream> LiveSessionHost::events() const {
    const std::lock_guard lock(state_->mutex);
    return state_->availability == LiveSessionAvailability::Bound ? state_->services.events
                                                                  : nullptr;
}

bool LiveSessionHost::waitingForGesture() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return !state_->queue.empty() && state_->queue.front()->waitedForGesture;
}

} // namespace bloom::scripting
