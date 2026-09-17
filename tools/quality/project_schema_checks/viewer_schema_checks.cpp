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

void validateDocumentSchemaV1_12(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.12",
                       "document id");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.12"})", "document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.12").at("properties").at("minor"),
                 R"({"const":12})", "minor");
    const auto& definitions = schema.at("$defs");
    requireExact(
        definitions.at("keyframeHandle-1.12"),
        R"({"type":"object","required":["time","value"],"properties":{"time":{"type":"number","minimum":0,"maximum":1},"value":{"type":"number"}},"unevaluatedProperties":false})",
        "closed ease handle");
    // Both handles are OPTIONAL on every keyframe that can carry one -- a default handle is never
    // written -- so they appear in `properties` and never in `required`.
    for (const auto* name : {"scalarKeyframe-1.3", "vec2ComponentKeyframe-1.9",
                             "vec3ComponentKeyframe-1.9", "color4ComponentKeyframe-1.9"}) {
        const auto& properties = definitions.at(name).at("properties");
        for (const auto* member : {"outgoingHandle", "incomingHandle"}) {
            requireExact(properties.at(member), R"({"$ref":"#/$defs/keyframeHandle-1.12"})",
                         "keyframe ease handle");
        }
        const auto& required = schema_detail::requireArray(
            schema_detail::requireMember(definitions.at(name), "required", "keyframe"),
            "keyframe required");
        for (const auto& entry : required) {
            if (entry.isString() &&
                (entry.asString() == "outgoingHandle" || entry.asString() == "incomingHandle")) {
                fail("an ease handle must never be a required keyframe member");
            }
        }
    }
    validateReferences(schema, schema);
}

void validateManifestSchemaV1_12(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.12",
                       "manifest id");
    requireExact(schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.12"})", "manifest document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.12").at("properties").at("minor"),
                 R"({"const":12})", "manifest minor");
    validateReferences(schema, schema);
}

void validateDocumentSchemaV1_11(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.11",
                       "document id");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.11"})", "document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.11").at("properties").at("minor"),
                 R"({"const":11})", "minor");
    auto definitions = schema.at("$defs");
    for (const auto* name : {"asset-1.11", "assetLocator-1.11"}) {
        requireExact(definitions.at(name).at("unevaluatedProperties"), "false",
                     "closed asset record");
        definitions.at(name).at("unevaluatedProperties") = json::Value(true);
    }
    static_cast<void>(validateObjectShape(
        definitions, "asset-1.11",
        {"id", "kind", "locator", "contentDigest", "interpretation", "width", "height", "manifest"},
        {"id", "kind", "locator", "contentDigest", "interpretation", "width", "height", "manifest",
         "audio"}));
    requireExact(definitions.at("audio-1.11").at("unevaluatedProperties"), "false",
                 "closed audio metadata");
    definitions.at("audio-1.11").at("unevaluatedProperties") = json::Value(true);
    const auto& audio =
        validateObjectShape(definitions, "audio-1.11", {"rate", "channels", "frames", "duration"},
                            {"rate", "channels", "frames", "duration"});
    requireExact(requireMember(audio, "rate", "audio metadata"),
                 R"({"type":"integer","minimum":8000,"maximum":384000})", "audio metadata rate");
    requireExact(requireMember(audio, "channels", "audio metadata"),
                 R"({"type":"integer","minimum":1,"maximum":32})", "audio metadata channels");
    requireExact(requireMember(audio, "frames", "audio metadata"), R"({"$ref":"#/$defs/objectId"})",
                 "audio metadata frames");
    requireExact(requireMember(audio, "duration", "audio metadata"),
                 R"({"$ref":"#/$defs/positiveRationalTime"})", "audio metadata duration");
    static_cast<void>(validateObjectShape(definitions, "assetLocator-1.11",
                                          {"kind", "portability", "path", "relinkHint"},
                                          {"kind", "portability", "path", "relinkHint"}));
    requireExact(definitions.at("project-1.0").at("properties").at("assets"),
                 R"({"type":"array","maxItems":100000,"items":{"$ref":"#/$defs/asset-1.11"}})",
                 "assets");
    requireExact(definitions.at("highestIssued-1.2").at("properties").at("asset"),
                 R"({"$ref":"#/$defs/allocatorHighWater"})", "asset watermark");
    validateReferences(schema, schema);
}
void validateManifestSchemaV1_11(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.11",
                       "manifest id");
    requireExact(schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.11"})", "manifest document version");
    const auto& providedNodeTypeItems = schema.at("$defs")
                                            .at("requirement-1.0")
                                            .at("properties")
                                            .at("providedNodeTypeIds")
                                            .at("items");
    const auto& reservedTypes = schema_detail::requireArray(
        schema_detail::requireMember(providedNodeTypeItems, "allOf", "provided node type items"),
        "provided node type items allOf")[1];
    requireExact(
        schema_detail::requireMember(
            schema_detail::requireMember(reservedTypes, "not", "reserved node types"), "enum",
            "reserved node types"),
        R"(["bloom.composition-output","bloom.layer-output","bloom.layer-stack","bloom.solid-source","bloom.text-source","bloom.image-source","bloom.audio-source"])",
        "manifest reserved node types");
    validateReferences(schema, schema);
}

