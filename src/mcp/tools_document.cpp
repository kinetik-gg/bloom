// The document tools both hosts share: query, transact, history and events (moved from
// apps/bloom-mcp/tools.cpp and server.cpp).
#include "core_internal.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/scripting/command_diagnostics.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace bloom::mcp {
namespace {
constexpr std::array<std::string_view, 7> kContextSources{
    "", "project", "composition", "time", "layer", "selection", "layers"};
constexpr std::size_t kParameterPageSize = 64;
// Even a control-heavy string expands by at most 6x in JSON and 2x in the MCP text copy.
// 128 KiB therefore stays below the 4 MiB response cap with both representations present.
constexpr std::size_t kParameterStringChunkBytes = std::size_t{128} * 1024;

std::string parameterCursor(const std::uint64_t session, const std::uint64_t project,
                            const std::uint64_t composition, const std::uint64_t revision,
                            const std::size_t offset) {
    return "p1:" + std::to_string(session) + ':' + std::to_string(project) + ':' +
           std::to_string(composition) + ':' + std::to_string(revision) + ':' +
           std::to_string(offset);
}

std::size_t parseParameterCursor(const std::string_view cursor, const std::uint64_t session,
                                 const std::uint64_t project, const std::uint64_t composition,
                                 const std::uint64_t revision, const std::size_t count) {
    if (!cursor.starts_with("p1:"))
        throw InvalidInput("Malformed parameter cursor");
    std::array<std::uint64_t, 5> values{};
    auto remaining = cursor.substr(3);
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto separator = remaining.find(':');
        if ((index == values.size() - 1) != (separator == std::string_view::npos))
            throw InvalidInput("Malformed parameter cursor");
        const auto part = remaining.substr(0, separator);
        if (part.empty() || (part.size() > 1 && part.front() == '0'))
            throw InvalidInput("Malformed parameter cursor");
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), values[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
            throw InvalidInput("Malformed parameter cursor");
        if (index < values.size() - 1)
            remaining.remove_prefix(separator + 1);
    }
    if (values[0] != session || values[1] != project || values[2] != composition ||
        values[3] != revision)
        throw InvalidInput("Stale parameter cursor");
    if (values[4] >= count)
        throw InvalidInput("Parameter cursor is out of range");
    return static_cast<std::size_t>(values[4]);
}

document::Vec2d pathPoint(yyjson_val* item) {
    if (!yyjson_is_arr(item) || yyjson_arr_size(item) != 2 ||
        !yyjson_is_num(yyjson_arr_get(item, 0)) || !yyjson_is_num(yyjson_arr_get(item, 1)))
        throw InvalidInput("A path point is two finite numbers");
    const document::Vec2d point{yyjson_get_num(yyjson_arr_get(item, 0)),
                                yyjson_get_num(yyjson_arr_get(item, 1))};
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
        throw InvalidInput("A path point is two finite numbers");
    return point;
}

// ADR 0028: a path argument has the shape the parameter projection publishes: {"closed": bool,
// "anchors": [{"point": [x, y], "inHandle": [x, y] | null, "outHandle": [x, y] | null}]}.
document::PathValue pathValue(yyjson_val* item) {
    auto* closed = yyjson_obj_get(item, "closed");
    auto* anchors = yyjson_obj_get(item, "anchors");
    if (yyjson_obj_size(item) != 2 || !yyjson_is_bool(closed) || !yyjson_is_arr(anchors) ||
        yyjson_arr_size(anchors) > document::kMaximumPathAnchors)
        throw InvalidInput("A path is {closed, anchors}");
    document::PathValue path;
    path.closed = yyjson_get_bool(closed);
    yyjson_arr_iter rows = yyjson_arr_iter_with(anchors);
    while (auto* row = yyjson_arr_iter_next(&rows)) {
        if (!yyjson_is_obj(row))
            throw InvalidInput("A path anchor is an object");
        document::PathAnchor anchor;
        bool hasPoint = false;
        std::optional<document::PathAnchorMode> mode;
        yyjson_obj_iter fields = yyjson_obj_iter_with(row);
        while (auto* key = yyjson_obj_iter_next(&fields)) {
            const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
            auto* field = yyjson_obj_iter_get_val(key);
            if (name == "point") {
                anchor.point = pathPoint(field);
                hasPoint = true;
            } else if (name == "inHandle" || name == "outHandle") {
                if (!yyjson_is_null(field))
                    (name == "inHandle" ? anchor.inHandle : anchor.outHandle) = pathPoint(field);
            } else if (name == "mode") {
                mode = yyjson_is_str(field) ? document::pathAnchorModeFromName(std::string_view(
                                                  yyjson_get_str(field), yyjson_get_len(field)))
                                            : std::nullopt;
                if (!mode)
                    throw InvalidInput("A path anchor mode is corner, mirrored, aligned, free or "
                                       "auto");
            } else {
                throw InvalidInput("A path anchor has point, inHandle, outHandle and mode");
            }
        }
        if (!hasPoint)
            throw InvalidInput("A path anchor needs a point");
        anchor.mode = mode.value_or(document::inferPathAnchorMode(anchor));
        path.anchors.push_back(anchor);
    }
    return path;
}

