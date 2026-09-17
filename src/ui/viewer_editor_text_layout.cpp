#include "viewer_editor_text_layout.hpp"
#include <bloom/runtime/snapshot_compiler.hpp>
#include <chrono>
#include <thread>

namespace bloom::ui {
ViewerTextLayout::ViewerTextLayout(QObject* parent) : QObject(parent) {
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    scheduler_ = std::make_shared<runtime::TaskScheduler>(config);
    retire_ = std::make_shared<std::atomic_bool>(false);
    std::thread([scheduler = scheduler_, retire = retire_] {
        retire->wait(false);
        while (!scheduler->isQuiescent())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }).detach();
}
ViewerTextLayout::~ViewerTextLayout() {
    cancel();
    scheduler_->beginShutdown();
    retire_->store(true);
    retire_->notify_one();
}
void ViewerTextLayout::cancel() {
    ++generation_;
    pending_.reset();
    task_.cancel();
}
void ViewerTextLayout::request(Request request) {
    pending_ = std::move(request);
    ++generation_;
    if (active_)
        task_.cancel();
    else
        start();
}
void ViewerTextLayout::start() {
    if (!pending_) {
        timer_.stop();
        return;
    }
    auto request = std::move(*pending_);
    pending_.reset();
    activeGeneration_ = generation_;
    const auto owner = runtime::TaskOwnerId::fromRaw(request.composition.value());
    const auto submission = scheduler_->submit<std::shared_ptr<Result>>(
        runtime::TaskRequest{"Layout Canvas Text",
                             {runtime::TaskOwnerKind::Composition, owner},
                             runtime::TaskPriority::Interactive},
        [request = std::move(request), font = font_](runtime::TaskContext& context) mutable {
            auto result = std::make_shared<Result>();
            result->content = request.content;
            if (!font) {
                runtime::NodeDefinitionRegistry definitions;
                if (!runtime::registerBuiltInNodeDefinitions(definitions))
                    return runtime::TaskResult<std::shared_ptr<Result>>::cancelled();
                definitions.freeze();
                const auto compiled = runtime::SnapshotCompiler(definitions)
                                          .compile({request.snapshot, request.composition, {}},
                                                   context.cancellation());
                if (compiled.plan) {
                    for (const auto& operation : compiled.plan->operations())
                        if (const auto* text = std::get_if<runtime::CompiledText>(&operation);
                            text && text->contentParameterId == request.parameter)
                            font = text->font;
                }
                if (!font)
                    result->diagnostic = tr("Text layout is unavailable");
            }
            if (font) {
                result->font = *font;
                auto layout =
                    render::layoutText(*font, request.content, request.parameters, request.options,
                                       [&context] { return context.isCancellationRequested(); });
                if (layout)
                    result->layout = std::move(*layout.value());
                else
                    result->diagnostic = tr("Text layout could not be prepared");
            }
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Result>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<Result>>::succeeded(std::move(result));
        });
    task_ = submission.handle;
    active_ = submission.accepted();
    if (active_)
        timer_.start();
    else if (ready)
        ready({{}, {}, {}, tr("Text layout could not start")});
}
void ViewerTextLayout::poll() {
    if (auto result = task_.tryTakeResult()) {
        active_ = false;
        if (activeGeneration_ == generation_) {
            if (result->value()) {
                auto value = std::move(**result->value());
                if (value.diagnostic.isEmpty())
                    font_ = value.font;
                if (ready)
                    ready(std::move(value));
            } else if (ready) {
                const auto message =
                    result->diagnostics().empty()
                        ? tr("Text layout could not be prepared")
                        : QString::fromStdString(result->diagnostics().front().summary);
                ready({{}, {}, {}, message});
            }
        }
        start();
    }
}
} // namespace bloom::ui
