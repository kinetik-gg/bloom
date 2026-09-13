#include "schema_check_helpers.hpp"

#include <algorithm>
#include <string>

namespace bloom::quality {
namespace {
constexpr std::string_view version13 =
    R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":3}},"unevaluatedProperties":false})";
constexpr std::string_view legacyInterpolation = R"({"enum":["hold","linear"]})";

void eraseMember(json::Value& object, const std::string_view name) {
    std::erase_if(object.asObject(), [name](const auto& member) { return member.first == name; });
}

void renameMember(json::Value& object, const std::string_view from, const std::string_view to) {
    using namespace schema_detail;
    for (auto& member : object.asObject()) {
        if (member.first == from) {
            member.first = std::string{to};
            return;
        }
    }
    fail("$.$defs is missing required member '" + std::string{from} + "'");
}

// Check every constraint 1.3 adds, then reduce the artifact to its 1.2 predecessor and reuse that
// version's complete checks for everything unchanged -- the same ladder validateDocumentSchemaV1_2
// already climbs down to 1.1. Normalization stays confined to this quality-tool copy.
json::Value checkVersion13(const json::Value& schema, const std::string_view id,
                           const std::string_view title) {
    using namespace schema_detail;
    requireExactString(requireMember(schema, "$id", "$"), id, "$.$id");
    requireExactString(requireMember(schema, "title", "$"), title, "$.title");
    requireExact(requireMember(requireMember(schema, "$defs", "$"), "fixedVersion-1.3", "$.$defs"),
                 version13, "$.$defs.fixedVersion-1.3");
    validateReferences(schema, schema);
    return schema;
}
} // namespace

void validateManifestSchemaV1_3(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion13(schema, "urn:kinetik:bloom:schema:project-manifest:1.3",
                                     "Bloom Project Manifest for Document 1.3");
    auto& documentVersion =
        historical.at("$defs").at("document-1.0").at("properties").at("schemaVersion");
    requireExact(documentVersion, R"({"$ref":"#/$defs/fixedVersion-1.3"})",
                 "manifest document version");
    // The predecessor artifact is this one with its version definition one minor back, so the
    // definition is renamed and retargeted rather than removed: the manifest has no other 1.3
    // content of its own.
    documentVersion = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.2"})");
    renameMember(historical.at("$defs"), "fixedVersion-1.3", "fixedVersion-1.2");
    historical.at("$defs").at("fixedVersion-1.2").at("properties").at("minor") =
        json::parse(R"({"const":2})");
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.2"));
    historical.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.2"));
    validateManifestSchemaV1_2(historical);
}

void validateDocumentSchemaV1_3(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion13(schema, "urn:kinetik:bloom:schema:project-document:1.3",
                                     "Bloom Project Document 1.3");
    auto& definitions = historical.at("$defs");

    // 1.3's delta is entirely in the animation VALUE space: a third curve kind and a third
    // interpolation token. Neither widens an existing frozen shape, so each gets its own
    // 1.3-suffixed definition and the historical -1.0 ones are left exactly as they shipped.
    requireExact(
        requireMember(definitions, "keyframeInterpolation-1.3", "$.$defs"),
        R"({"$comment":"ease-in-out is a cubic Bezier ease with fixed symmetric handles; see animation-and-time.md.","enum":["hold","linear","ease-in-out"]})",
        "$.$defs.keyframeInterpolation-1.3");
    const auto& color4Value =
        validateObjectShape(definitions, "color4Value-1.3", {"red", "green", "blue", "alpha"},
                            {"red", "green", "blue", "alpha"});
    for (const std::string_view channel : {"red", "green", "blue"}) {
        requireExact(requireMember(color4Value, channel, "color4Value-1.3"), R"({"type":"number"})",
                     "$.$defs.color4Value-1.3.properties." + std::string{channel});
    }
    // Alpha carries the authoring-colour unit domain, exactly as core::Color4d::isValid() does.
    requireExact(requireMember(color4Value, "alpha", "color4Value-1.3"),
                 R"({"type":"number","minimum":0,"maximum":1})",
                 "$.$defs.color4Value-1.3.properties.alpha");
    static_cast<void>(validateObjectShape(definitions, "color4Keyframe-1.3",
                                          {"id", "time", "value", "outgoingInterpolation"},
                                          {"id", "time", "value", "outgoingInterpolation"}));
    for (const std::string_view keyframe :
         {"scalarKeyframe-1.3", "vec2Keyframe-1.3", "color4Keyframe-1.3"}) {
        const auto& properties =
            requireMember(requireMember(definitions, keyframe, "$.$defs"), "properties", keyframe);
        requireExact(requireMember(properties, "outgoingInterpolation", keyframe),
                     R"({"$ref":"#/$defs/keyframeInterpolation-1.3"})",
                     "$.$defs." + std::string{keyframe} + ".properties.outgoingInterpolation");
    }
    static const std::array curveSpecs{
        ExpectedBranch{"scalar", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
        ExpectedBranch{"vec2", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
        ExpectedBranch{"color4", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
    };
    const auto curves = validateDiscriminatedUnion(definitions, "animationCurve-1.3", curveSpecs);
    validateArray(requireMember(*curves[2], "keyframes", "color4 curve"),
                  "$.$defs.animationCurve-1.3 color4 keys", "#/$defs/color4Keyframe-1.3", 1'000'000,
                  1, true);
    requireExact(historical.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.3"})", "document schema version");

    // --- reduce to the 1.2 predecessor --------------------------------------------------------
    historical.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.2"})");
    renameMember(definitions, "fixedVersion-1.3", "fixedVersion-1.2");
    definitions.at("fixedVersion-1.2").at("properties").at("minor") = json::parse(R"({"const":2})");
    definitions.at("composition-1.0").at("properties").at("animationCurves").at("items") =
        json::parse(R"({"$ref":"#/$defs/animationCurve-1.0"})");
    renameMember(definitions, "animationCurve-1.3", "animationCurve-1.0");
    renameMember(definitions, "scalarKeyframe-1.3", "scalarKeyframe-1.0");
    renameMember(definitions, "vec2Keyframe-1.3", "vec2Keyframe-1.0");
    auto& branches = definitions.at("animationCurve-1.0").at("oneOf").asArray();
    branches.pop_back();
    branches[0].at("properties").at("keyframes").at("items") =
        json::parse(R"({"$ref":"#/$defs/scalarKeyframe-1.0"})");
    branches[1].at("properties").at("keyframes").at("items") =
        json::parse(R"({"$ref":"#/$defs/vec2Keyframe-1.0"})");
    for (const std::string_view keyframe : {"scalarKeyframe-1.0", "vec2Keyframe-1.0"}) {
        definitions.at(keyframe).at("properties").at("outgoingInterpolation") =
            json::parse(std::string{legacyInterpolation});
    }
    eraseMember(definitions, "color4Keyframe-1.3");
    eraseMember(definitions, "color4Value-1.3");
    eraseMember(definitions, "keyframeInterpolation-1.3");
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.2"));
    historical.at("title") = json::Value(std::string("Bloom Project Document 1.2"));
    validateDocumentSchemaV1_2(historical);
}
} // namespace bloom::quality
