#include "schema_check_helpers.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace bloom::quality {
namespace {
constexpr std::string_view version14 =
    R"({"type":"object","required":["major","minor"],"properties":{"major":{"const":1},"minor":{"const":4}},"unevaluatedProperties":false})";

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

// Check every constraint 1.4 adds, then reduce the artifact to its 1.3 predecessor and reuse that
// version's complete checks for everything unchanged -- the same ladder validateDocumentSchemaV1_3
// already climbs down to 1.2. Normalization stays confined to this quality-tool copy.
json::Value checkVersion14(const json::Value& schema, const std::string_view id,
                           const std::string_view title) {
    using namespace schema_detail;
    requireExactString(requireMember(schema, "$id", "$"), id, "$.$id");
    requireExactString(requireMember(schema, "title", "$"), title, "$.title");
    requireExact(requireMember(requireMember(schema, "$defs", "$"), "fixedVersion-1.4", "$.$defs"),
                 version14, "$.$defs.fixedVersion-1.4");
    validateReferences(schema, schema);
    return schema;
}
} // namespace

void validateManifestSchemaV1_4(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion14(schema, "urn:kinetik:bloom:schema:project-manifest:1.4",
                                     "Bloom Project Manifest for Document 1.4");
    auto& documentVersion =
        historical.at("$defs").at("document-1.0").at("properties").at("schemaVersion");
    requireExact(documentVersion, R"({"$ref":"#/$defs/fixedVersion-1.4"})",
                 "manifest document version");
    // The predecessor artifact is this one with its version definition one minor back, so the
    // definition is renamed and retargeted rather than removed: the manifest has no other 1.4
    // content of its own.
    documentVersion = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.3"})");
    renameMember(historical.at("$defs"), "fixedVersion-1.4", "fixedVersion-1.3");
    historical.at("$defs").at("fixedVersion-1.3").at("properties").at("minor") =
        json::parse(R"({"const":3})");
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.3"));
    historical.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.3"));
    validateManifestSchemaV1_3(historical);
}

void validateDocumentSchemaV1_4(const json::Value& schema) {
    using namespace schema_detail;
    auto historical = checkVersion14(schema, "urn:kinetik:bloom:schema:project-document:1.4",
                                     "Bloom Project Document 1.4");
    auto& definitions = historical.at("$defs");

    // 1.4's delta is two new discriminated-union arms and nothing else: a "vec3" constant value and
    // a "driver" parameter source. Neither widens an existing frozen shape, so each gets its own
    // 1.4-suffixed definition that EXTENDS the -1.0 one by reference, and the historical
    // definitions are left exactly as they shipped.
    const auto& values =
        requireArray(requireMember(requireMember(definitions, "parameterValue-1.4", "$.$defs"),
                                   "oneOf", "parameterValue-1.4"),
                     "$.$defs.parameterValue-1.4.oneOf");
    if (values.size() != 2) {
        fail(
            "$.$defs.parameterValue-1.4.oneOf must extend parameterValue-1.0 with exactly one arm");
    }
    requireExact(values[0], R"({"$ref":"#/$defs/parameterValue-1.0"})",
                 "$.$defs.parameterValue-1.4.oneOf[0]");
    requireExact(
        values[1],
        R"({"type":"object","required":["kind","x","y","z"],"properties":{"kind":{"const":"vec3"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"unevaluatedProperties":true})",
        "$.$defs.parameterValue-1.4.oneOf[1]");

    // The driver arm is checked as a shape, not as a byte-exact literal, because it is one branch
    // of a union the helper already knows how to read -- and because what matters is that it names
    // a node and a port on exactly an edge source's terms.
    static const std::array sourceSpecs{
        ExpectedBranch{"constant", {"kind", "value"}, {"kind", "value"}},
        ExpectedBranch{"animation-curve", {"kind", "curveId"}, {"kind", "curveId"}},
        ExpectedBranch{"driver",
                       {"kind", "sourceNodeId", "outputPort"},
                       {"kind", "sourceNodeId", "outputPort"}},
    };
    const auto sources =
        validateDiscriminatedUnion(definitions, "parameterSource-1.4", sourceSpecs);
    requireExact(requireMember(*sources[0], "value", "constant source"),
                 R"({"$ref":"#/$defs/parameterValue-1.4"})",
                 "$.$defs.parameterSource-1.4 constant value");
    requireExact(requireMember(*sources[2], "sourceNodeId", "driver source"),
                 R"({"$ref":"#/$defs/objectId"})", "$.$defs.parameterSource-1.4 driver node");
    requireExact(requireMember(*sources[2], "outputPort", "driver source"),
                 R"({"$ref":"#/$defs/structuralText"})", "$.$defs.parameterSource-1.4 driver port");
    requireExact(requireMember(requireMember(definitions, "parameter-1.0", "$.$defs"), "properties",
                               "parameter-1.0")
                     .at("source"),
                 R"({"$ref":"#/$defs/parameterSource-1.4"})", "$.$defs.parameter-1.0 source");
    requireExact(historical.at("properties").at("schemaVersion"),
                 R"({"$ref":"#/$defs/fixedVersion-1.4"})", "document schema version");

    // --- reduce to the 1.3 predecessor --------------------------------------------------------
    historical.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.3"})");
    renameMember(definitions, "fixedVersion-1.4", "fixedVersion-1.3");
    definitions.at("fixedVersion-1.3").at("properties").at("minor") = json::parse(R"({"const":3})");
    definitions.at("parameter-1.0").at("properties").at("source") =
        json::parse(R"({"$ref":"#/$defs/parameterSource-1.0"})");
    eraseMember(definitions, "parameterSource-1.4");
    eraseMember(definitions, "parameterValue-1.4");
    historical.at("$id") =
        json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.3"));
    historical.at("title") = json::Value(std::string("Bloom Project Document 1.3"));
    validateDocumentSchemaV1_3(historical);
}
} // namespace bloom::quality
