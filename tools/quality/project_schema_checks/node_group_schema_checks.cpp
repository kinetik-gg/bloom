#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
namespace {
constexpr std::string_view version12 =
    R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":2}},"unevaluatedProperties":false})";

void eraseMember(json::Value& object, const std::string_view name) {
    std::erase_if(object.asObject(), [name](const auto& member) { return member.first == name; });
}

// Check every constraint 1.2 adds, then reduce the artifact to its 1.1 predecessor and reuse that
// version's complete checks for everything unchanged -- the same ladder validateDocumentSchemaV1_1
// already climbs down to 1.0. Normalization stays confined to this quality-tool copy.
json::Value checkVersion12(const json::Value& schema, const std::string_view id,
                           const std::string_view title) {
    using namespace schema_detail;
    requireExactString(requireMember(schema, "$id", "$"), id, "$.$id");
    requireExactString(requireMember(schema, "title", "$"), title, "$.title");
    requireExact(requireMember(requireMember(schema, "$defs", "$"), "fixedVersion-1.2", "$.$defs"),
                 version12, "$.$defs.fixedVersion-1.2");
    validateReferences(schema, schema);
    return schema;
}
} // namespace

void validateManifestSchemaV1_2(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion12(schema, "urn:kinetik:bloom:schema:project-manifest:1.2",
                                     "Bloom Project Manifest for Document 1.2");
    auto& documentVersion =
        historical.at("$defs").at("document-1.0").at("properties").at("schemaVersion");
    requireExact(documentVersion, R"({"$ref":"#/$defs/fixedVersion-1.2"})",
                 "manifest document version");
    // The predecessor artifact is this one with its version definition one minor back, so the
    // definition is renamed and retargeted rather than removed: the manifest has no other 1.2
    // content of its own.
    documentVersion = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.1"})");
    for (auto& member : historical.at("$defs").asObject()) {
        if (member.first == "fixedVersion-1.2") {
            member.first = "fixedVersion-1.1";
            member.second.at("properties").at("minor") = json::parse("{\"const\":1}");
        }
    }
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.1"));
    historical.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.1"));
    validateManifestSchemaV1_1(historical);
}

void validateDocumentSchemaV1_2(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion12(schema, "urn:kinetik:bloom:schema:project-document:1.2",
                                     "Bloom Project Document 1.2");
    auto& definitions = historical.at("$defs");
    requireExact(
        requireMember(definitions, "nodeGroup-1.2", "$.$defs"),
        R"({"$comment":"A node group frames member cards. It is layout only: no ports, no encapsulation, no evaluation meaning.","type":"object","required":["groupId","name","members","padding"],"properties":{"groupId":{"$ref":"#/$defs/objectId"},"name":{"$ref":"#/$defs/humanFacingName"},"members":{"type":"array","maxItems":1000000,"items":{"$ref":"#/$defs/objectId"}},"padding":{"type":"object","required":["x","y"],"properties":{"x":{"type":"number","minimum":0},"y":{"type":"number","minimum":0}},"unevaluatedProperties":false}},"unevaluatedProperties":false})",
        "$.$defs.nodeGroup-1.2");
    const auto& composition =
        validateObjectShape(definitions, "composition-1.0",
                            {"id", "name", "duration", "format", "parameters", "animationCurves",
                             "graph", "nodeLayout", "nodeGroups"},
                            {"id", "name", "duration", "format", "parameters", "animationCurves",
                             "graph", "nodeLayout", "nodeGroups"});
    validateArray(requireMember(composition, "nodeGroups", "composition"), "composition.nodeGroups",
                  "#/$defs/nodeGroup-1.2");
    // The eleventh allocator namespace is the one place 1.2 widens a shape the format contract
    // previously froze, so it is checked member by member rather than by inheritance.
    const auto& highestIssued = validateObjectShape(
        definitions, "highestIssued-1.2",
        {"composition", "node", "edge", "layer", "layerSlot", "parameter", "animationCurve",
         "keyframe", "driverBinding", "extensionRecord", "nodeGroup"},
        {"composition", "node", "edge", "layer", "layerSlot", "parameter", "animationCurve",
         "keyframe", "driverBinding", "extensionRecord", "nodeGroup"});
    requireExact(requireMember(highestIssued, "nodeGroup", "$.$defs.highestIssued-1.2"),
                 R"({"$ref":"#/$defs/allocatorHighWater"})",
                 "$.$defs.highestIssued-1.2.properties.nodeGroup");
    requireExact(definitions.at("idAllocation-1.0").at("properties").at("highestIssued"),
                 R"({"$ref":"#/$defs/highestIssued-1.2"})", "idAllocation highestIssued");
    requireExact(historical.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.2"})", "document schema version");

    historical.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.1"})");
    definitions.at("idAllocation-1.0").at("properties").at("highestIssued") =
        json::parse(R"({"$ref":"#/$defs/highestIssued-1.0"})");
    eraseMember(definitions, "fixedVersion-1.2");
    eraseMember(definitions, "nodeGroup-1.2");
    eraseMember(definitions, "highestIssued-1.2");
    eraseMember(definitions.at("composition-1.0").at("properties"), "nodeGroups");
    std::erase_if(definitions.at("composition-1.0").at("required").asArray(),
                  [](const auto& value) { return value.asString() == "nodeGroups"; });
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.1"));
    historical.at("title") = json::Value(std::string("Bloom Project Document 1.1"));
    validateDocumentSchemaV1_1(historical);
}
} // namespace bloom::quality
