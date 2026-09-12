#include <bloom/project/document_migration.hpp>

#include <bloom/document/node_layout.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_json_writer.hpp>

#include <array>
#include <span>
#include <string_view>

namespace bloom::project {
namespace {
using Buffer = std::pmr::vector<char>;
void append(Buffer& output, const std::string_view text) {
    output.insert(output.end(), text.begin(), text.end());
}

bool quoted(Buffer& output, const std::string_view text) {
    auto counter = CanonicalJsonWriter::counting();
    if (!counter.stringValue(text) || !counter.finish())
        return false;
    const auto offset = output.size();
    output.resize(offset + counter.bytesRequired());
    CanonicalJsonWriter writer(std::span(output).subspan(offset), {});
    return writer.stringValue(text) && writer.finish();
}

// Copy strict DOM values without normalizing number tokens: migration must not turn an invalid
// structural integer spelling into an accepted one before typed decoding has examined it.
bool copyValue(const JsonValue& value, Buffer& output) {
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
        for (const auto& element : value.arrayElements()) {
            if (!first)
                append(output, ",");
            first = false;
            if (!copyValue(element, output))
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
            if (!copyValue(member.value(), output))
                return false;
        }
        append(output, "}");
        return true;
    }
    }
    return false;
}

bool layout(const JsonValue& composition, Buffer& output) {
    const auto* graph = composition.findMember("graph");
    const auto* nodes = graph ? graph->findMember("nodes") : nullptr;
    if (!nodes || nodes->kind() != JsonValueKind::Array)
        return false;
    std::array<std::size_t, 4> rows{};
    append(output, "[");
    bool first = true;
    for (const auto& node : nodes->arrayElements()) {
        const auto* id = node.findMember("id");
        const auto* type = node.findMember("typeId");
        const auto idText = id ? id->asString() : std::nullopt;
        const auto typeText = type ? type->asString() : std::nullopt;
        if (!idText || !typeText)
            return false;
        const auto parsedId = parseCanonicalObjectId(*idText);
        if (!parsedId || *parsedId.value() == 0)
            return false;
        const auto column = document::defaultNodeLayoutColumn(*typeText);
        const auto record = document::defaultNodeLayoutRecord(column, rows.at(column)++);
        if (!first)
            append(output, ",");
        first = false;
        append(output, "{\"nodeId\":");
        if (!quoted(output, *idText))
            return false;
        append(output, ",\"position\":{\"x\":");
        const auto x = formatCanonicalFloat64(record.position.x);
        const auto y = formatCanonicalFloat64(record.position.y);
        if (!x || !y)
            return false;
        append(output, x.value()->view());
        append(output, ",\"y\":");
        append(output, y.value()->view());
        append(output, "},\"width\":128,\"collapsed\":false,\"muted\":false}");
    }
    append(output, "]");
    return true;
}

enum class Scope { Root, Project, Composition };
bool transform(const JsonValue& value, const Scope scope, Buffer& output) {
    if (value.kind() != JsonValueKind::Object ||
        (scope == Scope::Composition && value.findMember("nodeLayout")))
        return false;
    append(output, "{");
    bool first = true;
    for (const auto& member : value.objectMembers()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (scope == Scope::Root && member.key() == "schemaVersion") {
            append(output, "{\"major\":1,\"minor\":1}");
        } else if (scope == Scope::Root && member.key() == "project") {
            if (!transform(member.value(), Scope::Project, output))
                return false;
        } else if (scope == Scope::Project && member.key() == "compositions") {
            if (member.value().kind() != JsonValueKind::Array)
                return false;
            append(output, "[");
            bool firstComposition = true;
            for (const auto& composition : member.value().arrayElements()) {
                if (!firstComposition)
                    append(output, ",");
                firstComposition = false;
                if (!transform(composition, Scope::Composition, output))
                    return false;
            }
            append(output, "]");
        } else if (!copyValue(member.value(), output))
            return false;
    }
    if (scope == Scope::Composition) {
        append(output, ",\"nodeLayout\":");
        if (!layout(value, output))
            return false;
    }
    append(output, "}");
    return true;
}
} // namespace

MigrationStepOutcome migrateNodeLayoutV1_0(const JsonValue& root,
                                           std::pmr::memory_resource* /*resource*/,
                                           Buffer& output) {
    const auto* version = root.findMember("schemaVersion");
    const auto* major = version ? version->findMember("major") : nullptr;
    const auto* minor = version ? version->findMember("minor") : nullptr;
    if (!major || !minor || major->asNumberToken() != "1" || minor->asNumberToken() != "0" ||
        version->objectMembers().size() != 2)
        return MigrationStepOutcome::failure("/schemaVersion");
    if (!transform(root, Scope::Root, output))
        return MigrationStepOutcome::failure("/project/compositions");
    return MigrationStepOutcome::success();
}
} // namespace bloom::project
