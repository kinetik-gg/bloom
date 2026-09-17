#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersion(const json::Value& schema, bool manifest) {
    using namespace schema_detail;
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.16",
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.16").at("properties").at("minor"),
                 R"({"const":16})", "minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.16"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.16")
            member.first = "fixedVersion-1.15";
    previous.at("$defs").at("fixedVersion-1.15").at("properties").at("minor") =
        json::parse(R"({"const":15})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.15"})");
    previous.at("$id") = json::Value("urn:kinetik:bloom:schema:project-" + kind + ":1.15");
    previous.at("title") = json::Value(std::string(
        manifest ? "Bloom Project Manifest for Document 1.15" : "Bloom Project Document 1.15"));
    return previous;
}
} // namespace
void validateDocumentSchemaV1_16(const json::Value& schema) {
    using namespace schema_detail;
    const auto& defs = schema.at("$defs");
    const auto& asset = defs.at("asset-1.11");
    requireExact(
        asset.at("required"),
        R"(["id","kind","locator","contentDigest","interpretation","width","height","manifest","name","tags","order"])",
        "asset required members");
    requireExact(requireMember(asset.at("properties"), "name", "asset properties"),
                 R"({"$ref":"#/$defs/humanFacingName"})", "asset name");
    requireExact(requireMember(asset.at("properties"), "folder", "asset properties"),
                 R"({"$ref":"#/$defs/objectId"})", "asset folder");
    requireExact(requireMember(asset.at("properties"), "order", "asset properties"),
                 R"({"$ref":"#/$defs/allocatorHighWater"})", "asset order");
    requireExact(
        requireMember(asset.at("properties"), "tags", "asset properties"),
        R"({"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"string","minLength":1,"maxLength":256}})",
        "bounded tags");
    requireExact(
        defs.at("assetFolder-1.16"),
        R"({"type":"object","required":["id","name"],"properties":{"id":{"$ref":"#/$defs/objectId"},"name":{"$ref":"#/$defs/humanFacingName"},"parent":{"$ref":"#/$defs/objectId"}},"unevaluatedProperties":false})",
        "folder record");
    requireExact(
        defs.at("project-1.0").at("properties").at("assetFolders"),
        R"({"type":"array","maxItems":100000,"items":{"$ref":"#/$defs/assetFolder-1.16"}})",
        "folders");
    requireExact(requireMember(defs.at("highestIssued-1.2").at("properties"), "assetFolder",
                               "allocator properties"),
                 R"({"$ref":"#/$defs/allocatorHighWater"})", "folder watermark");
    validateReferences(schema, schema);
    auto previous = previousVersion(schema, false);
    auto& previousDefs = previous.at("$defs");
    erase(previousDefs, "assetFolder-1.16");
    erase(previousDefs.at("project-1.0").at("properties"), "assetFolders");
    erase(previousDefs.at("highestIssued-1.2").at("properties"), "assetFolder");
    auto& oldAsset = previousDefs.at("asset-1.11");
    for (const auto* key : {"name", "folder", "tags", "order"})
        erase(oldAsset.at("properties"), key);
    oldAsset.at("required") = json::parse(
        R"(["id","kind","locator","contentDigest","interpretation","width","height","manifest"])");
    validateDocumentSchemaV1_15(previous);
}
void validateManifestSchemaV1_16(const json::Value& schema) {
    schema_detail::validateReferences(schema, schema);
    validateManifestSchemaV1_15(previousVersion(schema, true));
}
} // namespace bloom::quality
