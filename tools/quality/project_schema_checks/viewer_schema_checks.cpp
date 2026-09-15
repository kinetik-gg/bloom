#include "schema_check_helpers.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace bloom::quality {
namespace {

void erase(json::Value& object, const std::string_view name) {
    std::erase_if(object.asObject(), [name](const auto& member) { return member.first == name; });
}

void version(const json::Value& schema) {
    using namespace schema_detail;
    requireExact(
        schema.at("$defs").at("fixedVersion-1.8"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":8}},"unevaluatedProperties":false})",
        "version 1.8");
    validateReferences(schema, schema);
}

void validateSafeAreas(const json::Value& definitions) {
    using namespace schema_detail;
    const auto& safeAreas = requireMember(definitions, "safeAreaSettings-1.8", "$.$defs");
    requireExactKeys(safeAreas, {"type", "required", "properties", "unevaluatedProperties"},
                     "$.$defs.safeAreaSettings-1.8 keys");
    requireExactString(requireMember(safeAreas, "type", "safe areas"), "object", "safe areas type");
    requireExactStringArray(requireMember(safeAreas, "required", "safe areas"), {"action", "title"},
                            "safe areas required");
    requireExactBoolean(requireMember(safeAreas, "unevaluatedProperties", "safe areas"), false,
                        "safe areas closure");
    const auto& properties = requireMember(safeAreas, "properties", "safe areas");
    requireExactKeys(properties, {"action", "title"}, "safe areas properties");
    requireExact(requireMember(properties, "action", "safe areas"),
                 R"({"type":"number","exclusiveMinimum":0,"maximum":1})", "safe areas action");
    requireExact(requireMember(properties, "title", "safe areas"),
                 R"({"type":"number","exclusiveMinimum":0,"maximum":1})", "safe areas title");
}

} // namespace

void validateDocumentSchemaV1_8(const json::Value& schema) {
    using namespace schema_detail;
    version(schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.8",
                       "document id");
    requireExactString(schema.at("title"), "Bloom Project Document 1.8", "document title");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.8"})", "document version");
    const auto& definitions = schema.at("$defs");
    validateSafeAreas(definitions);
    const auto& composition = definitions.at("composition-1.0").at("properties");
    requireExact(requireMember(composition, "safeAreas", "composition"),
                 R"({"$ref":"#/$defs/safeAreaSettings-1.8"})", "composition safe areas");

    auto previous = schema;
    auto& previousDefinitions = previous.at("$defs");
    erase(previousDefinitions.at("composition-1.0").at("properties"), "safeAreas");
    erase(previousDefinitions, "safeAreaSettings-1.8");
    for (auto& member : previousDefinitions.asObject()) {
        if (member.first == "fixedVersion-1.8") {
            member.first = "fixedVersion-1.7";
        }
    }
    previousDefinitions.at("fixedVersion-1.7").at("properties").at("minor") =
        json::parse(R"({"const":7})");
    previous.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.7"})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.7"));
    previous.at("title") = json::Value(std::string("Bloom Project Document 1.7"));
    validateDocumentSchemaV1_7(previous);
}

void validateManifestSchemaV1_8(const json::Value& schema) {
    using namespace schema_detail;
    version(schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.8",
                       "manifest id");
    requireExactString(schema.at("title"), "Bloom Project Manifest for Document 1.8",
                       "manifest title");
    const auto& definitions = schema.at("$defs");
    requireExact(definitions.at("document-1.0").at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.8"})", "manifest version");

    auto previous = schema;
    auto& previousDefinitions = previous.at("$defs");
    for (auto& member : previousDefinitions.asObject()) {
        if (member.first == "fixedVersion-1.8") {
            member.first = "fixedVersion-1.7";
        }
    }
    previousDefinitions.at("fixedVersion-1.7").at("properties").at("minor") =
        json::parse(R"({"const":7})");
    previousDefinitions.at("document-1.0").at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.7"})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.7"));
    previous.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.7"));
    validateManifestSchemaV1_7(previous);
}

void validateDocumentSchemaV1_9(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.9",
                       "document id");
    requireExactString(schema.at("title"), "Bloom Project Document 1.9", "document title");
    requireExact(
        schema.at("$defs").at("fixedVersion-1.9"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":9}},"unevaluatedProperties":false})",
        "version 1.9");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.9"})", "document version");

    const auto& definitions = schema.at("$defs");
    static const std::array curveSpecs{
        ExpectedBranch{"scalar", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
        ExpectedBranch{"vec2", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
        ExpectedBranch{"vec3", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
        ExpectedBranch{"color4", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
    };
    const auto curves = validateDiscriminatedUnion(definitions, "animationCurve-1.9", curveSpecs);
    validateArray(requireMember(*curves[1], "keyframes", "vec2 curve"),
                  "$.$defs.animationCurve-1.9 vec2 keys", "#/$defs/vec2ComponentKeyframe-1.9",
                  1'000'000, 1, true);
    validateArray(requireMember(*curves[2], "keyframes", "vec3 curve"),
                  "$.$defs.animationCurve-1.9 vec3 keys", "#/$defs/vec3ComponentKeyframe-1.9",
                  1'000'000, 1, true);
    validateArray(requireMember(*curves[3], "keyframes", "color4 curve"),
                  "$.$defs.animationCurve-1.9 color keys", "#/$defs/color4ComponentKeyframe-1.9",
                  1'000'000, 1, true);
    for (const std::string_view definitionName :
         {"vec2ComponentKeyframe-1.9", "vec3ComponentKeyframe-1.9",
          "color4ComponentKeyframe-1.9"}) {
        static_cast<void>(
            validateObjectShape(definitions, definitionName,
                                {"id", "component", "time", "value", "outgoingInterpolation"},
                                {"id", "component", "time", "value", "outgoingInterpolation"}));
    }
    validateReferences(schema, schema);
}

void validateManifestSchemaV1_9(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.9",
                       "manifest id");
    requireExactString(schema.at("title"), "Bloom Project Manifest for Document 1.9",
                       "manifest title");
    requireExact(
        schema.at("$defs").at("fixedVersion-1.9"),
        R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":9}},"unevaluatedProperties":false})",
        "manifest version");
    requireExact(schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.9"})", "manifest document version");
    validateReferences(schema, schema);
}

} // namespace bloom::quality
