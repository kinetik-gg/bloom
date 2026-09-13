#include "schema_check_helpers.hpp"

#include <algorithm>

namespace bloom::quality {
namespace {
constexpr std::string_view version11 =
    R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":1}},"unevaluatedProperties":false})";

void eraseMember(json::Value& object, const std::string_view name) {
    std::erase_if(object.asObject(), [name](const auto& member) { return member.first == name; });
}

// Check every new constraint, then reuse the complete historical checks for the unchanged
// definitions. Normalization is confined to this quality-tool copy of the schema artifact.
json::Value checkVersion11(const json::Value& schema, const std::string_view id,
                           const std::string_view title) {
    using namespace schema_detail;
    requireExactString(requireMember(schema, "$id", "$"), id, "$.$id");
    requireExactString(requireMember(schema, "title", "$"), title, "$.title");
    requireExact(requireMember(requireMember(schema, "$defs", "$"), "fixedVersion-1.1", "$.$defs"),
                 version11, "$.$defs.fixedVersion-1.1");
    validateReferences(schema, schema);
    auto historical = schema;
    eraseMember(historical.at("$defs"), "fixedVersion-1.1");
    return historical;
}
} // namespace

void validateManifestSchemaV1_1(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion11(schema, "urn:kinetik:bloom:schema:project-manifest:1.1",
                                     "Bloom Project Manifest for Document 1.1");
    auto& documentVersion =
        historical.at("$defs").at("document-1.0").at("properties").at("schemaVersion");
    requireExact(documentVersion, R"({"$ref":"#/$defs/fixedVersion-1.1"})",
                 "manifest document version");
    documentVersion = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.0"})");
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.0"));
    validateManifestSchema(historical);
}

void validateDocumentSchemaV1_1(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion11(schema, "urn:kinetik:bloom:schema:project-document:1.1",
                                     "Bloom Project Document 1.1");
    auto& definitions = historical.at("$defs");
    requireExact(
        requireMember(definitions, "nodeLayout-1.1", "$.$defs"),
        R"({"type":"object","required":["nodeId","position","width","collapsed","muted"],"properties":{"nodeId":{"$ref":"#/$defs/objectId"},"position":{"type":"object","required":["x","y"],"properties":{"x":{"type":"number"},"y":{"type":"number"}},"unevaluatedProperties":false},"width":{"type":"number","exclusiveMinimum":0},"collapsed":{"type":"boolean"},"muted":{"type":"boolean"}},"unevaluatedProperties":false})",
        "$.$defs.nodeLayout-1.1");
    const auto& composition = validateObjectShape(definitions, "composition-1.0",
                                                  {"id", "name", "duration", "format", "parameters",
                                                   "animationCurves", "graph", "nodeLayout"},
                                                  {"id", "name", "duration", "format", "parameters",
                                                   "animationCurves", "graph", "nodeLayout"});
    validateArray(requireMember(composition, "nodeLayout", "composition"), "composition.nodeLayout",
                  "#/$defs/nodeLayout-1.1");
    requireExact(historical.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.1"})", "document schema version");
    historical.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.0"})");
    eraseMember(definitions, "nodeLayout-1.1");
    eraseMember(definitions.at("composition-1.0").at("properties"), "nodeLayout");
    std::erase_if(definitions.at("composition-1.0").at("required").asArray(),
                  [](const auto& value) { return value.asString() == "nodeLayout"; });
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.0"));
    historical.at("title") = json::Value(std::string("Bloom Project Document 1.0"));
    validateDocumentSchema(historical);
}
} // namespace bloom::quality