scripting::Value value(yyjson_val* item, const std::size_t depth = 0) {
    if (depth > 16)
        throw InvalidInput("Argument exceeds nesting limit");
    if (yyjson_is_bool(item))
        return scripting::Value(yyjson_get_bool(item));
    if (yyjson_is_sint(item))
        return scripting::Value(yyjson_get_sint(item));
    if (yyjson_is_uint(item)) {
        const auto number = yyjson_get_uint(item);
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw InvalidInput("Integer exceeds signed 64-bit range");
        return scripting::Value(static_cast<std::int64_t>(number));
    }
    if (yyjson_is_real(item)) {
        const auto number = yyjson_get_real(item);
        if (!std::isfinite(number))
            throw InvalidInput("Number must be finite");
        return scripting::Value(number);
    }
    if (yyjson_is_str(item))
        return scripting::Value(string(item, kRequestLimit));
    if (yyjson_is_arr(item)) {
        scripting::ValueArray values;
        yyjson_arr_iter iterator = yyjson_arr_iter_with(item);
        while (auto* child = yyjson_arr_iter_next(&iterator))
            values.push_back(value(child, depth + 1));
        return scripting::Value(std::move(values));
    }
    if (yyjson_is_obj(item))
        return scripting::Value(pathValue(item));
    throw InvalidInput("Operation arguments require scalars, arrays or paths");
}

bool matches(yyjson_val* value, const scripting::ValueKind kind) {
    using Kind = scripting::ValueKind;
    switch (kind) {
    case Kind::Boolean:
        return yyjson_is_bool(value);
    case Kind::Integer:
        return yyjson_is_int(value);
    case Kind::Double:
        return yyjson_is_num(value);
    case Kind::String:
        return yyjson_is_str(value);
    case Kind::Id:
        return yyjson_is_uint(value) && yyjson_get_uint(value) != 0;
    case Kind::Array:
        return yyjson_is_arr(value);
    case Kind::Path:
        return yyjson_is_obj(value);
    case Kind::Time:
        return yyjson_is_int(value) || (yyjson_is_arr(value) && yyjson_arr_size(value) == 2 &&
                                        yyjson_is_int(yyjson_arr_get_first(value)));
    case Kind::Value:
        // A number, a vector or colour, true/false for a Boolean parameter, or text for a String
        // parameter or an enumeration member named by its label ("screen").
        return yyjson_is_num(value) || yyjson_is_bool(value) || yyjson_is_str(value) ||
               (yyjson_is_arr(value) && yyjson_arr_size(value) >= 2 && yyjson_arr_size(value) <= 4);
    case Kind::Vec2:
    case Kind::Vec3:
    case Kind::Color4: {
        const std::size_t count = kind == Kind::Vec2 ? 2 : kind == Kind::Vec3 ? 3 : 4;
        if (!yyjson_is_arr(value) || yyjson_arr_size(value) != count)
            return false;
        yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
        while (auto* item = yyjson_arr_iter_next(&iterator)) {
            if (!yyjson_is_num(item))
                return false;
        }
        return true;
    }
    }
    return false;
}
} // namespace

