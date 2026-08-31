#include <bloom/ui/qualified_display_processor_bootstrap.hpp>

#include <bloom/ui/task_ui_bridge.hpp>

#include <utility>

namespace bloom::ui {

namespace {

using QualifiedHandle = std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>;

} // namespace

QualifiedDisplayProcessorBootstrap::QualifiedDisplayProcessorBootstrap(
    runtime::TaskScheduler& scheduler, TaskUiBridge& taskUiBridge,
    runtime::QualifiedDisplayProcessorProvider& provider, QObject* parent)
    : QObject(parent), scheduler_(scheduler), provider_(provider) {
    connect(&taskUiBridge, &TaskUiBridge::snapshotsPolled, this,
            &QualifiedDisplayProcessorBootstrap::pollOnce);

    runtime::TaskRequest request(
        "Build the qualified Bloom Neutral display processor",
        {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);

    auto submission = scheduler_.submit<QualifiedHandle>(
        std::move(request), [](runtime::TaskContext&) -> runtime::TaskResult<QualifiedHandle> {
            auto built = runtime::buildBloomNeutralQualifiedDisplayProcessor();
            if (!built.succeeded()) {
                return runtime::TaskResult<QualifiedHandle>::failed(built.diagnostic());
            }
            return runtime::TaskResult<QualifiedHandle>::succeeded(built.handle());
        });

    if (!submission.accepted()) {
        provider_.publish(runtime::QualifiedDisplayProcessorBuildResult::failed(
            {.code = "bloom.ui.qualified-display-bootstrap.submission-failed",
             .severity = runtime::DiagnosticSeverity::Error,
             .summary =
                 "The qualified Bloom Neutral display processor build could not be scheduled",
             .detail = {},
             .suggestedAction = "Restart the application; report this if it persists."}));
        completed_ = true;
        return;
    }
    handle_ = std::move(submission.handle);
    submitted_ = true;
    taskUiBridge.wake();
}

void QualifiedDisplayProcessorBootstrap::pollOnce() {
    if (completed_ || !submitted_) {
        return;
    }
    auto result = handle_.tryTakeResult();
    if (!result.has_value()) {
        return;
    }
    completed_ = true;

    if (result->state() == runtime::TaskState::Succeeded) {
        const auto& value = result->value();
        if (value.has_value() && *value != nullptr) {
            provider_.publish(runtime::QualifiedDisplayProcessorBuildResult::ready(*value));
            return;
        }
    }

    const auto& diagnostics = result->diagnostics();
    runtime::TaskDiagnostic diagnostic =
        diagnostics.empty()
            ? runtime::TaskDiagnostic{.code = "bloom.ui.qualified-display-bootstrap.no-diagnostic",
                                      .severity = runtime::DiagnosticSeverity::Error,
                                      .summary = "The qualified Bloom Neutral display processor "
                                                 "could not be built",
                                      .detail = {},
                                      .suggestedAction =
                                          "Restart the application; report this if it persists."}
            : diagnostics.front();
    provider_.publish(runtime::QualifiedDisplayProcessorBuildResult::failed(std::move(diagnostic)));
}

} // namespace bloom::ui