void validateDocumentSchemaV1_13(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.13",
                       "document id");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.13"})", "document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.13").at("properties").at("minor"),
                 R"({"const":13})", "minor");
    const auto& definitions = schema.at("$defs");
    requireExact(definitions.at("layerOutput-1.0").at("properties").at("parent"),
                 R"({"$ref":"#/$defs/objectId"})", "parent layer identity");
    requireExact(
        definitions.at("keyframeHandle-1.12"),
        R"({"type":"object","required":["time","value"],"properties":{"time":{"type":"number","minimum":0,"maximum":1},"value":{"type":"number"}},"unevaluatedProperties":false})",
        "closed ease handle");
    // Both handles are OPTIONAL on every keyframe that can carry one -- a default handle is never
    // written -- so they appear in `properties` and never in `required`.
    for (const auto* name : {"scalarKeyframe-1.3", "vec2ComponentKeyframe-1.9",
                             "vec3ComponentKeyframe-1.9", "color4ComponentKeyframe-1.9"}) {
        const auto& properties = definitions.at(name).at("properties");
        for (const auto* member : {"outgoingHandle", "incomingHandle"}) {
            requireExact(properties.at(member), R"({"$ref":"#/$defs/keyframeHandle-1.12"})",
                         "keyframe ease handle");
        }
        const auto& required = schema_detail::requireArray(
            schema_detail::requireMember(definitions.at(name), "required", "keyframe"),
            "keyframe required");
        for (const auto& entry : required) {
            if (entry.isString() &&
                (entry.asString() == "outgoingHandle" || entry.asString() == "incomingHandle")) {
                fail("an ease handle must never be a required keyframe member");
            }
        }
    }
    validateReferences(schema, schema);
}

void validateManifestSchemaV1_13(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.13",
                       "manifest id");
    requireExact(schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.13"})", "manifest document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.13").at("properties").at("minor"),
                 R"({"const":13})", "manifest minor");
    validateReferences(schema, schema);
}

