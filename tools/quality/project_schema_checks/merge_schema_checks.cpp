#include "schema_check_helpers.hpp"
#include <algorithm>
#include <string>

namespace bloom::quality {
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
void version(const json::Value& schema) {
    schema_detail::requireExact(
        schema.at("$defs").at("fixedVersion-1.6"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":6}},"unevaluatedProperties":false})",
        "version 1.6");
    schema_detail::validateReferences(schema, schema);
}
} // namespace
void validateDocumentSchemaV1_6(const json::Value& schema) {
    using namespace schema_detail;
    version(schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.6",
                       "document id");
    requireExactString(schema.at("title"), "Bloom Project Document 1.6", "document title");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.6"})", "document version");
    auto previous = schema;
    auto& defs = previous.at("$defs");
    auto& graph = defs.at("graph-1.0").at("properties");
    requireExact(graph.at("merges"),
                 R"({"type":"array","maxItems":1000000,"items":{"$ref":"#/$defs/layerStack-1.0"}})",
                 "merges");
    erase(graph, "merges");
    requireExact(graph.at("layerStack"),
                 R"({"anyOf":[{"$ref":"#/$defs/layerStack-1.0"},{"type":"null"}]})",
                 "primary Merge");
    graph.at("layerStack") = json::parse(R"({"$ref":"#/$defs/layerStack-1.0"})");
    auto& entry = defs.at("layerStackEntry-1.0").at("properties").at("layerId");
    requireExact(entry, R"({"anyOf":[{"$ref":"#/$defs/objectId"},{"type":"null"}]})",
                 "optional layer identity");
    entry = json::parse(R"({"$ref":"#/$defs/objectId"})");
    auto& stack = defs.at("layerStack-1.0").at("properties");
    requireExact(stack.at("enabled"), R"({"type":"boolean"})", "Merge enabled");
    erase(stack, "enabled");
    for (auto& member : defs.asObject())
        if (member.first == "fixedVersion-1.6")
            member.first = "fixedVersion-1.5";
    defs.at("fixedVersion-1.5").at("properties").at("minor") = json::parse(R"({"const":5})");
    previous.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.5"})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.5"));
    previous.at("title") = json::Value(std::string("Bloom Project Document 1.5"));
    validateDocumentSchemaV1_5(previous);
}
void validateManifestSchemaV1_6(const json::Value& schema) {
    using namespace schema_detail;
    version(schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.6",
                       "manifest id");
    requireExactString(schema.at("title"), "Bloom Project Manifest for Document 1.6",
                       "manifest title");
    auto previous = schema;
    auto& declaration =
        previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion");
    requireExact(declaration, R"({"$ref":"#/$defs/fixedVersion-1.6"})", "manifest version");
    declaration = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.5"})");
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.6")
            member.first = "fixedVersion-1.5";
    previous.at("$defs").at("fixedVersion-1.5").at("properties").at("minor") =
        json::parse(R"({"const":5})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.5"));
    previous.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.5"));
    validateManifestSchemaV1_5(previous);
}
} // namespace bloom::quality
