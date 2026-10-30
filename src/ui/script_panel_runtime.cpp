#include "script_panel_runtime.hpp"
#include "runtime.hpp"

#include <QCoreApplication>
#include <QTimer>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/scripting/live_session_host.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace bloom::ui {
namespace py = scripting::python;

struct ScriptPanelRuntime::State {
    explicit State(scripting::LiveSessionHost& host) : live(host) {}
    struct Work {
        std::string source;
        std::uint64_t cancellation = 0;
    };
    struct Output {
        std::string source;
        py::ExecutionResult result;
    };
    scripting::LiveSessionHost& live;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::atomic_bool alive = true, executing = false;
    std::atomic_uint64_t cancellation = 0, activeCancellation = 0;
    // Cancels the running script's queued commits (Cancel, replacement, shutdown).
    std::stop_source scriptStop;
    std::deque<Work> scripts;
    std::deque<Output> outputs;
    std::shared_ptr<py::EmbeddedPython> python;
    std::shared_ptr<host::GpuExportProvider> gpuExportProvider;
    bool ownsGpuExportProvider = false;
    std::filesystem::path executable;
};

ScriptPanelRuntime::ScriptPanelRuntime(scripting::LiveSessionHost& live,
                                       std::shared_ptr<host::GpuExportProvider> provider)
    : state_(std::make_shared<State>(live)) {
    state_->ownsGpuExportProvider = provider == nullptr;
    state_->gpuExportProvider = provider ? std::move(provider) : host::GpuExportProvider::create();
    state_->executable = QCoreApplication::applicationFilePath().toStdString();
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            &ScriptPanelRuntime::shutdown);
    // Console output and task progress only; commits are drained by the LiveSessionAdapter.
    auto* timer = new QTimer(this);
    constexpr auto pumpInterval = std::chrono::milliseconds{16};
    timer->setInterval(pumpInterval);
    connect(timer, &QTimer::timeout, this, &ScriptPanelRuntime::poll);
    timer->start();
}
ScriptPanelRuntime::~ScriptPanelRuntime() {
    shutdown();
    if (worker_.joinable())
        worker_.join();
}
bool ScriptPanelRuntime::submit(const QString& source) {
    if (source.isEmpty() || source.size() > 65536 || !state_->alive)
        return false;
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->scripts.size() == 16)
            return false;
        state_->scripts.push_back({source.toStdString(), state_->cancellation.load()});
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
    state_->scriptStop.request_stop();
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
    return state_->executing || !state_->scripts.empty();
}
void ScriptPanelRuntime::poll() {
    std::deque<State::Output> outputs;
    std::vector<runtime::TaskSnapshot> tasks;
    {
        const std::lock_guard lock(state_->mutex);
        outputs.swap(state_->outputs);
        if (state_->python)
            tasks = state_->python->tasks();
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
    struct OwnedRootRetirement final {
        std::shared_ptr<State> state;
        ~OwnedRootRetirement() {
            if (state->ownsGpuExportProvider) {
                static_cast<void>(
                    state->gpuExportProvider->shutdownAndWait(std::chrono::seconds(10)));
            }
        }
    } retirement{state};
    try {
        auto python = std::make_shared<py::EmbeddedPython>(state->executable);
        {
            const std::lock_guard lock(state->mutex);
            state->python = python;
        }
        // One lease per live generation: Python proxies bind its session and go stale with it.
        std::shared_ptr<scripting::LiveSessionLease> lease;
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
                if (work.cancellation != state->cancellation)
                    throw std::runtime_error("Script cancelled or project replaced; run it again");
                if (!lease || lease->revoked()) {
                    auto acquired = state->live.acquire({});
                    if (!acquired)
                        throw std::runtime_error(
                            "The live project is being replaced; run the script again");
                    lease = std::make_shared<scripting::LiveSessionLease>(std::move(acquired));
                    python->bindLive(
                        lease->sharedSession(),
                        [lease] {
                            const auto current = lease->current();
                            if (!current)
                                throw std::runtime_error("The live project was replaced");
                            const auto& context = current->context;
                            return py::PythonContext{
                                .project = context.project.value_or(0),
                                .composition = context.composition.value_or(0),
                                .selection = context.selection,
                                .time = context.time.value_or(core::RationalTime::fromInteger(0))};
                        },
                        [state, lease] {
                            return lease->revoked() ||
                                   state->cancellation != state->activeCancellation;
                        },
                        state->gpuExportProvider);
                }
                // Each script's commits carry its own cancellation.
                {
                    const std::lock_guard lock(state->mutex);
                    state->scriptStop = std::stop_source();
                    if (work.cancellation != state->cancellation)
                        state->scriptStop.request_stop();
                    lease->setControl({.stop = state->scriptStop.get_token(),
                                       .deadline = std::chrono::steady_clock::time_point::max()});
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
        lease.reset();
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
