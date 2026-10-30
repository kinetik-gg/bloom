// bloom.scripting.live-session-host (ADR 0034, section 10): the shared live session marshal. FIFO
// with one commit per drain, the 16-commit bound, a commit that waits behind an open gesture and is
// withdrawn at its deadline, cancellation that withdraws a queued commit and never a started one,
// revocation with ProjectReplaced, and leases that keep reading after replacement but refuse to
// commit.
#include <bloom/commands/operations.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

// A headless session standing in for the desktop owner: the test thread plays the UI thread.
struct Owner final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    commands::CommandObserverId observer = session->commandStack()->addObserver(
        [stream = events](const commands::CommandEvent& event) { stream->publish(event); });
    std::atomic_bool gesture = false;
    std::vector<std::string> applied;
    scripting::LiveSessionHost host;

    Owner() { bind(); }
    ~Owner() { session->commandStack()->removeObserver(observer); }
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;

    scripting::LiveSessionState state() const {
        scripting::LiveSessionState state{.renderSource = session->renderSourceSnapshot(),
                                          .context = session->operationContext(),
                                          .viewerDisplay = {},
                                          .viewerView = {},
                                          .projectPath = std::nullopt,
                                          .readOnly = false};
        state.context.layerGeometry = {};
        return state;
    }
    void bind() {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     state());
    }
    scripting::LiveSessionCommitter committer() {
        return {.execute =
                    [this](commands::Transaction transaction) {
                        applied.emplace_back(transaction.label());
                        auto result = session->execute(std::move(transaction));
                        host.publish(state());
                        return result;
                    },
                .history =
                    [this](const bool redo) {
                        auto result = redo ? session->redo() : session->undo();
                        host.publish(state());
                        return result;
                    },
                .gestureOpen = [this] { return gesture.load(); },
                .revision = [this] { return session->snapshot().revision(); }};
    }
    // Drains until `predicate` holds, like the adapter's event-loop slices.
    template <typename Predicate> void drainUntil(Predicate predicate) {
        const auto committing = committer();
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            if (!host.drainOne(committing))
                std::this_thread::sleep_for(1ms);
        }
    }
};

commands::Transaction rename(const std::string& name, const document::Revision revision) {
    commands::Transaction transaction(name, revision);
    transaction.emplace<commands::SetProjectName>(name);
    return transaction;
}

void fifoOnePerDrain() {
    Owner owner;
    const auto committing = owner.committer();
    std::vector<std::future<scripting::SessionCommandResult>> results;
    for (int index = 0; index < 3; ++index) {
        results.push_back(std::async(std::launch::async, [&owner, index] {
            auto lease = owner.host.acquire({});
            std::this_thread::sleep_for(std::chrono::milliseconds(20 * index));
            return lease.session().execute(
                rename("Step " + std::to_string(index), document::Revision::fromRaw(0)));
        }));
    }
    while (owner.host.queuedCommits() < 3)
        std::this_thread::sleep_for(1ms);
    expect(owner.host.drainOne(committing) && owner.applied.size() == 1,
           "one drain applies exactly one commit");
    expect(owner.host.queuedCommits() == 2, "the others stay queued");
    owner.drainUntil([&] { return owner.applied.size() == 3; });
    expect(owner.applied == std::vector<std::string>{"Step 0", "Step 1", "Step 2"},
           "commits apply in FIFO order");
    expect(results[0].get().succeeded(), "the first commit applies");
    expect(!results[1].get().succeeded(), "a later commit with a stale revision is refused");
    static_cast<void>(results[2].get());
}

void boundedAtSixteen() {
    Owner owner;
    std::stop_source stop;
    std::vector<std::future<scripting::SessionCommandResult>> queued;
    for (std::size_t index = 0; index < scripting::kLiveCommitQueueCapacity; ++index) {
        queued.push_back(std::async(std::launch::async, [&owner, &stop] {
            auto lease = owner.host.acquire({.stop = stop.get_token()});
            return lease.session().execute(rename("Queued", document::Revision::fromRaw(0)));
        }));
    }
    while (owner.host.queuedCommits() < scripting::kLiveCommitQueueCapacity)
        std::this_thread::sleep_for(1ms);
    auto lease = owner.host.acquire({});
    const auto full = lease.session().execute(rename("Overflow", document::Revision::fromRaw(0)));
    expect(!full.succeeded() && full.diagnostic &&
               full.diagnostic->code ==
                   scripting::liveCommitDiagnosticCode(scripting::LiveCommitStatus::QueueFull),
           "the seventeenth commit is refused with queue-full");
    stop.request_stop();
    for (auto& result : queued) {
        const auto refused = result.get();
        expect(refused.diagnostic && refused.diagnostic->code == "bloom.live.cancelled",
               "cancellation withdraws every queued commit");
    }
    expect(owner.host.queuedCommits() == 0 && owner.applied.empty(), "nothing was applied");
}

