#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
// Document 1.30 adds Blender's keyframe easing equations (ten interpolation tokens and the optional
// keyframe `easing` member) and the optional presentation-only layer members `shy` and
// `nameFollowsSource`. Removing them and the version must give back the 1.29 artifact exactly.
namespace {
void erase(json::Value& object, std::string_view key) {
    std::erase_if(object.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVersionV1_30(const json::Value& schema, bool manifest) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.30",
                       "easing schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.30").at("properties").at("minor"),
                 R"({"const":30})", "easing minor");
    const auto& version =
        manifest ? schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : schema.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.30"})", "schema version");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.30")
            member.first = "fixedVersion-1.29";
    previous.at("$defs").at("fixedVersion-1.29").at("properties").at("minor") =
        json::parse(R"({"const":29})");
    auto& previousRef =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    previousRef = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.29"})");
    previous.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-" + kind + ":1.29"));
    return previous;
}
} // namespace

void validateDocumentSchemaV1_30(const json::Value& schema) {
    using namespace schema_detail;
    auto previous = previousVersionV1_30(schema, false);
    auto& defs = previous.at("$defs");
    requireExact(
        defs.at("keyframeInterpolation-1.30").at("enum"),
        R"(["hold","linear","ease-in-out","sine","quad","cubic","quart","quint","expo","circ","back","bounce","elastic"])",
        "closed 1.30 interpolation vocabulary");
    requireExact(defs.at("keyframeEasing-1.30").at("required"),
                 R"(["mode","back","amplitude","period"])", "easing members");
    requireExact(defs.at("keyframeEasing-1.30").at("properties").at("mode"),
                 R"({"enum":["auto","in","out","in-out"]})", "easing modes");
    requireExact(defs.at("keyframeEasing-1.30").at("properties").at("amplitude"),
                 R"({"type":"number","minimum":0})", "non-negative amplitude");
    for (const auto* keyframe :
         {"scalarKeyframe-1.3", "vec2ComponentKeyframe-1.9", "vec3ComponentKeyframe-1.9",
          "color4ComponentKeyframe-1.9", "pathKeyframe-1.24"}) {
        auto& properties = defs.at(keyframe).at("properties");
        requireExact(properties.at("outgoingInterpolation"),
                     R"({"$ref":"#/$defs/keyframeInterpolation-1.30"})", "1.30 interpolation");
        requireExact(properties.at("easing"), R"({"$ref":"#/$defs/keyframeEasing-1.30"})",
                     "keyframe easing member");
        properties.at("outgoingInterpolation") =
            json::parse(R"({"$ref":"#/$defs/keyframeInterpolation-1.3"})");
        erase(properties, "easing");
    }
    auto& layer = defs.at("layerOutput-1.0").at("properties");
    for (const auto* name : {"shy", "nameFollowsSource"}) {
        requireExact(layer.at(name), R"({"type":"boolean"})", "layer presentation flag");
        erase(layer, name);
    }
    erase(defs, "keyframeInterpolation-1.30");
    erase(defs, "keyframeEasing-1.30");
    validateDocumentSchemaV1_29(previous);
}

void validateManifestSchemaV1_30(const json::Value& schema) {
    validateManifestSchemaV1_29(previousVersionV1_30(schema, true));
}
} // namespace bloom::quality