yyjson_mut_val* commandResult(Json& out, const commands::CommandResult& result) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(result.succeeded()));
    out.set(value, "revision", out.number(result.afterRevision.value()));
    auto* diagnostics = out.array();
    for (const auto& diagnostic : scripting::commandDiagnostics(result)) {
        auto* detail = out.object();
        out.set(detail, "code", out.text(diagnostic.code));
        if (diagnostic.severity != "error")
            out.set(detail, "severity", out.text(diagnostic.severity));
        out.set(detail, "operationIndex",
                diagnostic.operationIndex ? out.number(*diagnostic.operationIndex) : out.null());
        if (!diagnostic.operation.empty())
            out.set(detail, "operation", out.text(diagnostic.operation));
        if (!diagnostic.path.empty())
            out.set(detail, "path", out.text(diagnostic.path));
        out.set(detail, "message", out.text(diagnostic.message));
        out.append(diagnostics, detail);
    }
    out.set(value, "diagnostics", diagnostics);
    auto* outputs = out.array();
    for (const auto& output : result.outputs) {
        auto* record = out.object();
        out.set(record, "operationIndex", out.number(output.operationIndex));
        out.set(record, "name", out.text(output.output.name));
        out.set(
            record, "id",
            out.number(std::visit([](const auto& id) { return id.value(); }, output.output.id)));
        out.append(outputs, record);
    }
    out.set(value, "outputs", outputs);
    return value;
}

namespace {

// The live host's refusal codes, as MCP tool errors.
void throwLiveRefusal(const scripting::SessionCommandResult& result) {
    if (!result.diagnostic)
        return;
    const auto& code = result.diagnostic->code;
    using scripting::LiveCommitStatus;
    const auto is = [&](const LiveCommitStatus status) {
        return code == scripting::liveCommitDiagnosticCode(status);
    };
    if (is(LiveCommitStatus::QueueFull))
        throw ToolFailure(McpToolErrorCode::CommitQueueFull,
                          "Bloom has too many edits waiting; nothing was queued.");
    if (is(LiveCommitStatus::ProjectReplaced))
        throw ToolFailure(McpToolErrorCode::ProjectReplaced,
                          "The project was replaced; nothing was applied.");
    if (is(LiveCommitStatus::Cancelled) || is(LiveCommitStatus::ShuttingDown))
        throw ToolFailure(McpToolErrorCode::Cancelled,
                          "The request was cancelled; nothing was applied.");
    if (is(LiveCommitStatus::DeadlineExceeded))
        throw ToolFailure(McpToolErrorCode::DeadlineExceeded,
                          "Bloom was busy past the request's deadline; nothing was applied.");
    if (is(LiveCommitStatus::GestureOpen))
        throw ToolFailure(McpToolErrorCode::GestureOpen,
                          "The artist kept an edit open past the deadline; nothing was applied.");
}

// A command the session could not run at all.
void throwUnavailable(const McpCall& call, const scripting::SessionCommandResult& result) {
    throwLiveRefusal(result);
    if (call.lease != nullptr && call.lease->revoked())
        throw ToolFailure(McpToolErrorCode::ProjectReplaced,
                          "The project was replaced; nothing was applied.");
    if (result.status == host::ProjectSessionCommandStatus::ReadOnly)
        throw ToolFailure(McpToolErrorCode::ReadOnly, "The project is read-only.");
    throw std::runtime_error("The session cannot edit");
}

// After a successful edit, every edited composition is compiled once (never rendered) so an edit
// that leaves one uncompilable is reported with the edit instead of surfacing later as a failed
// preview or export: compileFailures in the result, and a composition.compile-failed event.
yyjson_mut_val* compileFailures(McpServerCore::State& core, Json& out, McpCall& call,
                                const commands::CommandResult& command) {
    auto* failures = out.array();
    const auto snapshot = call.session.snapshot();
    std::vector<document::CompositionId> edited;
    if (command.affectedComposition) {
        edited.push_back(*command.affectedComposition);
    } else {
        for (const auto& composition : snapshot.project().compositions())
            edited.push_back(composition.id());
    }
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    for (const auto composition : edited) {
        if (call.control.stop.stop_requested() ||
            snapshot.project().findComposition(composition) == nullptr)
            continue;
        const auto compiled =
            compiler.compile({.snapshot = snapshot, .compositionId = composition}, {});
        const auto failure = runtime::compileFailureSummary(compiled, composition);
        if (!failure)
            continue;
        auto* record = out.object();
        out.set(record, "composition", out.number(composition.value()));
        out.set(record, "code", out.text(failure->code));
        out.set(record, "detail", out.text(failure->detail));
        auto* subject = out.object();
        out.set(subject, "composition", out.number(failure->compositionId.value()));
        out.set(subject, "node",
                failure->nodeId ? out.number(failure->nodeId->value()) : out.null());
        out.set(subject, "parameter",
                failure->parameterId ? out.number(failure->parameterId->value()) : out.null());
        out.set(record, "subject", subject);
        out.append(failures, record);
        call.compileFailures.push_back(
            {.composition = failure->compositionId.value(),
             .detail = failure->detail,
             .node = failure->nodeId ? std::optional(failure->nodeId->value()) : std::nullopt,
             .parameter =
                 failure->parameterId ? std::optional(failure->parameterId->value()) : std::nullopt});
        core.notice(
            {.kind = "composition.compile-failed",
             .message = failure->detail,
             .revision = command.afterRevision.value(),
             .composition = failure->compositionId.value(),
             .node = failure->nodeId ? std::optional(failure->nodeId->value()) : std::nullopt,
             .parameter =
                 failure->parameterId ? std::optional(failure->parameterId->value()) : std::nullopt,
             .path = {},
             .projectName = {}});
    }
    return failures;
}

} // namespace

