#include "native.hpp"
#include <bloom/host/project_color_choice.hpp>
#include <nanobind/stl/optional.h>

#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bloom::scripting::python {
namespace {

std::shared_ptr<Session> makeSession(const std::string& project) {
    const nb::gil_scoped_release release;
    auto result = project.empty() ? Session::createNew() : Session::open(project);
    if (!result) {
        throw std::runtime_error(result.diagnostic().message);
    }
    return std::move(result).takeSession();
}

nb::dict failure(const OperationDiagnostic& diagnostic) {
    nb::dict row;
    row["succeeded"] = false;
    nb::dict detail;
    detail["code"] = diagnostic.code;
    detail["operation_id"] = diagnostic.operationId;
    detail["argument"] = diagnostic.argument;
    detail["message"] = diagnostic.message;
    nb::list diagnostics;
    diagnostics.append(detail);
    row["diagnostics"] = diagnostics;
    return row;
}

constexpr std::array<const char*, 7> kContextSources{"",      "project",   "composition", "time",
                                                     "layer", "selection", "layers"};

} // namespace

NativeHost::NativeHost(const std::string& project, std::shared_ptr<PythonRootRetirements> roots)
    : NativeHost(makeSession(project), {}, std::move(roots)) {}

NativeHost::NativeHost(std::shared_ptr<Session> session,
                       std::shared_ptr<host::GpuExportProvider> provider,
                       std::shared_ptr<PythonRootRetirements> roots)
    : session_(std::move(session)), rootRetirements_(std::move(roots)),
      gpuExportProvider_(std::move(provider)) {
    if (!rootRetirements_)
        throw std::invalid_argument("Python hosts require the interpreter retirement registry");
    if (!session_ || !session_->isValid()) {
        throw std::invalid_argument("A live Bloom session is required");
    }
    ownsGpuExportProvider_ = gpuExportProvider_ == nullptr;
    if (ownsGpuExportProvider_) {
        gpuExportProvider_ = rootRetirements_->sharedProvider();
    }
    facade_ = std::make_unique<Facade>(*session_);
    subscription_ = facade_->events.subscribe([this](const commands::CommandEvent& event) {
        const std::lock_guard lock(eventMutex_);
        if (pendingEvents_.size() == 1024) {
            pendingEvents_.pop_front();
            eventsDropped_ = true;
        }
        pendingEvents_.push_back(event);
    });
    // This is the final potentially failing construction step. Every created scheduler is
    // registered before its threads start; failure cleanup/joins run outside the GIL.
    const nb::gil_scoped_release release;
    retirement_ = rootRetirements_->registerHost(gpuExportProvider_, ownsGpuExportProvider_);
}

NativeHost::~NativeHost() {
    // Allocation-free GC handoff precedes provider release; no native/host/scheduler join here.
    if (retirement_)
        retirement_->release(gpuExportProvider_);
}

std::unique_lock<std::mutex> NativeHost::lockSession() const {
    // A native render/save may hold this lock while the GIL is released. Waiting readers
    // must also release the GIL so progress and cancellation remain callable.
    const nb::gil_scoped_release release;
    return std::unique_lock(sessionMutex_);
}

nb::dict NativeHost::snapshot() const {
    const auto access = lockSession();
    return projectSnapshot(facade_->query.snapshot());
}

OperationContext NativeHost::operationContext() const {
    OperationContext context;
    if (contextProvider) {
        const auto live = contextProvider();
        context.project = live.project;
        if (live.composition != 0) {
            context.composition = live.composition;
        }
        context.selection = live.selection;
        context.time = live.time;
        if (context.composition.has_value()) {
            context.layers =
                selectedLayers(facade_->query.snapshot(), *context.composition, context.selection);
        }
        context.layerGeometry = session_->layerGeometryProvider();
        context.compositionFrameRate = session_->compositionFrameRateLookup();
    } else {
        context = session_->operationContext();
    }
    // ADR 0031: align and distribute evaluate one frame; other Python threads keep running.
    if (context.layerGeometry) {
        context.layerGeometry =
            [measure = std::move(context.layerGeometry)](const document::CompositionId composition,
                                                         const core::RationalTime time) {
                const nb::gil_scoped_release release;
                return measure(composition, time);
            };
    }
    return context;
}

