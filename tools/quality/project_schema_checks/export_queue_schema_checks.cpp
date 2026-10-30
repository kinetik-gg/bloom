#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.29 (ADR 0033) adds the export queue: the required project member exportQueue and the
// required exportJob/exportOutput allocator namespaces. Removing them and the version must give
// back the 1.28 artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersionV1_29(const json::Value& schema, bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.29",
                       "export queue schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.29").at("properties").at("minor"),
                 R"({"const":29})", "export queue minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.29"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.29")
            member.first = "fixedVersion-1.28";
    previous.at("$defs").at("fixedVersion-1.28").at("properties").at("minor") =
        json::parse(R"({"const":28})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.28"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.28"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_29(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersionV1_29(schema, false);
    auto& defs = previous.at("$defs");
    requireExact(defs.at("project-1.0").at("properties").at("exportQueue"),
                 R"({"$ref":"#/$defs/exportQueue-1.29"})", "project export queue");
    requireExact(
        defs.at("exportQueue-1.29"),
        R"({"type":"object","required":["jobs"],"properties":{"jobs":{"type":"array","maxItems":1024,"items":{"$ref":"#/$defs/exportJob-1.29"}}},"unevaluatedProperties":false})",
        "export queue");
    requireExact(defs.at("exportJob-1.29").at("required"),
                 R"(["id","composition","name","range","outputs","enabled"])",
                 "export job members");
    requireExact(
        defs.at("exportJob-1.29").at("properties").at("outputs"),
        R"({"type":"array","minItems":1,"maxItems":16,"items":{"$ref":"#/$defs/exportOutput-1.29"}})",
        "export outputs bound");
    requireExact(
        defs.at("exportOutputSettings-1.29").at("properties").at("format"),
        R"({"enum":["png","exr","tiff","dpx","prores-mov","dnxhr-mxf","h264-mov","h264-mp4","hevc-mov","hevc-mp4","wav"]})",
        "closed 1.29 format vocabulary");
    requireExact(defs.at("exportDestination-1.29").at("properties").at("overwrite"),
                 R"({"enum":["ask","replace","refuse"]})", "overwrite policy");
    for (const auto* name : {"exportJob", "exportOutput"})
        requireExact(defs.at("highestIssued-1.24").at("properties").at(name),
                     R"({"$ref":"#/$defs/allocatorHighWater"})", "export id namespace");
    for (const auto* def :
         {"exportQueue-1.29", "exportJob-1.29", "exportRange-1.29", "exportOutput-1.29",
          "exportOutputSettings-1.29", "exportNaming-1.29", "exportDestination-1.29"})
        erase(defs, def);
    erase(defs.at("project-1.0").at("properties"), "exportQueue");
    std::erase_if(defs.at("project-1.0").at("required").asArray(),
                  [](const auto& value) { return value.asString() == "exportQueue"; });
    auto& highest = defs.at("highestIssued-1.24");
    for (const auto* name : {"exportJob", "exportOutput"}) {
        erase(highest.at("properties"), name);
        std::erase_if(highest.at("required").asArray(),
                      [name](const auto& value) { return value.asString() == name; });
    }
    validateDocumentSchemaV1_28(previous);
}

void validateManifestSchemaV1_29(const json::Value& schema) {
    validateManifestSchemaV1_28(previousVersionV1_29(schema, true));
}
} // namespace bloom::quality