yyjson_mut_val* transactTool(McpServerCore::State& core, Json& out, McpCall& call,
                             yyjson_val* arguments) {
    members(arguments, {"expectedRevision", "operations", "label"},
            {"expectedRevision", "operations"});
    const auto revision = integer(member(arguments, "expectedRevision"));
    const auto label = member(arguments, "label") ? string(member(arguments, "label"))
                       : call.live != nullptr     ? "Agent edit"
                                                  : "MCP transaction";
    auto* operations = member(arguments, "operations");
    if (!yyjson_is_arr(operations) || yyjson_arr_size(operations) == 0)
        throw InvalidInput("Expected operations");
    commands::Transaction transaction(label, document::Revision::fromRaw(revision));
    const auto& context = call.context;
    const auto& registry = call.session.operations();
    std::string operationIds;
    std::size_t operationIndex = 0;
    yyjson_arr_iter iterator = yyjson_arr_iter_with(operations);
    for (auto* operation = yyjson_arr_iter_next(&iterator); operation != nullptr;
         operation = yyjson_arr_iter_next(&iterator), ++operationIndex) {
        // Every argument refusal names the operation's place in the list, so an agent knows which
        // entry to fix.
        const auto at = "operations[" + std::to_string(operationIndex) + "]";
        if (!yyjson_is_obj(operation) || !member(operation, "op") || !member(operation, "args"))
            throw InvalidInput(at + ": each operation is {\"op\": <id>, \"args\": {...}}");
        members(operation, {"op", "args"}, {"op", "args"});
        const auto id = string(member(operation, "op"), 128);
        const auto* descriptor = registry.find(id);
        if (operationIds.size() < 200)
            operationIds += (operationIds.empty() ? "" : ",") + id;
        if (!descriptor)
            throw InvalidInput(at + ": unknown operation \"" + id +
                               "\"; query {\"kind\":\"operations\"} lists them");
        auto* args = member(operation, "args");
        if (!yyjson_is_obj(args))
            throw InvalidInput(at + " (" + id + "): args must be an object");
        scripting::Arguments values;
        yyjson_obj_iter argumentsIterator = yyjson_obj_iter_with(args);
        while (auto* key = yyjson_obj_iter_next(&argumentsIterator)) {
            const auto name = string(key, 128);
            const auto schema = std::ranges::find(descriptor->schema.arguments, name,
                                                  &scripting::ArgumentSchema::name);
            auto* item = yyjson_obj_iter_get_val(key);
            if (schema == descriptor->schema.arguments.end())
                throw InvalidInput(at + " (" + id + "): unknown argument \"" + name +
                                   "\"; the example is " +
                                   scripting::exampleCall(descriptor->schema));
            if (!matches(item, schema->kind))
                throw InvalidInput(at + " (" + id + "): argument \"" + name + "\" expects " +
                                   std::string(scripting::valueKindName(schema->kind)) +
                                   "; the example is " +
                                   scripting::exampleCall(descriptor->schema));
            // SCRIPT-0 converts these optional fields to uint32_t in its factory.
            if (id == "bloom.composition.add" &&
                (name == "frameRateNumerator" || name == "frameRateDenominator") &&
                (!yyjson_is_uint(item) || yyjson_get_uint(item) == 0 ||
                 yyjson_get_uint(item) > std::numeric_limits<std::uint32_t>::max()))
                throw InvalidInput(at + " (" + id +
                                   "): frame-rate components must be in 1..4294967295");
            values.emplace(name, schema->kind == scripting::ValueKind::Id
                                     ? scripting::Value(scripting::StableId{integer(item)})
                                     : value(item));
        }
        auto created = registry.create(id, std::move(values), context);
        if (!created) {
            auto* result = out.object();
            out.set(result, "succeeded", out.boolean(false));
            auto* diagnostics = out.array();
            auto* diagnostic = out.object();
            out.set(diagnostic, "code", out.text(created.diagnostic()->code));
            out.set(diagnostic, "operationIndex", out.number(operationIndex));
            out.set(diagnostic, "operation", out.text(created.diagnostic()->operationId));
            out.set(diagnostic, "argument", out.text(created.diagnostic()->argument));
            out.set(diagnostic, "message", out.text(created.diagnostic()->message));
            out.append(diagnostics, diagnostic);
            out.set(result, "diagnostics", diagnostics);
            return result;
        }
        if (!transaction.add(std::move(created).takeOperation()))
            throw InvalidInput(at + " (" + id + "): the operation could not be added");
    }
    call.summary = operationIds + " \"" + std::string(label.substr(0, 64)) + "\"";
    if (call.control.stop.stop_requested())
        throw std::runtime_error("Request cancelled");
    const auto result = call.session.execute(std::move(transaction));
    if (!result.command)
        throwUnavailable(call, result);
    auto* value = commandResult(out, *result.command);
    out.set(value, "compileFailures",
            result.command->changed() && result.command->renderAffecting
                ? compileFailures(core, out, call, *result.command)
                : out.array());
    return value;
}