nb::dict NativeHost::context() const {
    const auto access = lockSession();
    if (contextProvider) {
        const auto context = contextProvider();
        nb::dict result;
        result["project"] = context.project;
        result["composition"] = context.composition ? nb::cast(context.composition) : nb::none();
        nb::list selection;
        for (const auto id : context.selection)
            selection.append(id);
        result["selection"] = nb::tuple(selection);
        nb::list layers;
        for (const auto id :
             selectedLayers(facade_->query.snapshot(), context.composition, context.selection)) {
            layers.append(id);
        }
        result["layers"] = nb::tuple(layers);
        result["time"] = nb::make_tuple(context.time.numerator(), context.time.denominator());
        return result;
    }
    const auto snapshot = facade_->query.snapshot();
    nb::dict result;
    result["project"] = snapshot.project().id().value();
    result["composition"] = snapshot.project().compositions().empty()
                                ? nb::none()
                                : nb::cast(snapshot.project().compositions().front().id().value());
    result["selection"] = nb::tuple();
    result["layers"] = nb::tuple();
    result["time"] = nb::make_tuple(0, 1);
    return result;
}

nb::list NativeHost::schemas() const {
    nb::list result;
    for (const auto& descriptor : facade_->operations.descriptors()) {
        nb::dict row;
        row["id"] = descriptor.schema.typeId;
        row["description"] = descriptor.schema.description;
        nb::list arguments;
        for (const auto& argument : descriptor.schema.arguments) {
            nb::dict value;
            value["name"] = argument.name;
            value["kind"] = std::string(valueKindName(argument.kind));
            value["required"] = argument.required;
            value["contextual"] = argument.contextual();
            value["context"] = kContextSources.at(static_cast<std::size_t>(argument.context));
            value["summary"] = argument.summary;
            value["example"] = argument.example.has_value()
                                   ? nb::cast(pythonLiteral(*argument.example))
                                   : nb::none();
            arguments.append(value);
        }
        row["arguments"] = arguments;
        row["example"] = exampleCall(descriptor.schema);
        result.append(row);
    }
    return result;
}

document::Vec2d pathPoint(const nb::handle value) {
    if (!nb::isinstance<nb::tuple>(value) && !nb::isinstance<nb::list>(value)) {
        throw nb::type_error("A path point is (x, y)");
    }
    const auto sequence = nb::borrow<nb::sequence>(value);
    if (nb::len(sequence) != 2) {
        throw nb::type_error("A path point is (x, y)");
    }
    const document::Vec2d point{nb::cast<double>(sequence[0]), nb::cast<double>(sequence[1])};
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        throw nb::value_error("Path points must be finite");
    }
    return point;
}

// ADR 0028: a path argument is the mapping bloom.data projects for a path value: {"closed": bool,
// "anchors": [{"point": (x, y), "in_handle": (x, y) or None, "out_handle": (x, y) or None}]}.
Value pathValue(const nb::dict& mapping) {
    if (nb::len(mapping) != 2 || !mapping.contains("closed") || !mapping.contains("anchors") ||
        !nb::isinstance<nb::bool_>(mapping["closed"])) {
        throw nb::type_error("A path is {'closed': bool, 'anchors': [...]}");
    }
    const auto anchors = nb::borrow<nb::sequence>(mapping["anchors"]);
    if (nb::len(anchors) > document::kMaximumPathAnchors) {
        throw nb::value_error("A path has at most 4096 anchors");
    }
    document::PathValue path;
    path.closed = nb::cast<bool>(mapping["closed"]);
    for (const auto item : anchors) {
        if (!nb::isinstance<nb::dict>(item)) {
            throw nb::type_error("A path anchor is a mapping with a point");
        }
        const auto row = nb::borrow<nb::dict>(item);
        if (!row.contains("point")) {
            throw nb::type_error("A path anchor needs a point");
        }
        document::PathAnchor anchor;
        anchor.point = pathPoint(row["point"]);
        std::optional<document::PathAnchorMode> mode;
        for (const auto [key, handle] : row) {
            const auto name = nb::cast<std::string>(key);
            if (name == "point") {
                continue;
            }
            if (name == "mode") {
                mode = nb::isinstance<nb::str>(handle)
                           ? document::pathAnchorModeFromName(nb::cast<std::string>(handle))
                           : std::nullopt;
                if (!mode) {
                    throw nb::value_error("A path anchor mode is corner, mirrored, aligned, free "
                                          "or auto");
                }
                continue;
            }
            const bool incoming = name == "in_handle" || name == "inHandle";
            if (!incoming && name != "out_handle" && name != "outHandle") {
                throw nb::type_error("A path anchor has point, in_handle, out_handle and mode");
            }
            if (!handle.is_none()) {
                (incoming ? anchor.inHandle : anchor.outHandle) = pathPoint(handle);
            }
        }
        anchor.mode = mode.value_or(document::inferPathAnchorMode(anchor));
        path.anchors.push_back(anchor);
    }
    return Value(std::move(path));
}

