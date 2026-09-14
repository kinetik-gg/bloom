#include "schema_check_helpers.hpp"

namespace bloom::quality {
namespace {
json::Value previousSchema(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"),
                       manifest ? "urn:kinetik:bloom:schema:project-manifest:1.7"
                                : "urn:kinetik:bloom:schema:project-document:1.7",
                       "schema id");
    requireExactString(schema.at("title"),
                       manifest ? "Bloom Project Manifest for Document 1.7"
                                : "Bloom Project Document 1.7",
                       "schema title");
    requireExact(
        schema.at("$defs").at("fixedVersion-1.7"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":7}},"unevaluatedProperties":false})",
        "version 1.7");
    validateReferences(schema, schema);
    auto previous = schema;
    auto& declaration =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    requireExact(declaration, R"({"$ref":"#/$defs/fixedVersion-1.7"})", "document version");
    declaration = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.6"})");
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.7")
            member.first = "fixedVersion-1.6";
    previous.at("$defs").at("fixedVersion-1.6").at("properties").at("minor") =
        json::parse(R"({"const":6})");
    previous.at("$id") =
        json::Value(std::string(manifest ? "urn:kinetik:bloom:schema:project-manifest:1.6"
                                         : "urn:kinetik:bloom:schema:project-document:1.6"));
    previous.at("title") = json::Value(std::string(
        manifest ? "Bloom Project Manifest for Document 1.6" : "Bloom Project Document 1.6"));
    return previous;
}
} // namespace
void validateDocumentSchemaV1_7(const json::Value& schema) {
    validateDocumentSchemaV1_6(previousSchema(schema, false));
}
void validateManifestSchemaV1_7(const json::Value& schema) {
    validateManifestSchemaV1_6(previousSchema(schema, true));
}
} // namespace bloom::quality