yyjson_mut_val* historyTool(McpServerCore::State&, Json& out, McpCall& call,
                            yyjson_val* arguments) {
    members(arguments, {"action", "expectedRevision"}, {"action", "expectedRevision"});
    const auto action = string(member(arguments, "action"), 8);
    if (action != "undo" && action != "redo")
        throw InvalidInput("History action is undo or redo");
    const bool redo = action == "redo";
    const auto expected =
        document::Revision::fromRaw(integer(member(arguments, "expectedRevision")));
    call.summary = action;
    scripting::SessionCommandResult result;
    if (call.lease != nullptr) {
        result = call.lease->history(redo, expected);
    } else if (call.session.snapshot().revision() != expected) {
        commands::CommandResult stale;
        stale.action = redo ? commands::CommandAction::Redo : commands::CommandAction::Undo;
        stale.status = commands::CommandStatus::StaleRevision;
        stale.beforeRevision = call.session.snapshot().revision();
        stale.afterRevision = stale.beforeRevision;
        stale.expectedRevision = expected;
        return commandResult(out, stale);
    } else {
        result = redo ? call.session.redo() : call.session.undo();
    }
    if (!result.command)
        throwUnavailable(call, result);
    return commandResult(out, *result.command);
}

yyjson_mut_val* eventsTool(McpServerCore::State& core, Json& out, McpCall& call,
                           yyjson_val* arguments) {
    members(arguments, {"sinceRevision", "afterSequence"}, {"sinceRevision"});
    const auto since = integer(member(arguments, "sinceRevision"));
    const auto after =
        member(arguments, "afterSequence") ? integer(member(arguments, "afterSequence")) : 0;
    const auto revision = call.session.snapshot().revision().value();
    const std::lock_guard lock(core.eventsMutex);
    // A cursor from before the live project was replaced must start again.
    const bool replaced = core.replaced && after < core.resetSequence;
    if (!replaced && (since > revision || after > core.sequence))
        throw InvalidInput("Event cursor is in the future");
    auto* result = out.object();
    out.set(result, "revision", out.number(revision));
    out.set(result, "sequence", out.number(core.sequence));
    const auto& events = core.events;
    const auto revisionOf = [](const McpEventEntry& entry) {
        if (const auto* command = std::get_if<commands::CommandEvent>(&entry))
            return command->result.afterRevision.value();
        return std::get<McpNotice>(entry).revision;
    };
    out.set(result, "resetRequired",
            out.boolean(replaced || (!events.empty() && events.front().first > 1 &&
                                     after < events.front().first - 1 &&
                                     since <= revisionOf(events.front().second))));
    auto* list = out.array();
    constexpr std::array<std::string_view, 3> names{"revision", "history", "rejected"};
    for (const auto& [sequence, entry] : events) {
        if ((!replaced && sequence <= after) || (!replaced && revisionOf(entry) < since))
            continue;
        yyjson_mut_val* value = nullptr;
        if (const auto* event = std::get_if<commands::CommandEvent>(&entry)) {
            value = commandResult(out, event->result);
            out.set(value, "kind", out.text(names.at(static_cast<std::size_t>(event->kind))));
        } else {
            const auto& notice = std::get<McpNotice>(entry);
            value = noticeValue(out, notice);
        }
        out.set(value, "sequence", out.number(sequence));
        out.append(list, value);
    }
    out.set(result, "events", list);
    return result;
}

