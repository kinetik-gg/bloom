#include "server.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bloom::mcp {
namespace {
constexpr std::array<std::string_view, 9> kKinds{"bool", "int",    "float", "str",  "vec2",
                                                 "vec3", "color4", "id",    "array"};

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
    throw InvalidInput("Operation arguments require scalars or arrays");
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
    for (const auto& failure : result.operationFailures) {
        auto* detail = out.object();
        out.set(detail, "code", out.text("bloom.scripting.command-rejected"));
        out.set(detail, "operation", out.text(failure.operationType));
        out.set(detail, "message", out.text(failure.issue.message));
        out.append(diagnostics, detail);
    }
    if (!result.succeeded() && result.operationFailures.empty()) {
        auto* detail = out.object();
        out.set(detail, "code",
                out.text(result.status == commands::CommandStatus::StaleRevision
                             ? "bloom.scripting.stale-revision"
                             : "bloom.scripting.command-rejected"));
        out.set(detail, "message", out.text("The host refused the transaction"));
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

yyjson_mut_val* Server::transact(Json& out, yyjson_val* arguments) {
    members(arguments, {"expectedRevision", "operations", "label"},
            {"expectedRevision", "operations"});
    const auto revision = integer(member(arguments, "expectedRevision"));
    const auto label =
        member(arguments, "label") ? string(member(arguments, "label")) : "MCP transaction";
    auto* operations = member(arguments, "operations");
    if (!yyjson_is_arr(operations) || yyjson_arr_size(operations) == 0)
        throw InvalidInput("Expected operations");
    commands::Transaction transaction(label, document::Revision::fromRaw(revision));
    yyjson_arr_iter iterator = yyjson_arr_iter_with(operations);
    while (auto* operation = yyjson_arr_iter_next(&iterator)) {
        members(operation, {"op", "args"}, {"op", "args"});
        const auto id = string(member(operation, "op"), 128);
        const auto* descriptor = facade_.operations.find(id);
        if (!descriptor)
            throw InvalidInput("Unknown operation ID");
        auto* args = member(operation, "args");
        if (!yyjson_is_obj(args))
            throw InvalidInput("Expected an argument object");
        scripting::Arguments values;
        yyjson_obj_iter argumentsIterator = yyjson_obj_iter_with(args);
        while (auto* key = yyjson_obj_iter_next(&argumentsIterator)) {
            const auto name = string(key, 128);
            const auto schema = std::ranges::find(descriptor->schema.arguments, name,
                                                  &scripting::ArgumentSchema::name);
            auto* item = yyjson_obj_iter_get_val(key);
            if (schema == descriptor->schema.arguments.end() || !matches(item, schema->kind))
                throw InvalidInput("Unknown argument or wrong type: " + name);
            // SCRIPT-0 converts these optional fields to uint32_t in its factory.
            if (id == "bloom.composition.add" &&
                (name == "frameRateNumerator" || name == "frameRateDenominator") &&
                (!yyjson_is_uint(item) || yyjson_get_uint(item) == 0 ||
                 yyjson_get_uint(item) > std::numeric_limits<std::uint32_t>::max()))
                throw InvalidInput("Frame-rate components must be in 1..4294967295");
            values.emplace(name, schema->kind == scripting::ValueKind::Id
                                     ? scripting::Value(scripting::StableId{integer(item)})
                                     : value(item));
        }
        auto created = facade_.operations.create(id, values);
        if (!created) {
            auto* result = out.object();
            out.set(result, "succeeded", out.boolean(false));
            auto* diagnostics = out.array();
            auto* diagnostic = out.object();
            out.set(diagnostic, "code", out.text(created.diagnostic()->code));
            out.set(diagnostic, "operation", out.text(created.diagnostic()->operationId));
            out.set(diagnostic, "argument", out.text(created.diagnostic()->argument));
            out.set(diagnostic, "message", out.text(created.diagnostic()->message));
            out.append(diagnostics, diagnostic);
            out.set(result, "diagnostics", diagnostics);
            return result;
        }
        if (!transaction.add(std::move(created).takeOperation()))
            throw InvalidInput("Invalid operation");
    }
    if (cancellationRequested_.load())
        throw std::runtime_error("Request cancelled");
    const auto result = session_->execute(std::move(transaction));
    if (!result.command)
        throw std::runtime_error("The session cannot edit");
    return commandResult(out, *result.command);
}

yyjson_mut_val* Server::query(Json& out, yyjson_val* arguments) {
    members(arguments, {"kind", "composition"}, {"kind"});
    const auto kind = string(member(arguments, "kind"), 32);
    if (member(arguments, "composition") && kind != "nodes" && kind != "parameters")
        throw InvalidInput("This query kind does not accept a composition");
    const auto snapshot = facade_.query.snapshot();
    auto* result = out.object();
    out.set(result, "revision", out.number(snapshot.revision().value()));
    if (kind == "project") {
        auto* project = out.object();
        out.set(project, "id", out.number(snapshot.project().id().value()));
        out.set(project, "name", out.text(snapshot.project().name()));
        out.set(result, "project", project);
        return result;
    }
    auto* records = out.array();
    if (kind == "operations") {
        for (const auto& descriptor : facade_.operations.descriptors()) {
            auto* record = out.object();
            out.set(record, "id", out.text(descriptor.schema.typeId));
            auto* schema = out.array();
            for (const auto& argument : descriptor.schema.arguments) {
                auto* item = out.object();
                out.set(item, "name", out.text(argument.name));
                out.set(item, "kind", out.text(kKinds.at(static_cast<std::size_t>(argument.kind))));
                out.set(item, "required", out.boolean(argument.required));
                out.append(schema, item);
            }
            out.set(record, "arguments", schema);
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
        for (const auto& asset : facade_.query.assets()) {
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
