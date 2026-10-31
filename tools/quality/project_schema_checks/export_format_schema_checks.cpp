#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.33 adds the WebM and AV1 export formats (`vp9-webm`, `av1-webm`, `av1-mp4`) and the
// VP9/AV1 codec profile `8bit`. Restoring the 1.29 inline vocabularies, removing the two new
// definitions and renaming the version must give back the previous artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
// The same schema as `minor - 1`: only the id, the fixed-version definition and its references.
json::Value previousVersion(const json::Value& schema, const bool manifest, const int minor) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    const std::string current = "1." + std::to_string(minor);
    const std::string previousName = "1." + std::to_string(minor - 1);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":" + current,
                       "schema id");
    requireExact(schema.at("$defs").at("fixedVersion-" + current).at("properties").at("minor"),
                 "{\"const\":" + std::to_string(minor) + "}", "schema minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, "{\"$ref\":\"#/$defs/fixedVersion-" + current + "\"}", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-" + current)
            member.first = "fixedVersion-" + previousName;
    previous.at("$defs").at("fixedVersion-" + previousName).at("properties").at("minor") =
        json::parse("{\"const\":" + std::to_string(minor - 1) + "}");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse("{\"$ref\":\"#/$defs/fixedVersion-" + previousName + "\"}");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":" + previousName));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_33(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false, 33);
    auto& defs = previous.at("$defs");
    requireExact(defs.at("exportFormat-1.33").at("enum"),
                 R"(["png","exr","tiff","dpx","prores-mov","dnxhr-mxf","h264-mov","h264-mp4",)"
                 R"("hevc-mov","hevc-mp4","wav","vp9-webm","av1-webm","av1-mp4"])",
                 "closed 1.33 export format vocabulary");
    requireExact(defs.at("exportCodecProfile-1.33").at("enum"),
                 R"(["","proxy","lt","422","hq","4444","4444xq","lb","sq","hqx","444","high",)"
                 R"("main","main10","8bit","10bit","16bit","s16","s24"])",
                 "closed 1.33 codec profile vocabulary");
    auto& settings = defs.at("exportOutputSettings-1.29").at("properties");
    requireExact(settings.at("format"), R"({"$ref":"#/$defs/exportFormat-1.33"})",
                 "1.33 export format");
    requireExact(settings.at("codecProfile"), R"({"$ref":"#/$defs/exportCodecProfile-1.33"})",
                 "1.33 codec profile");
    settings.at("format") = json::parse(
        R"({"enum":["png","exr","tiff","dpx","prores-mov","dnxhr-mxf","h264-mov","h264-mp4",)"
        R"("hevc-mov","hevc-mp4","wav"]})");
    settings.at("codecProfile") = json::parse(
        R"({"enum":["","proxy","lt","422","hq","4444","4444xq","lb","sq","hqx","444","high",)"
        R"("main","main10","10bit","16bit","s16","s24"]})");
    erase(defs, "exportFormat-1.33");
    erase(defs, "exportCodecProfile-1.33");
    validateDocumentSchemaV1_32(previous);
}

void validateManifestSchemaV1_33(const json::Value& schema) {
    validateManifestSchemaV1_32(previousVersion(schema, true, 33));
}
} // namespace bloom::quality