namespace {
// query kind "context": the live editor context (or the headless session defaults).
yyjson_mut_val* contextProjection(Json& out, McpCall& call, const document::Snapshot& snapshot) {
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    out.set(result, "live", out.boolean(call.live != nullptr));
    auto* project = out.object();
    out.set(project, "id", out.number(snapshot.project().id().value()));
    out.set(project, "name", out.text(snapshot.project().name()));
    out.set(result, "project", project);
    const auto& context = call.context;
    out.set(result, "composition",
            context.composition ? out.number(*context.composition) : out.null());
    auto* selection = out.array();
    for (const auto node : context.selection)
        out.append(selection, out.number(node));
    out.set(result, "selectedNodes", selection);
    auto* layers = out.array();
    for (const auto layer : context.layers)
        out.append(layers, out.number(layer));
    out.set(result, "selectedLayers", layers);
    const auto time = context.time.value_or(core::RationalTime::fromInteger(0));
    auto* timeValue = out.array();
    out.append(timeValue, out.signedNumber(time.numerator()));
    out.append(timeValue, out.signedNumber(time.denominator()));
    out.set(result, "time", timeValue);
    // The Viewer's display and view; empty means the project's default display and view.
    out.set(result, "display", out.text(call.live ? call.live->viewerDisplay : std::string{}));
    out.set(result, "view", out.text(call.live ? call.live->viewerView : std::string{}));
    out.set(result, "readOnly",
            out.boolean(call.live ? call.live->readOnly
                                  : call.session.state().contentKind ==
                                        host::ProjectSessionContentKind::PreservedReadOnly));
    return result;
}
} // namespace