void validateDocumentSchemaV1_14(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.14",
                       "document id");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.14"})", "document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.14").at("properties").at("minor"),
                 R"({"const":14})", "minor");
    requireExact(
        schema.at("$defs").at("pathValue-1.14"),
        R"({"type":"object","required":["kind","anchors","closed"],"properties":{"kind":{"const":"path"},"anchors":{"type":"array","maxItems":4096,"items":{"$ref":"#/$defs/pathAnchor-1.14"}},"closed":{"type":"boolean"}},"unevaluatedProperties":true})",
        "bounded path value");
    requireExact(
        schema.at("$defs").at("pathAnchor-1.14"),
        R"({"type":"object","required":["point"],"properties":{"point":{"$ref":"#/$defs/pathPoint-1.14"},"inHandle":{"$ref":"#/$defs/pathPoint-1.14"},"outHandle":{"$ref":"#/$defs/pathPoint-1.14"}},"unevaluatedProperties":false})",
        "closed path anchor");
    requireExact(
        schema.at("$defs").at("pathPoint-1.14"),
        R"({"type":"object","required":["x","y"],"properties":{"x":{"type":"number"},"y":{"type":"number"}},"unevaluatedProperties":false})",
        "path point");
    const auto& alternatives = requireArray(schema.at("$defs").at("parameterValue-1.4").at("oneOf"),
                                            "parameter value alternatives");
    if (alternatives.empty())
        fail("path value alternative missing");
    requireExact(alternatives.back(), R"({"$ref":"#/$defs/pathValue-1.14"})",
                 "path value discriminator arm");
    validateReferences(schema, schema);
}
void validateDocumentSchemaV1_15(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.15",
                       "document id");
    requireExact(schema.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.15"})", "document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.15").at("properties").at("minor"),
                 R"({"const":15})", "minor");
    // 1.15 is additive over 1.14: the path value family SHAPE-1 froze at 1.14 is still referenced
    // under its own introduction version, and nothing about it moved.
    requireExact(
        schema.at("$defs").at("pathValue-1.14"),
        R"({"type":"object","required":["kind","anchors","closed"],"properties":{"kind":{"const":"path"},"anchors":{"type":"array","maxItems":4096,"items":{"$ref":"#/$defs/pathAnchor-1.14"}},"closed":{"type":"boolean"}},"unevaluatedProperties":true})",
        "bounded path value");
    const auto& alternatives = requireArray(schema.at("$defs").at("parameterValue-1.4").at("oneOf"),
                                            "parameter value alternatives");
    if (alternatives.empty())
        fail("path value alternative missing");
    requireExact(alternatives.back(), R"({"$ref":"#/$defs/pathValue-1.14"})",
                 "path value discriminator arm");
    // What 1.15 itself adds: the Font asset kind, its descriptor, and the locator portabilities a
    // builtin or system face needs (an embedded face carries no filesystem path, hence minLength
    // 0).
    requireExact(
        schema.at("$defs").at("font-1.15"),
        R"({"type":"object","required":["family","style","faceIndex"],"properties":{"family":{"type":"string","minLength":1,"maxLength":1024},"style":{"type":"string","minLength":1,"maxLength":1024},"faceIndex":{"$ref":"#/$defs/unsigned32"}},"unevaluatedProperties":false})",
        "font descriptor");
    requireExact(
        schema.at("$defs").at("assetLocator-1.11"),
        R"({"type":"object","required":["kind","portability","path","relinkHint"],"properties":{"kind":{"enum":["file","font"]},"portability":{"enum":["project-relative","builtin","system"]},"path":{"type":"string","minLength":0,"maxLength":4096},"relinkHint":{"type":"string","pattern":"^(?:file|font):","maxLength":16384}},"unevaluatedProperties":false})",
        "font-aware asset locator");
    const auto& asset = schema.at("$defs").at("asset-1.11");
    requireExact(asset.at("properties").at("kind"),
                 R"({"enum":["image","sequence","audio","font"]})", "asset kind vocabulary");
    requireExact(asset.at("properties").at("font"), R"({"$ref":"#/$defs/font-1.15"})",
                 "asset font payload");
    requireExact(
        asset.at("allOf"),
        R"([{"if":{"properties":{"kind":{"const":"font"}}},"then":{"required":["font"]}}])",
        "font assets require their descriptor");
    validateReferences(schema, schema);
}
void validateManifestSchemaV1_15(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.15",
                       "manifest id");
    requireExact(schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.15"})", "manifest document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.15").at("properties").at("minor"),
                 R"({"const":15})", "manifest minor");
    validateReferences(schema, schema);
}
void validateManifestSchemaV1_14(const json::Value& schema) {
    using namespace schema_detail;
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.14",
                       "manifest id");
    requireExact(schema.at("$defs").at("document-1.0").at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.14"})", "manifest document version");
    requireExact(schema.at("$defs").at("fixedVersion-1.14").at("properties").at("minor"),
                 R"({"const":14})", "manifest minor");
    validateReferences(schema, schema);
}

} // namespace bloom::quality