void gestureWaitsThenDeadline() {
    Owner owner;
    owner.gesture = true;
    auto waiting = std::async(std::launch::async, [&owner] {
        auto lease = owner.host.acquire({});
        return lease.session().execute(rename("After drag", document::Revision::fromRaw(0)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    const auto committing = owner.committer();
    expect(!owner.host.drainOne(committing) && owner.host.waitingForGesture(),
           "a commit never applies while a gesture is open");
    owner.gesture = false;
    owner.drainUntil([&] { return !owner.applied.empty(); });
    expect(waiting.get().succeeded(), "the commit applies once the gesture ends");

    owner.gesture = true;
    const auto deadline = std::chrono::steady_clock::now() + 150ms;
    auto expired = std::async(std::launch::async, [&owner, deadline] {
        auto lease = owner.host.acquire({.stop = {}, .deadline = deadline});
        return lease.session().execute(rename("Never", document::Revision::fromRaw(1)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    expect(!owner.host.drainOne(committing), "still blocked by the gesture");
    const auto result = expired.get();
    expect(result.diagnostic && result.diagnostic->code == "bloom.live.gesture-open",
           "a commit held past its deadline by a gesture is withdrawn with gesture-open");
    expect(owner.applied.size() == 1 && owner.host.queuedCommits() == 0,
           "the withdrawn commit never applies");
}

void cancellationNeverInterruptsAStartedCommit() {
    Owner owner;
    std::stop_source stop;
    std::promise<void> entered;
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    auto started = std::async(std::launch::async, [&] {
        auto lease = owner.host.acquire({.stop = stop.get_token()});
        return lease.session().execute(rename("Atomic", document::Revision::fromRaw(0)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    auto committing = owner.committer();
    auto plain = committing.execute;
    committing.execute = [&](commands::Transaction transaction) {
        entered.set_value();
        releaseFuture.wait();
        return plain(std::move(transaction));
    };
    std::thread drain([&] { static_cast<void>(owner.host.drainOne(committing)); });
    entered.get_future().wait();
    stop.request_stop();
    std::this_thread::sleep_for(20ms);
    release.set_value();
    drain.join();
    expect(started.get().succeeded(), "a commit that started applying finishes atomically");
    expect(owner.session->snapshot().project().name() == "Atomic", "and it landed");
}

void revocationAndLeases() {
    Owner owner;
    auto reader = owner.host.acquire({});
    expect(static_cast<bool>(reader) &&
               reader.availability() == scripting::LiveSessionAvailability::Bound,
           "a lease is available while bound");
    const auto readGeneration = reader.generation();
    auto queued = std::async(std::launch::async, [&owner] {
        auto lease = owner.host.acquire({});
        return lease.session().execute(rename("Lost", document::Revision::fromRaw(0)));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    owner.host.revoke(scripting::LiveSessionAvailability::Replacing);
    const auto lost = queued.get();
    expect(lost.diagnostic && lost.diagnostic->code == "bloom.live.project-replaced",
           "revocation withdraws a queued commit with project-replaced");
    expect(!owner.host.acquire({}) && owner.host.acquire({}).availability() ==
                                          scripting::LiveSessionAvailability::Replacing,
           "no lease while a project is being replaced");
    owner.bind();
    expect(owner.host.generation() != readGeneration, "a new generation starts");
    expect(reader.revoked() && reader.state().renderSource.snapshot.project().name() == "Untitled",
           "an old lease keeps reading the state it took");
    const auto refused = reader.session().execute(rename("Old", document::Revision::fromRaw(0)));
    // The attached session sees its revoked binding and refuses before queueing anything.
    expect(!refused.succeeded() && !refused.command && owner.host.queuedCommits() == 0,
           "an old lease refuses commits");
    auto fresh = owner.host.acquire({});
    auto history = std::async(std::launch::async, [&fresh] {
        return fresh.history(false, document::Revision::fromRaw(7));
    });
    while (owner.host.queuedCommits() == 0)
        std::this_thread::sleep_for(1ms);
    owner.drainUntil([&] { return owner.host.queuedCommits() == 0; });
    const auto stale = history.get();
    expect(stale.command && stale.command->status == commands::CommandStatus::StaleRevision,
           "a revision-guarded undo is checked on the owner thread");
    expect(owner.host.events() != nullptr, "the bound generation publishes its event stream");
    owner.host.revoke(scripting::LiveSessionAvailability::ShuttingDown);
    expect(owner.host.availability() == scripting::LiveSessionAvailability::ShuttingDown,
           "shutdown is terminal");
    const auto after = fresh.session().execute(rename("Late", document::Revision::fromRaw(0)));
    expect(!after.succeeded() && owner.host.queuedCommits() == 0,
           "commits after shutdown are refused");
}

void subscriptionWakesTheOwner() {
    Owner owner;
    auto subscription = owner.host.subscribeCommits();
    const auto observed = subscription.serial();
    std::stop_source stop;
    auto commit = std::async(std::launch::async, [&] {
        auto lease = owner.host.acquire({.stop = stop.get_token()});
        return lease.session().execute(rename("Wake", document::Revision::fromRaw(0)));
    });
    const auto changed = subscription.waitForChange(observed, {});
    expect(changed.has_value() && *changed != observed, "a queued commit wakes the owner");
    owner.drainUntil([&] { return !owner.applied.empty(); });
    expect(commit.get().succeeded(), "and the owner applies it");
    subscription.close();
    expect(!subscription.waitForChange(subscription.serial(), {}).has_value(),
           "a closed subscription stops waiting");
}

} // namespace

int main() {
    try {
        fifoOnePerDrain();
        boundedAtSixteen();
        gestureWaitsThenDeadline();
        cancellationNeverInterruptsAStartedCommit();
        revocationAndLeases();
        subscriptionWakesTheOwner();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "live session host: FIFO, bound, gesture, cancellation, revocation and "
                     "leases passed\n";
    return failures == 0 ? 0 : 1;
}