yyjson_mut_val* queryTool(McpServerCore::State& core, Json& out, McpCall& call,
                          yyjson_val* arguments) {
    const auto sessionToken = core.cursorToken ^ (call.generation * 0x9e3779b97f4a7c15ULL);
    members(arguments,
            {"kind", "composition", "pageSize", "cursor", "parameter", "expectedRevision", "offset",
             "limit"},
            {"kind"});
    const auto kind = string(member(arguments, "kind"), 32);
    const bool parameterPage =
        kind == "parameters" && (member(arguments, "pageSize") || member(arguments, "cursor"));
    const bool individualParameter = kind == "parameter" || kind == "parameter-value";
    if (member(arguments, "composition") && kind != "nodes" && kind != "parameters" &&
        !individualParameter)
        throw InvalidInput("This query kind does not accept a composition");
    if ((member(arguments, "pageSize") || member(arguments, "cursor")) && kind != "parameters")
        throw InvalidInput("Paging applies only to parameters");
    if (member(arguments, "parameter") != nullptr && !individualParameter)
        throw InvalidInput("A parameter ID requires an individual parameter query");
    if ((member(arguments, "expectedRevision") || member(arguments, "offset") ||
         member(arguments, "limit")) &&
        !individualParameter)
        throw InvalidInput("Unexpected parameter fetch option");
    const auto snapshot = call.session.snapshot();
    if (kind == "context")
        return contextProjection(out, call, snapshot);
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    if (kind == "project") {
        auto* project = out.object();
        out.set(project, "id", out.number(snapshot.project().id().value()));
        out.set(project, "name", out.text(snapshot.project().name()));
        out.set(result, "project", project);
        return result;
    }
    if (individualParameter &&
        (!member(arguments, "composition") || !member(arguments, "parameter")))
        throw InvalidInput("Individual parameter queries require composition and parameter IDs");
    if (individualParameter && member(arguments, "expectedRevision") &&
        integer(member(arguments, "expectedRevision")) != snapshot.revision().value())
        throw InvalidInput("Stale parameter revision");
    if (kind == "parameter-value" && !member(arguments, "expectedRevision"))
        throw InvalidInput("String chunks require expectedRevision");
    if (kind == "parameter" && (member(arguments, "offset") || member(arguments, "limit")))
        throw InvalidInput("String chunk options require parameter-value");
    if (individualParameter) {
        const auto compositionId = integer(member(arguments, "composition"));
        const auto* composition =
            snapshot.project().findComposition(document::CompositionId::fromRaw(compositionId));
        if (!composition)
            throw InvalidInput("Stale composition ID at revision " +
                               std::to_string(snapshot.revision().value()));
        const auto parameterId = integer(member(arguments, "parameter"));
        const auto* parameter =
            composition->parameters().find(document::ParameterId::fromRaw(parameterId));
        if (!parameter)
            throw InvalidInput("Stale parameter ID at revision " +
                               std::to_string(snapshot.revision().value()));
        out.set(result, "composition", out.number(compositionId));
        out.set(result, "parameter", out.number(parameterId));
        if (kind == "parameter") {
            const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
            const auto* text = constant ? std::get_if<std::string>(&constant->value) : nullptr;
            out.set(result, "record",
                    text && text->size() > kParameterStringChunkBytes
                        ? parameterSummary(out, *parameter)
                        : parameterRecord(out, *parameter));
            return result;
        }
        const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
        const auto* text = constant ? std::get_if<std::string>(&constant->value) : nullptr;
        if (!text)
            throw InvalidInput("Parameter is not a constant string");
        const auto offset = member(arguments, "offset") ? integer(member(arguments, "offset")) : 0;
        const auto limit = member(arguments, "limit") ? integer(member(arguments, "limit"))
                                                      : kParameterStringChunkBytes;
        if (limit < 4 || limit > kParameterStringChunkBytes || offset > text->size() ||
            (offset < text->size() &&
             (static_cast<unsigned char>((*text)[offset]) & 0xc0U) == 0x80U))
            throw InvalidInput("Invalid UTF-8 string chunk range");
        auto end = static_cast<std::size_t>(offset) +
                   std::min<std::size_t>(static_cast<std::size_t>(limit), text->size() - offset);
        while (end < text->size() && (static_cast<unsigned char>((*text)[end]) & 0xc0U) == 0x80U)
            --end;
        out.set(result, "offset", out.number(offset));
        out.set(result, "nextOffset", out.number(end));
        out.set(result, "totalBytes", out.number(text->size()));
        out.set(result, "complete", out.boolean(end == text->size()));
        out.set(result, "chunk", out.text(std::string_view(*text).substr(offset, end - offset)));
        return result;
    }
    auto* records = out.array();
    if (kind == "operations") {
        for (const auto& descriptor : call.session.operations().descriptors()) {
            auto* record = out.object();
            out.set(record, "id", out.text(descriptor.schema.typeId));
            out.set(record, "description", out.text(descriptor.schema.description));
            auto* schema = out.array();
            for (const auto& argument : descriptor.schema.arguments) {
                auto* item = out.object();
                out.set(item, "name", out.text(argument.name));
                out.set(item, "kind", out.text(scripting::valueKindName(argument.kind)));
                out.set(item, "required", out.boolean(argument.required));
                out.set(item, "contextual", out.boolean(argument.contextual()));
                out.set(item, "context",
                        out.text(kContextSources.at(static_cast<std::size_t>(argument.context))));
                out.set(item, "summary", out.text(argument.summary));
                out.append(schema, item);
            }
            out.set(record, "arguments", schema);
            out.set(record, "example", out.text(scripting::exampleCall(descriptor.schema)));
            out.append(records, record);
        }
    } else if (kind == "compositions") {
        for (const auto& composition : snapshot.project().compositions()) {
            auto* record = out.object();
            out.set(record, "id", out.number(composition.id().value()));
            out.set(record, "name", out.text(composition.name()));
            out.set(record, "width", out.number(composition.format().width()));
            out.set(record, "height", out.number(composition.format().height()));
            out.set(record, "frameRateNumerator",
                    out.number(composition.format().frameRate().numerator()));
            out.set(record, "frameRateDenominator",
                    out.number(composition.format().frameRate().denominator()));
            out.append(records, record);
        }
    } else if (kind == "assets") {
        for (const auto& asset : snapshot.project().assets()) {
            auto* record = out.object();
            out.set(record, "id", out.number(asset.id.value()));
            out.set(record, "name", out.text(asset.name));
            out.set(record, "kind", out.number(static_cast<unsigned>(asset.kind)));
            out.set(record, "path", out.text(asset.locator.path));
            out.set(record, "width", out.number(asset.width));
            out.set(record, "height", out.number(asset.height));
            const auto digest = asset.contentDigest.toLowercaseHex();
            out.set(record, "digest",
                    out.text("sha256:" + std::string(digest.data(), digest.size())));
            out.append(records, record);
        }
    } else if (kind == "nodes" || kind == "parameters") {
        const auto id = integer(member(arguments, "composition"));
        const auto* composition =
            snapshot.project().findComposition(document::CompositionId::fromRaw(id));
        if (!composition)
            throw InvalidInput("Stale composition ID at revision " +
                               std::to_string(snapshot.revision().value()));
        if (kind == "nodes") {
            for (const auto& node : composition->graph().nodes()) {
                auto* record = out.object();
                out.set(record, "id", out.number(node.id.value()));
                out.set(record, "typeId", out.text(node.typeId));
                auto* parameters = out.object();
                for (const auto& parameter : node.parameters)
                    out.set(parameters, parameter.role, out.number(parameter.parameterId.value()));
                out.set(record, "parameters", parameters);
                out.append(records, record);
            }
        } else if (parameterPage) {
            // A summary only carries bounded structural strings (at most 256 bytes each),
            // never a value. Even worst-case JSON escaping and the MCP text copy fit 4 MiB.
            const auto pageSize = member(arguments, "pageSize")
                                      ? integer(member(arguments, "pageSize"))
                                      : kParameterPageSize;
            if (pageSize == 0 || pageSize > kParameterPageSize)
                throw InvalidInput("Parameter pageSize must be in 1..64");
            const auto parameters = composition->parameters().records();
            const auto offset =
                member(arguments, "cursor")
                    ? parseParameterCursor(string(member(arguments, "cursor"), 160), sessionToken,
                                           snapshot.project().id().value(), id,
                                           snapshot.revision().value(), parameters.size())
                    : 0;
            const auto end = offset + std::min<std::size_t>(pageSize, parameters.size() - offset);
            for (auto index = offset; index < end; ++index)
                out.append(records, parameterSummary(out, parameters[index]));
            out.set(result, "nextCursor",
                    end < parameters.size()
                        ? out.text(parameterCursor(sessionToken, snapshot.project().id().value(),
                                                   id, snapshot.revision().value(), end))
                        : out.null());
            out.set(result, "totalRecords", out.number(parameters.size()));
        } else {
            for (const auto& parameter : composition->parameters().records()) {
                out.append(records, parameterRecord(out, parameter));
            }
        }
    } else
        throw InvalidInput("Unknown query kind");
    out.set(result, "records", records);
    return result;
}
} // namespace bloom::mcp
