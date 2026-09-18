#include "script_panel_runtime.hpp"
#include "runtime.hpp"

#include <QCoreApplication>
#include <QTimer>
#include <bloom/scripting/events.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/project_host.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>

namespace bloom::ui {
namespace py = scripting::python;
namespace {
using Result = scripting::SessionCommandResult;
using Action = std::function<Result(CompositionSession&, ProjectHost&)>;
Result completed(commands::CommandResult result) {
    return {.status = host::ProjectSessionCommandStatus::Completed,
            .command = std::move(result),
            .diagnostic = std::nullopt};
}
} // namespace
struct ScriptPanelRuntime::State {
    struct Work {
        std::string source;
        std::uint64_t generation, cancellation;
    };
    struct Request {
        std::uint64_t generation, cancellation;
        Action action;
        std::promise<Result> result;
    };
    struct Output {
        std::string source;
        py::ExecutionResult result;
    };
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::atomic_bool alive = true, executing = false, available = false;
    std::atomic_uint64_t generation = 1, cancellation = 0, activeCancellation = 0;
    std::optional<document::Snapshot> snapshot;
    py::PythonContext context;
    std::deque<Work> scripts;
    std::deque<std::shared_ptr<Request>> requests;
    std::deque<Output> outputs;
    std::shared_ptr<scripting::EventStream> events;
    std::shared_ptr<int> observerLife;
    commands::CommandStack* observedStack = nullptr;
    commands::CommandObserverId observer = 0;
    host::PublicationCoordinator* publication;
    platform::StagedArtifactCoordinator* artifacts;
    std::shared_ptr<py::EmbeddedPython> python;
    std::filesystem::path executable;

    Result enqueue(std::uint64_t expected, Action action) {
        auto request = std::make_shared<Request>();
        request->generation = expected;
        request->cancellation = activeCancellation.load();
        request->action = std::move(action);
        auto result = request->result.get_future();
        {
            const std::lock_guard lock(mutex);
            if (requests.size() == 16)
                throw std::runtime_error("The UI transaction queue is full");
            requests.push_back(request);
        }
        while (result.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
            if (!alive || generation != expected || cancellation != request->cancellation)
                throw std::runtime_error(
                    "The queued transaction was cancelled or its project replaced");
        }
        return result.get();
    }
};

ScriptPanelRuntime::ScriptPanelRuntime(CompositionSession& session, ProjectHost& host)
    : session_(session), host_(host), state_(std::make_shared<State>()) {
    state_->publication = &host_.publicationCoordinator();
    state_->artifacts = &host_.artifactCoordinator();
    state_->executable = QCoreApplication::applicationFilePath().toStdString();
    replaceSession();
    connect(&session_, &CompositionSession::snapshotChanged, this, &ScriptPanelRuntime::refresh);
    connect(&session_, &CompositionSession::compositionChanged, this, &ScriptPanelRuntime::refresh);
    connect(&session_, &CompositionSession::selectionChanged, this, &ScriptPanelRuntime::refresh);
    connect(&session_, &CompositionSession::currentTimeChanged, this, &ScriptPanelRuntime::refresh);
    connect(&host_, &ProjectHost::sessionReplaced, this, &ScriptPanelRuntime::replaceSession);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            &ScriptPanelRuntime::shutdown);
    auto* timer = new QTimer(this);
    // A scheduler cadence, independent of reduced animation settings.
    constexpr auto pumpInterval = std::chrono::milliseconds{16};
    timer->setInterval(pumpInterval);
    connect(timer, &QTimer::timeout, this, &ScriptPanelRuntime::poll);
    timer->start();
}
ScriptPanelRuntime::~ScriptPanelRuntime() {
    shutdown();
    state_->observerLife.reset();
    if (state_->observedStack == host_.liveDocumentAndStack().second && state_->observedStack)
        state_->observedStack->removeObserver(state_->observer);
    if (worker_.joinable())
        worker_.join();
}
void ScriptPanelRuntime::refresh() {
    const std::lock_guard lock(state_->mutex);
    state_->snapshot = session_.snapshot();
    state_->context.project = session_.snapshot().project().id().value();
    state_->context.composition = session_.compositionId().value();
    state_->context.time = session_.currentTime();
    state_->context.selection.clear();
    for (const auto id : session_.selectedNodes())
        state_->context.selection.push_back(id.value());
}
void ScriptPanelRuntime::replaceSession() {
    cancel();
    ++state_->generation;
    state_->observerLife.reset();
    auto* const stack = host_.liveDocumentAndStack().second;
    if (stack && stack == state_->observedStack)
        stack->removeObserver(state_->observer);
    state_->observedStack = stack;
    state_->available = stack != nullptr;
    auto stream = std::make_shared<scripting::EventStream>();
    {
        const std::lock_guard lock(state_->mutex);
        state_->events = stream;
    }
    state_->observerLife = std::make_shared<int>(0);
    if (stack) {
        state_->observer = stack->addObserver([life = std::weak_ptr(state_->observerLife),
                                               stream](const commands::CommandEvent& event) {
            if (!life.expired())
                stream->publish(event);
        });
    }
    refresh();
}
bool ScriptPanelRuntime::submit(const QString& source) {
    if (source.isEmpty() || source.size() > 65536 || !state_->alive)
        return false;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->scripts.size() == 16)
            return false;
        state_->scripts.push_back(
            {source.toStdString(), state_->generation.load(), state_->cancellation.load()});
    }
    if (!worker_.joinable())
        worker_ = std::jthread([state = state_] { work(state); });
    state_->ready.notify_one();
    emit activityChanged(QStringLiteral("Queued"), true);
    return true;
}
void ScriptPanelRuntime::cancel() {
    ++state_->cancellation;
    const std::lock_guard lock(state_->mutex);
    if (state_->python)
        state_->python->requestCancellation();
    state_->ready.notify_one();
}
void ScriptPanelRuntime::shutdown() {
    state_->alive = false;
    cancel();
}
bool ScriptPanelRuntime::busy() const {
    const std::lock_guard lock(state_->mutex);
    return state_->executing || !state_->scripts.empty() || !state_->requests.empty();
}
void ScriptPanelRuntime::poll() {
    std::shared_ptr<State::Request> request;
    std::deque<State::Output> outputs;
    std::vector<runtime::TaskSnapshot> tasks;
    {
        const std::lock_guard lock(state_->mutex);
        if (!state_->requests.empty()) {
            request = std::move(state_->requests.front());
            state_->requests.pop_front();
        }
        outputs.swap(state_->outputs);
        if (state_->python)
            tasks = state_->python->tasks();
    }
    // Exactly one queued authoring transaction per event-loop slice.
    if (request) {
        try {
            if (!state_->alive || request->generation != state_->generation ||
                request->cancellation != state_->cancellation)
                throw std::runtime_error(
                    "The queued transaction was cancelled or its project replaced");
            request->result.set_value(request->action(session_, host_));
        } catch (...) {
            request->result.set_exception(std::current_exception());
        }
    }
    for (auto& output : outputs)
        emit outputReady(QString::fromStdString(output.source),
                         QString::fromStdString(output.result.output), output.result.succeeded);
    QString message = busy() ? QStringLiteral("Running") : QStringLiteral("Ready");
    bool active = busy();
    for (const auto& task : tasks) {
        if (runtime::isTerminal(task.state))
            continue;
        active = true;
        message = QString::fromStdString(task.name + ": " + task.progress.phase);
        if (task.progress.total)
            message +=
                QStringLiteral(" %1/%2").arg(task.progress.completed).arg(*task.progress.total);
        break;
    }
    emit activityChanged(message, active);
}