Value scriptValue(const nb::handle value, const std::size_t depth) {
    if (depth > 16) {
        throw nb::value_error("Argument nesting exceeds 16 levels");
    }
    if (nb::isinstance<nb::bool_>(value)) {
        return Value(nb::cast<bool>(value));
    }
    if (nb::isinstance<nb::int_>(value)) {
        return Value(nb::cast<std::int64_t>(value));
    }
    if (nb::isinstance<nb::float_>(value)) {
        const auto number = nb::cast<double>(value);
        if (!std::isfinite(number)) {
            throw nb::value_error("Arguments must be finite");
        }
        return Value(number);
    }
    if (nb::isinstance<nb::str>(value)) {
        auto text = nb::cast<std::string>(value);
        if (text.size() > std::size_t{1024} * 1024U) {
            throw nb::value_error("Argument string exceeds 1 MiB");
        }
        return Value(std::move(text));
    }
    if (nb::isinstance<nb::tuple>(value) || nb::isinstance<nb::list>(value)) {
        const auto sequence = nb::borrow<nb::sequence>(value);
        if (nb::len(sequence) > 4096) {
            throw nb::value_error("Argument array exceeds 4096 entries");
        }
        ValueArray array;
        for (const auto item : sequence) {
            array.push_back(scriptValue(item, depth + 1));
        }
        return Value(std::move(array));
    }
    if (nb::isinstance<nb::dict>(value)) {
        return pathValue(nb::borrow<nb::dict>(value));
    }
    throw nb::type_error("Expected a scalar, an array or a path of Bloom argument values");
}

nb::dict NativeHost::transact(const nb::list& operations, const std::string& label,
                              const std::uint64_t revision) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("Transactions must run on the host authoring thread");
    }
    if (operations.empty() || operations.size() > 4096 || label.size() > 4096) {
        throw nb::value_error("A transaction needs 1..4096 operations and a bounded label");
    }
    const auto access = lockSession();
    const auto context = operationContext();
    commands::Transaction transaction(label, document::Revision::fromRaw(revision));
    for (const auto item : operations) {
        const auto request = nb::cast<nb::dict>(item);
        const auto id = nb::cast<std::string>(request["op"]);
        const auto args = nb::cast<nb::dict>(request["args"]);
        const auto* descriptor = facade_->operations.find(id);
        if (descriptor == nullptr) {
            return failure({"bloom.scripting.unknown-operation", id, "", "Unknown operation ID"});
        }
        Arguments arguments;
        for (const auto [key, value] : args) {
            const auto name = nb::cast<std::string>(key);
            const auto schema =
                std::ranges::find(descriptor->schema.arguments, name, &ArgumentSchema::name);
            if (schema == descriptor->schema.arguments.end()) {
                return failure({"bloom.scripting.invalid-argument", id, name, "Unknown argument"});
            }
            auto converted = scriptValue(value);
            // SCRIPT-0 converts these optional fields to uint32_t in its factory.
            if (id == "bloom.composition.add" &&
                (name == "frameRateNumerator" || name == "frameRateDenominator")) {
                const auto* component = std::get_if<std::int64_t>(&converted.storage);
                if (!component || *component <= 0 ||
                    static_cast<std::uint64_t>(*component) >
                        std::numeric_limits<std::uint32_t>::max()) {
                    return failure({"bloom.scripting.invalid-argument", id, name,
                                    "Frame-rate components must be in 1..4294967295"});
                }
            }
            arguments.emplace(name, std::move(converted));
        }
        auto created = facade_->operations.create(id, std::move(arguments), context);
        if (!created) {
            return failure(*created.diagnostic());
        }
        if (!transaction.add(std::move(created).takeOperation())) {
            throw nb::value_error("The transaction rejected an operation");
        }
    }
    const auto result = [&] {
        const nb::gil_scoped_release release;
        return session_->execute(std::move(transaction));
    }();
    if (!result.command) {
        // A live commit the marshal withdrew names why (bloom.live.*, ADR 0034).
        return failure(result.diagnostic
                           ? *result.diagnostic
                           : OperationDiagnostic{"bloom.scripting.session-unavailable", "", "",
                                                 "Session cannot edit"});
    }
    return commandResult(*result.command);
}

