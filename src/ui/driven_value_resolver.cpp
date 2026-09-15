#include "driven_value_resolver.hpp"
#include "composition_driver_probe.hpp"
#include <QTimer>
#include <algorithm>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <chrono>
#include <thread>

namespace bloom::ui {
namespace {
runtime::CompiledValue promotedValue(const runtime::CompiledValue& value, std::string_view schema) {
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        for (const auto& parameter : definition.parameters) {
            if (parameter.schemaKey != schema)
                continue;
            if (parameter.valueKind == document::ParameterValueKind::Integer) {
                if (const auto* boolean = std::get_if<bool>(&value))
                    return static_cast<std::int64_t>(*boolean);
            }
            if (parameter.valueKind == document::ParameterValueKind::Float64) {
                if (const auto* integer = std::get_if<std::int64_t>(&value))
                    return static_cast<double>(*integer);
                if (const auto* boolean = std::get_if<bool>(&value))
                    return static_cast<double>(*boolean);
            }
            if (const auto* scalar = std::get_if<double>(&value)) {
                if (parameter.valueKind == document::ParameterValueKind::Vec2d)
                    return document::Vec2d{*scalar, *scalar};
                if (parameter.valueKind == document::ParameterValueKind::Vec3d)
                    return document::Vec3d{*scalar, *scalar, *scalar};
            }
            return value;
        }
    }
    return value;
}
QString valueText(const runtime::CompiledValue& value) {
    return std::visit(
        [](const auto& held) -> QString {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, core::Color4d>)
                return exactColorText(held);
            else if constexpr (std::is_same_v<T, document::Vec2d>)
                return QString("%1, %2").arg(held.x, 0, 'g', 12).arg(held.y, 0, 'g', 12);
            else if constexpr (std::is_same_v<T, document::Vec3d>)
                return QString("%1, %2, %3")
                    .arg(held.x, 0, 'g', 12)
                    .arg(held.y, 0, 'g', 12)
                    .arg(held.z, 0, 'g', 12);
            else if constexpr (std::is_same_v<T, std::string>)
                return QString::fromStdString(held);
            else if constexpr (std::is_same_v<T, bool>)
                return held ? QObject::tr("True") : QObject::tr("False");
            else if constexpr (std::is_same_v<T, double>)
                return QString::number(held, 'g', 12);
            else
                return QString::number(static_cast<qlonglong>(held));
        },
        value);
}
} // namespace
DrivenValueResolver::DrivenValueResolver(CompositionSession& session, QObject* parent)
    : QObject(parent), session_(session), timer_(new QTimer(this)) {
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
}
DrivenValueResolver::~DrivenValueResolver() {
    if (!scheduler_)
        return;
    task_.cancel();
    scheduler_->beginShutdown();
    retire_->store(true);
    retire_->notify_one();
}
void DrivenValueResolver::request(std::vector<document::ParameterId> parameters) {
    parameters_ = std::move(parameters);
    ++generation_;
    if (active_)
        task_.cancel();
    else
        start();
}
void DrivenValueResolver::start() {
    if (parameters_.empty()) {
        timer_->stop();
        return;
    }
    if (!scheduler_) {
        auto config = runtime::TaskSchedulerConfig::defaults();
        config.cpuWorkerCount = 1;
        config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
        scheduler_ = std::make_shared<runtime::TaskScheduler>(config);
        retire_ = std::make_shared<std::atomic_bool>(false);
        // Start the Qt-free reaper while construction can report a thread-start failure. Closing
        // the panel only requests cancellation; it never creates or joins a thread.
        std::thread([scheduler = scheduler_, retire = retire_] {
            retire->wait(false);
            while (!scheduler->isQuiescent())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }).detach();
    }
    const auto snapshot = session_.snapshot();
    const auto compositionId = session_.compositionId();
    const auto time = session_.currentTime();
    const auto parameters = parameters_;
    activeGeneration_ = generation_;
    const auto submission = scheduler_->submit<std::shared_ptr<Values>>(
        runtime::TaskRequest{
            "Resolve Properties Drivers",
            {runtime::TaskOwnerKind::Application, runtime::TaskOwnerId::fromRaw(1)},
            runtime::TaskPriority::Interactive},
        [snapshot, compositionId, time, parameters](runtime::TaskContext& context) {
            auto values = std::make_shared<Values>();
            for (auto parameter : parameters)
                (*values)[parameter] = QObject::tr("Unavailable");
            const auto compiled =
                compileDriverProbe(snapshot, compositionId, parameters, context.cancellation());
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Values>>::cancelled();
            const auto* composition = snapshot.project().findComposition(compositionId);
            if (compiled.plan && composition) {
                const auto& plan = *compiled.plan;
                runtime::ValueGraphMemoization memoization;
                memoization.cancellation = &context.cancellation();
                const auto evaluated = runtime::evaluateValueGraph(
                    plan.valueOperations(), plan.valueOutputCount(), time,
                    composition->format().frameRate(),
                    {plan.scalarCurves(), plan.vec2Curves(), plan.color4Curves()}, memoization);
                for (auto id : parameters) {
                    const auto* parameter = composition->parameters().find(id);
                    const auto* driver =
                        parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                                  : nullptr;
                    if (!driver)
                        continue;
                    const auto* node = composition->graph().findNode(driver->sourceNodeId);
                    const auto* definition = node ? document::builtInNodeDefinitions().find(
                                                        node->typeId, node->schemaVersion)
                                                  : nullptr;
                    if (!definition)
                        continue;
                    const auto output = std::ranges::find(definition->outputs, driver->outputPort,
                                                          &document::OutputPortDefinition::name);
                    if (output == definition->outputs.end())
                        continue;
                    for (const auto& operation : plan.valueOperations()) {
                        if (operation.sourceNodeId != node->id)
                            continue;
                        const auto index =
                            operation.firstOutput.value() +
                            static_cast<std::size_t>(output - definition->outputs.begin());
                        if (index < evaluated.outputs.size())
                            (*values)[id] = valueText(
                                promotedValue(evaluated.outputs[index], parameter->schemaKey));
                    }
                    for (const auto& diagnostic : evaluated.diagnostics)
                        if (diagnostic.nodeId == driver->sourceNodeId)
                            (*values)[id] +=
                                QString(" · %1").arg(QString::fromStdString(diagnostic.summary));
                }
            } else if (!compiled.diagnostics.empty()) {
                for (auto& [id, text] : *values) {
                    (void)id;
                    text = QString::fromStdString(compiled.diagnostics.front().summary);
                }
            }
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<Values>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<Values>>::succeeded(std::move(values));
        });
    task_ = submission.handle;
    active_ = submission.accepted();
    if (active_)
        timer_->start();
}
void DrivenValueResolver::poll() {
    if (auto result = task_.tryTakeResult()) {
        active_ = false;
        if (activeGeneration_ == generation_) {
            if (ready && result->value())
                ready(**result->value());
            else if (ready) {
                Values unavailable;
                const auto message =
                    result->diagnostics().empty()
                        ? tr("Unavailable")
                        : QString::fromStdString(result->diagnostics().front().summary);
                for (auto id : parameters_)
                    unavailable[id] = message;
                ready(unavailable);
            }
            timer_->stop();
        } else
            start();
    }
}
} // namespace bloom::ui