void ScriptPanelRuntime::work(const std::shared_ptr<State>& state) {
    try {
        auto python = std::make_shared<py::EmbeddedPython>(state->executable);
        {
            const std::lock_guard lock(state->mutex);
            state->python = python;
        }
        std::uint64_t boundGeneration = 0;
        while (state->alive) {
            State::Work work;
            {
                std::unique_lock lock(state->mutex);
                state->ready.wait(lock, [&] { return !state->alive || !state->scripts.empty(); });
                if (!state->alive)
                    break;
                work = std::move(state->scripts.front());
                state->scripts.pop_front();
            }
            state->executing = true;
            state->activeCancellation = work.cancellation;
            py::ExecutionResult result;
            try {
                if (work.generation != state->generation ||
                    work.cancellation != state->cancellation)
                    throw std::runtime_error("Script cancelled or project replaced; run it again");
                if (boundGeneration != work.generation) {
                    const auto generation = work.generation;
                    const auto valid = [state, generation] {
                        return state->alive.load() && state->available.load() &&
                               state->generation == generation;
                    };
                    scripting::SessionBinding binding;
                    binding.valid = valid;
                    binding.snapshot = [state, valid] {
                        const std::lock_guard lock(state->mutex);
                        if (!valid() || !state->snapshot)
                            throw std::runtime_error("The live project was replaced");
                        return *state->snapshot;
                    };
                    binding.execute = [state, generation](commands::Transaction transaction) {
                        auto owned =
                            std::make_shared<commands::Transaction>(std::move(transaction));
                        return state->enqueue(
                            generation, [owned](CompositionSession& session, ProjectHost&) {
                                return completed(session.executeTransaction(std::move(*owned)));
                            });
                    };
                    binding.history = [state, generation](bool redo) {
                        return state->enqueue(
                            generation, [redo](CompositionSession&, ProjectHost& host) {
                                auto* stack = host.liveDocumentAndStack().second;
                                return stack ? completed(redo ? stack->redo() : stack->undo())
                                             : Result{};
                            });
                    };
                    {
                        const std::lock_guard lock(state->mutex);
                        binding.events = state->events;
                    }
                    binding.publication = state->publication;
                    binding.artifacts = state->artifacts;
                    auto attached = scripting::Session::attach(std::move(binding));
                    if (!attached)
                        throw std::runtime_error(attached.diagnostic().message);
                    python->bindLive(
                        std::move(attached).takeSession(),
                        [state, valid] {
                            const std::lock_guard lock(state->mutex);
                            if (!valid())
                                throw std::runtime_error("The live project was replaced");
                            return state->context;
                        },
                        [state, valid] {
                            return !valid() || state->cancellation != state->activeCancellation;
                        });
                    boundGeneration = generation;
                }
                result = python->execute(work.source, true);
                if (result.incomplete) {
                    result.succeeded = false;
                    result.output =
                        "Incomplete input; use exec(\"line one\\nline two\") for a block";
                }
            } catch (const std::exception& error) {
                result.output = error.what();
            }
            {
                const std::lock_guard lock(state->mutex);
                if (state->outputs.size() == 32)
                    state->outputs.pop_front();
                state->outputs.push_back({std::move(work.source), std::move(result)});
            }
            state->executing = false;
        }
        python->requestCancellation();
        {
            const std::lock_guard lock(state->mutex);
            state->python.reset();
        }
        // Interpreter shutdown and cooperative task joining occur on the scripting thread.
        python.reset();
    } catch (const std::exception& error) {
        const std::lock_guard lock(state->mutex);
        state->outputs.push_back({{}, {.output = error.what()}});
        state->alive = false;
        state->python.reset();
        state->executing = false;
    }
}
} // namespace bloom::ui
