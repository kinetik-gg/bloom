#include "schema_check_helpers.hpp"
#include <algorithm>
#include <string>

namespace bloom::quality {
namespace {
void erase(json::Value& object, const std::string_view name) {
    std::erase_if(object.asObject(), [name](const auto& member) { return member.first == name; });
}
void version(const json::Value& schema) {
    using namespace schema_detail;
    requireExact(schema.at("$defs").at("fixedVersion-1.5"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":5}},"unevaluatedProperties":false})", "version 1.5");
    validateReferences(schema, schema);
}
}
void validateDocumentSchemaV1_5(const json::Value& schema) {
    using namespace schema_detail;
    version(schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.5", "document id");
    requireExactString(schema.at("title"), "Bloom Project Document 1.5", "document title");
    requireExact(schema.at("properties").at("schemaVersion"), R"({"$ref":"#/$defs/fixedVersion-1.5"})", "document version");
    auto previous = schema;
    auto& definitions = previous.at("$defs");
    auto& layer = definitions.at("layerOutput-1.0").at("properties");
    for (const auto* key : {"inPoint", "outPoint"}) {
        requireExact(layer.at(key), R"({"$ref":"#/$defs/rationalTime"})", key);
        erase(layer, key);
    }
    for (const auto* key : {"enabled", "solo", "locked"}) {
        requireExact(layer.at(key), R"({"type":"boolean"})", key);
        erase(layer, key);
    }
    requireExact(layer.at("labelColor"), R"({"type":"array","minItems":3,"maxItems":3,"items":{"type":"integer","minimum":0,"maximum":255}})", "RGB label");
    erase(layer, "labelColor");
    auto& composition = definitions.at("composition-1.0").at("properties");
    requireExact(composition.at("workArea"), R"({"type":"object","required":["start","end"],"properties":{"start":{"$ref":"#/$defs/rationalTime"},"end":{"$ref":"#/$defs/rationalTime"}},"unevaluatedProperties":false})", "work area");
    erase(composition, "workArea");
    erase(definitions, "fixedVersion-1.5");
    previous.at("properties").at("schemaVersion") = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.4"})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.4"));
    previous.at("title") = json::Value(std::string("Bloom Project Document 1.4"));
    validateDocumentSchemaV1_4(previous);
}
void validateManifestSchemaV1_5(const json::Value& schema) {
    using namespace schema_detail;
    version(schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.5", "manifest id");
    requireExactString(schema.at("title"), "Bloom Project Manifest for Document 1.5", "manifest title");
    auto previous = schema;
    auto& declaration = previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion");
    requireExact(declaration, R"({"$ref":"#/$defs/fixedVersion-1.5"})", "manifest version");
    declaration = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.4"})");
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.5") member.first = "fixedVersion-1.4";
    previous.at("$defs").at("fixedVersion-1.4").at("properties").at("minor") = json::parse(R"({"const":4})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.4"));
    previous.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.4"));
    validateManifestSchemaV1_4(previous);
}
} // namespace bloom::quality
