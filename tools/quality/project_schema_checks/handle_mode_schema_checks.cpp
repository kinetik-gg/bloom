#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.32 adds the
// optional keyframe member `handleMode`. Removing the member and renaming the version must give
// back the previous artifact exactly.
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

void validateDocumentSchemaV1_32(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersion(schema, false, 32);
    auto& defs = previous.at("$defs");
    requireExact(defs.at("keyframeHandleMode-1.32").at("enum"), R"(["aligned","auto"])",
                 "closed 1.32 handle mode vocabulary (free is the default and never written)");
    for (const auto* keyframe :
         {"scalarKeyframe-1.3", "vec2ComponentKeyframe-1.9", "vec3ComponentKeyframe-1.9",
          "color4ComponentKeyframe-1.9", "pathKeyframe-1.24"}) {
        auto& properties = defs.at(keyframe).at("properties");
        requireExact(properties.at("handleMode"), R"({"$ref":"#/$defs/keyframeHandleMode-1.32"})",
                     "keyframe handle mode member");
        erase(properties, "handleMode");
    }
    erase(defs, "keyframeHandleMode-1.32");
    validateDocumentSchemaV1_31(previous);
}

void validateManifestSchemaV1_32(const json::Value& schema) {
    validateManifestSchemaV1_31(previousVersion(schema, true, 32));
}
} // namespace bloom::quality
