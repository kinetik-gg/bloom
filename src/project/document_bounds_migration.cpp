#include <bloom/project/document_migration.hpp>

#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_json_writer.hpp>

#include <limits>
#include <string>
#include <unordered_map>

namespace bloom::project {
namespace {
using Buffer = std::pmr::vector<char>;
using Replacements = std::pmr::unordered_map<const JsonValue*, std::pmr::string>;
void append(Buffer& output, const std::string_view value) {
    output.insert(output.end(), value.begin(), value.end());
}
bool quoted(Buffer& output, const std::string_view value) {
    auto counter = CanonicalJsonWriter::counting();
    if (!counter.stringValue(value) || !counter.finish())
        return false;
    const auto start = output.size();
    output.resize(start + counter.bytesRequired());
    CanonicalJsonWriter writer(std::span(output).subspan(start), {});
    return writer.stringValue(value) && writer.finish();
}
bool copy(const JsonValue& value, const Replacements& replacements, Buffer& output) {
    if (const auto it = replacements.find(&value); it != replacements.end()) {
        append(output, it->second);
        return true;
    }
    switch (value.kind()) {
    case JsonValueKind::Null:
        append(output, "null");
        return true;
    case JsonValueKind::Boolean:
        append(output, value.asBoolean().value_or(false) ? "true" : "false");
        return true;
    case JsonValueKind::Number:
        append(output, value.asNumberToken().value_or(""));
        return true;
    case JsonValueKind::String:
        return quoted(output, value.asString().value_or(""));
    case JsonValueKind::Array: {
        append(output, "[");
        bool first = true;
        for (const auto& child : value.arrayElements()) {
            if (!first)
                append(output, ",");
            first = false;
            if (!copy(child, replacements, output))
                return false;
        }
        append(output, "]");
        return true;
    }
    case JsonValueKind::Object: {
        append(output, "{");
        bool first = true;
        for (const auto& member : value.objectMembers()) {
            if (!first)
                append(output, ",");
            first = false;
            if (!quoted(output, member.key()))
                return false;
            append(output, ":");
            if (!copy(member.value(), replacements, output))
                return false;
        }
        append(output, "}");
        return true;
    }
    }
    return false;
}
const JsonValue* member(const JsonValue* value, const std::string_view name) {
    return value ? value->findMember(name) : nullptr;
}
} // namespace

MigrationStepOutcome migrateContentBoundsV1_6(const JsonValue& root,
                                              std::pmr::memory_resource* resource, Buffer& output) {
    const auto* version = root.findMember("schemaVersion");
    const auto* major = member(version, "major");
    const auto* minor = member(version, "minor");
    if (!major || !minor || major->asNumberToken() != "1" || minor->asNumberToken() != "6" ||
        version->objectMembers().size() != 2)
        return MigrationStepOutcome::failure("/schemaVersion");
    const auto* compositions = member(root.findMember("project"), "compositions");
    const auto* highWater =
        member(member(root.findMember("idAllocation"), "highestIssued"), "parameter");
    if (!compositions || compositions->kind() != JsonValueKind::Array || !highWater ||
        !highWater->asString())
        return MigrationStepOutcome::failure("/idAllocation/highestIssued/parameter");
    const auto parsed = parseCanonicalObjectId(*highWater->asString());
    if (!parsed)
        return MigrationStepOutcome::failure("/idAllocation/highestIssued/parameter");
    auto next = *parsed.value();
    Replacements replacements(resource);
    replacements.emplace(version, "{\"major\":1,\"minor\":7}");
    for (const auto& composition : compositions->arrayElements()) {
        const auto* nodes = member(composition.findMember("graph"), "nodes");
        const auto* parameters = composition.findMember("parameters");
        const auto* width = member(composition.findMember("format"), "width");
        const auto* height = member(composition.findMember("format"), "height");
        if (!nodes || nodes->kind() != JsonValueKind::Array || !parameters ||
            parameters->kind() != JsonValueKind::Array || !width || !height ||
            !width->asNumberToken() || !height->asNumberToken())
            return MigrationStepOutcome::failure("/project/compositions");
        Buffer values(resource);
        if (!copy(*parameters, replacements, values))
            return MigrationStepOutcome::failure("/project/compositions/parameters");
        values.pop_back();
        bool any = !parameters->arrayElements().empty();
        for (const auto& node : nodes->arrayElements()) {
            const auto* type = node.findMember("typeId");
            const auto* schema = node.findMember("schemaVersion");
            if (!type || type->asString() != "bloom.solid-source" || !schema ||
                schema->asNumberToken() != "1")
                continue;
            const auto* bindings = node.findMember("parameters");
            if (!bindings || bindings->kind() != JsonValueKind::Array ||
                bindings->arrayElements().size() != 1 ||
                !member(&bindings->arrayElements().front(), "role") ||
                member(&bindings->arrayElements().front(), "role")->asString() != "color")
                return MigrationStepOutcome::failure(
                    "/project/compositions/graph/nodes/parameters");
            Buffer roles(resource);
            if (!copy(*bindings, replacements, roles))
                return MigrationStepOutcome::failure(
                    "/project/compositions/graph/nodes/parameters");
            roles.pop_back();
            // Canonical binding order is role order; parameter records are monotonically allocated.
            for (const auto role : {std::string_view{"height"}, std::string_view{"width"}}) {
                if (next == std::numeric_limits<std::uint64_t>::max())
                    return MigrationStepOutcome::failure("/idAllocation/highestIssued/parameter");
                const auto id = formatCanonicalUInt64(++next);
                append(roles, ",{\"role\":");
                if (!quoted(roles, role))
                    return MigrationStepOutcome::failure("/project/compositions");
                append(roles, ",\"parameterId\":\"");
                append(roles, id.view());
                append(roles, "\"}");
                if (any)
                    append(values, ",");
                any = true;
                append(values, "{\"id\":\"");
                append(values, id.view());
                append(values, "\",\"schemaKey\":\"bloom.solid.");
                append(values, role);
                append(values, "\",\"source\":{\"kind\":\"constant\",\"value\":{\"kind\":"
                               "\"float64\",\"value\":");
                append(values, *(role == "width" ? width : height)->asNumberToken());
                append(values, "}}}");
            }
            append(roles, "]");
            replacements.emplace(bindings, std::pmr::string(roles.begin(), roles.end(), resource));
            replacements.emplace(schema, "2");
        }
        append(values, "]");
        replacements.emplace(parameters, std::pmr::string(values.begin(), values.end(), resource));
    }
    // Legacy Text v1, Layer v3, and Merge v1 remain explicit compatibility semantics. Their
    // frame clipping and sampled pivots cannot be represented by changing constant coordinates.
    Buffer watermark(resource);
    const auto finalWatermark = formatCanonicalUInt64(next);
    if (!quoted(watermark, finalWatermark.view()))
        return MigrationStepOutcome::failure("/idAllocation");
    replacements.emplace(highWater, std::pmr::string(watermark.begin(), watermark.end(), resource));
    if (!copy(root, replacements, output))
        return MigrationStepOutcome::failure("/project/compositions");
    return MigrationStepOutcome::success();
}
} // namespace bloom::project