nb::dict NativeHost::history(const bool redo) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("History must run on the host authoring thread");
    }
    const auto access = lockSession();
    const auto result = [&] {
        const nb::gil_scoped_release release;
        return redo ? session_->redo() : session_->undo();
    }();
    if (!result.command) {
        // A live commit the marshal withdrew names why (bloom.live.*, ADR 0034).
        return failure(result.diagnostic
                           ? *result.diagnostic
                           : OperationDiagnostic{"bloom.scripting.session-unavailable", "", "",
                                                 "Session cannot edit"});
    }
    return commandResult(*result.command);
}

void NativeHost::save(const std::string& path) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("Saving must start on the host authoring thread");
    }
    const auto access = lockSession();
    const nb::gil_scoped_release release;
    const auto result = session_->saveAs(path);
    if (!result.succeeded()) {
        throw std::runtime_error(result.message);
    }
}

nb::dict NativeHost::setColorSettings(const std::string& config,
                                      std::optional<std::string> workingSpace) {
    if (std::this_thread::get_id() != owner_) {
        throw std::runtime_error("Color settings change on the host authoring thread");
    }
    const auto access = lockSession();
    SessionColorSettingsResult result;
    {
        const nb::gil_scoped_release release;
        result = session_->setColorSettings(config, std::move(workingSpace));
    }
    if (!result.succeeded()) {
        throw std::runtime_error(result.code + ": " + result.message);
    }
    nb::dict row;
    row["code"] = result.code;
    if (result.settings) {
        row["config"] =
            std::string(host::projectColorConfigName(host::projectColorConfigOf(*result.settings)));
        row["working_space"] = result.settings->processColorSpaceId;
        row["working_space_label"] =
            std::string(host::workingColorSpaceLabel(result.settings->processColorSpaceId));
    }
    return row;
}

nb::list NativeHost::events() {
    std::deque<commands::CommandEvent> pending;
    bool dropped = false;
    {
        const std::lock_guard lock(eventMutex_);
        pending.swap(pendingEvents_);
        dropped = std::exchange(eventsDropped_, false);
    }
    nb::list result;
    if (dropped) {
        nb::dict row;
        row["kind"] = "reset";
        result.append(row);
    }
    constexpr std::array<const char*, 3> names{"revision", "history", "rejected"};
    for (const auto& event : pending) {
        auto row = commandResult(event.result);
        row["kind"] = names.at(static_cast<std::size_t>(event.kind));
        result.append(row);
    }
    return result;
}

nb::list NativeHost::tasks() const {
    auto call = retirement_->acquire();
    nb::list result;
    for (const auto& task : call.scheduler().snapshots()) {
        nb::dict row;
        row["id"] = task.id.value();
        row["name"] = task.name;
        row["state"] = static_cast<int>(task.state);
        row["completed"] = task.progress.completed;
        row["total"] = task.progress.total ? nb::cast(*task.progress.total) : nb::none();
        row["phase"] = task.progress.phase;
        row["cancellation_requested"] = task.cancellationRequested;
        result.append(row);
    }
    return result;
}

bool NativeHost::cancel(const std::uint64_t id) {
    auto call = retirement_->acquire();
    return call.scheduler().cancel(runtime::TaskId::fromRaw(id));
}

void NativeHost::requestCancellation() {
    ++cancellationEpoch_;
    cancelled_.store(true);
    auto call = retirement_->tryAcquire();
    if (!call)
        return; // Closing already cancels the scheduler; never dereference a collected handle.
    for (const auto& task : call->scheduler().snapshots()) {
        if (!runtime::isTerminal(task.state))
            static_cast<void>(call->scheduler().cancel(task.id));
    }
}

} // namespace bloom::scripting::python
