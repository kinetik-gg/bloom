#include "schema_check_helpers.hpp"
#include <algorithm>
#include <stdexcept>
namespace bloom::quality {
namespace {
void eraseVideoMember(json::Value& value, std::string_view key) {
    std::erase_if(value.asObject(), [key](const auto& member) { return member.first == key; });
}
json::Value previousVideoVersion(const json::Value& schema, bool manifest) {
    using namespace schema_detail;
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.17",
                       "video schema id");
    requireExact(schema.at("$defs").at("fixedVersion-1.17").at("properties").at("minor"),
                 R"({"const":17})", "video minor");
    auto previous = schema;
    for (auto& member : previous.at("$defs").asObject())
        if (member.first == "fixedVersion-1.17")
            member.first = "fixedVersion-1.16";
    previous.at("$defs").at("fixedVersion-1.16").at("properties").at("minor") =
        json::parse(R"({"const":16})");
    auto& version =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    requireExact(version, R"({"$ref":"#/$defs/fixedVersion-1.17"})", "video schema reference");
    version = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.16"})");
    previous.at("$id") = json::Value("urn:kinetik:bloom:schema:project-" + kind + ":1.16");
    previous.at("title") = json::Value(std::string(
        manifest ? "Bloom Project Manifest for Document 1.16" : "Bloom Project Document 1.16"));
    return previous;
}
} // namespace
void validateDocumentSchemaV1_17(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExact(
        schema.at("$defs").at("videoStream-1.17"),
        R"({"type":"object","required":["id","kind","codec","profile","pixelFormat","timecode","timebase","framePeriod","duration","width","height","sampleRate","primaries","transfer","matrix","range","channelLayout"],"properties":{"id":{"type":"integer","minimum":0,"maximum":255},"kind":{"type":"integer","minimum":1,"maximum":3},"codec":{"type":"string","maxLength":4096,"minLength":1},"profile":{"type":"string","maxLength":4096},"pixelFormat":{"type":"string","maxLength":4096},"timecode":{"type":"string","maxLength":4096},"timebase":{"$ref":"#/$defs/rationalTime"},"framePeriod":{"$ref":"#/$defs/rationalTime"},"duration":{"$ref":"#/$defs/rationalTime"},"width":{"type":"integer","minimum":1,"maximum":16384},"height":{"type":"integer","minimum":1,"maximum":16384},"sampleRate":{"type":"integer","minimum":0,"maximum":384000},"primaries":{"$ref":"#/$defs/signed64Decimal"},"transfer":{"$ref":"#/$defs/signed64Decimal"},"matrix":{"$ref":"#/$defs/signed64Decimal"},"range":{"$ref":"#/$defs/signed64Decimal"},"channelLayout":{"type":"array","maxItems":64,"items":{"type":"string","minLength":1,"maxLength":4096}}},"unevaluatedProperties":false})",
        "videoStream-1.17");
    requireExact(
        schema.at("$defs").at("video-1.17"),
        R"({"type":"object","required":["frames","duration","streams"],"properties":{"frames":{"$ref":"#/$defs/objectId"},"duration":{"$ref":"#/$defs/rationalTime"},"streams":{"type":"array","minItems":1,"maxItems":256,"items":{"$ref":"#/$defs/videoStream-1.17"}}},"unevaluatedProperties":false})",
        "video-1.17");
    requireExact(
        schema.at("$defs").at("asset-1.11").at("allOf"),
        R"([{"if":{"properties":{"kind":{"const":"font"}}},"then":{"required":["font"]}},{"if":{"properties":{"kind":{"const":"video"}}},"then":{"required":["video"]},"else":{"not":{"required":["video"]}}}])",
        "video kind condition");
    auto previous = previousVideoVersion(schema, false);
    auto& definitions = previous.at("$defs");
    eraseVideoMember(definitions, "videoStream-1.17");
    eraseVideoMember(definitions, "video-1.17");
    auto& asset = definitions.at("asset-1.11");
    requireExact(asset.at("properties").at("video"), R"({"$ref":"#/$defs/video-1.17"})",
                 "video asset property");
    requireExact(asset.at("properties").at("kind"),
                 R"({"enum":["image","sequence","audio","font","video"]})", "video kind");
    eraseVideoMember(asset.at("properties"), "video");
    asset.at("properties").at("kind") =
        json::parse(R"({"enum":["image","sequence","audio","font"]})");
    asset.at("allOf").asArray().pop_back();
    validateDocumentSchemaV1_16(previous);
}
void validateManifestSchemaV1_17(const json::Value& schema) {
    schema_detail::validateReferences(schema, schema);
    validateManifestSchemaV1_16(previousVideoVersion(schema, true));
}

namespace {
json::Value previousDataBlockVersion(const json::Value& schema, const bool manifest) {
    using namespace schema_detail;
    const std::string kind = manifest ? "manifest" : "document";
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-" + kind + ":1.18",
                       "data block schema id");
    auto previous = schema;
    auto& defs = previous.at("$defs").asObject();
    std::erase_if(defs, [](const auto& member) {
        return member.first == "fixedVersion-1.18" || member.first == "dataBlock-1.18";
    });
    if (!manifest) {
        auto& project = previous.at("$defs").at("project-1.0");
        std::erase_if(project.at("required").asArray(),
                      [](const auto& value) { return value.asString() == "dataBlocks"; });
        std::erase_if(project.at("properties").asObject(),
                      [](const auto& member) { return member.first == "dataBlocks"; });
        auto& high = previous.at("$defs").at("highestIssued-1.2");
        std::erase_if(high.at("required").asArray(),
                      [](const auto& value) { return value.asString() == "dataBlock"; });
        std::erase_if(high.at("properties").asObject(),
                      [](const auto& member) { return member.first == "dataBlock"; });
    }
    auto& version =
        manifest ? previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion")
                 : previous.at("properties").at("schemaVersion");
    version = json::parse(R"({"$ref":"#/$defs/fixedVersion-1.17"})");
    previous.at("$id") = json::Value("urn:kinetik:bloom:schema:project-" + kind + ":1.17");
    return previous;
}
} // namespace

void validateDocumentSchemaV1_18(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.18",
                       "data block document id");
    requireExact(schema.at("$defs").at("fixedVersion-1.18").at("properties").at("minor"),
                 R"({"const":18})", "data block minor");
    requireExact(
        schema.at("$defs").at("dataBlock-1.18").at("required"),
        R"(["id","kind","owner","typeId","schemaVersion","mediaType","provenance","subject","payload","tags"])",
        "data block required");
    validateDocumentSchemaV1_17(previousDataBlockVersion(schema, false));
}

void validateManifestSchemaV1_18(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.18",
                       "data block manifest id");
    requireExact(schema.at("$defs").at("fixedVersion-1.18").at("properties").at("minor"),
                 R"({"const":18})", "data block manifest minor");
    validateManifestSchemaV1_17(previousDataBlockVersion(schema, true));
}

void validateDocumentSchemaV1_19(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.19",
                       "working colour-space document id");
    requireExact(schema.at("$defs").at("fixedVersion-1.19").at("properties").at("minor"),
                 R"({"const":19})", "working colour-space minor");
    requireExact(
        schema.at("$defs").at("composition-1.0").at("properties").at("workingColorSpaceId"),
        R"({"type":"string","minLength":1,"maxLength":256})",
        "composition working colour-space override");
    auto previous = schema;
    auto& defs = previous.at("$defs");
    auto fixed =
        std::find_if(defs.asObject().begin(), defs.asObject().end(),
                     [](const auto& member) { return member.first == "fixedVersion-1.19"; });
    if (fixed == defs.asObject().end())
        throw std::runtime_error("document 1.19 fixed version definition is missing");
    fixed->first = "fixedVersion-1.18";
    fixed->second.at("properties").at("minor") = json::parse(R"({"const":18})");
    auto& composition = defs.at("composition-1.0");
    std::erase_if(composition.at("properties").asObject(),
                  [](const auto& member) { return member.first == "workingColorSpaceId"; });
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.18"));
    previous.at("title") = json::Value(std::string("Bloom Project Document 1.18"));
    previous.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.18"})");
    validateDocumentSchemaV1_18(previous);
}

void validateManifestSchemaV1_19(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.19",
                       "working colour-space manifest id");
    requireExact(schema.at("$defs").at("fixedVersion-1.19").at("properties").at("minor"),
                 R"({"const":19})", "working colour-space manifest minor");
    auto previous = schema;
    auto& defs = previous.at("$defs");
    auto fixed =
        std::find_if(defs.asObject().begin(), defs.asObject().end(),
                     [](const auto& member) { return member.first == "fixedVersion-1.19"; });
    if (fixed == defs.asObject().end())
        throw std::runtime_error("manifest 1.19 fixed version definition is missing");
    fixed->first = "fixedVersion-1.18";
    fixed->second.at("properties").at("minor") = json::parse(R"({"const":18})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.18"));
    previous.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.18"));
    previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.18"})");
    validateManifestSchemaV1_18(previous);
}

void validateDocumentSchemaV1_20(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.20",
                       "asset input colour-space document id");
    requireExact(schema.at("$defs").at("fixedVersion-1.20").at("properties").at("minor"),
                 R"({"const":20})", "asset input colour-space minor");
    const auto& interpretation =
        schema.at("$defs").at("asset-1.11").at("properties").at("interpretation");
    requireExact(interpretation.at("required"),
                 R"(["colorSpace","inputColorSpaceId","alphaAssociation"])",
                 "asset interpretation required members");
    requireExact(requireMember(interpretation.at("properties"), "inputColorSpaceId",
                               "asset interpretation properties"),
                 R"({"type":"string","maxLength":256})", "asset input colour-space id");
    auto previous = schema;
    auto& defs = previous.at("$defs");
    auto fixed =
        std::find_if(defs.asObject().begin(), defs.asObject().end(),
                     [](const auto& member) { return member.first == "fixedVersion-1.20"; });
    if (fixed == defs.asObject().end())
        throw std::runtime_error("document 1.20 fixed version definition is missing");
    fixed->first = "fixedVersion-1.19";
    fixed->second.at("properties").at("minor") = json::parse(R"({"const":19})");
    auto& previousInterpretation = defs.at("asset-1.11").at("properties").at("interpretation");
    std::erase_if(previousInterpretation.at("required").asArray(),
                  [](const auto& member) { return member.asString() == "inputColorSpaceId"; });
    std::erase_if(previousInterpretation.at("properties").asObject(),
                  [](const auto& member) { return member.first == "inputColorSpaceId"; });
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.19"));
    previous.at("title") = json::Value(std::string("Bloom Project Document 1.19"));
    previous.at("$comment") = json::Value(
        std::string("Document 1.19 adds an optional per-composition working colour-space override; "
                    "typed, provenance-carrying data blocks remain part of the current contract."));
    previous.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.19"})");
    validateDocumentSchemaV1_19(previous);
}

void validateManifestSchemaV1_20(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.20",
                       "asset input colour-space manifest id");
    requireExact(schema.at("$defs").at("fixedVersion-1.20").at("properties").at("minor"),
                 R"({"const":20})", "asset input colour-space manifest minor");
    auto previous = schema;
    auto& defs = previous.at("$defs");
    auto fixed =
        std::find_if(defs.asObject().begin(), defs.asObject().end(),
                     [](const auto& member) { return member.first == "fixedVersion-1.20"; });
    if (fixed == defs.asObject().end())
        throw std::runtime_error("manifest 1.20 fixed version definition is missing");
    fixed->first = "fixedVersion-1.19";
    fixed->second.at("properties").at("minor") = json::parse(R"({"const":19})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.19"));
    previous.at("title") = json::Value(std::string("Bloom Project Manifest for Document 1.19"));
    previous.at("$comment") =
        json::Value(std::string("Document 1.19 persists the current document version and the "
                                "manifest remains structurally unchanged."));
    previous.at("$defs").at("document-1.0").at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.19"})");
    validateManifestSchemaV1_19(previous);
}
void validateDocumentSchemaV1_21(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-document:1.21",
                       "LUT document id");
    requireExact(schema.at("$defs").at("fixedVersion-1.21").at("properties").at("minor"),
                 R"({"const":21})", "LUT document minor");
    requireExact(schema.at("$defs").at("asset-1.11").at("properties").at("kind").at("enum"),
                 R"(["image","sequence","audio","font","video","lut"])", "LUT asset kinds");
    auto previous = schema;
    auto& defs = previous.at("$defs");
    auto fixed =
        std::find_if(defs.asObject().begin(), defs.asObject().end(),
                     [](const auto& member) { return member.first == "fixedVersion-1.21"; });
    if (fixed == defs.asObject().end())
        throw std::runtime_error("document 1.21 version is missing");
    fixed->first = "fixedVersion-1.20";
    fixed->second.at("properties").at("minor") = json::parse(R"({"const":20})");
    defs.at("asset-1.11").at("properties").at("kind").at("enum") =
        json::parse(R"(["image","sequence","audio","font","video"])");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-document:1.20"));
    previous.at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.20"})");
    validateDocumentSchemaV1_20(previous);
}
void validateManifestSchemaV1_21(const json::Value& schema) {
    using namespace schema_detail;
    validateReferences(schema, schema);
    requireExactString(schema.at("$id"), "urn:kinetik:bloom:schema:project-manifest:1.21",
                       "LUT manifest id");
    requireExact(schema.at("$defs").at("fixedVersion-1.21").at("properties").at("minor"),
                 R"({"const":21})", "LUT manifest minor");
    auto previous = schema;
    auto& defs = previous.at("$defs");
    auto fixed =
        std::find_if(defs.asObject().begin(), defs.asObject().end(),
                     [](const auto& member) { return member.first == "fixedVersion-1.21"; });
    if (fixed == defs.asObject().end())
        throw std::runtime_error("manifest 1.21 version is missing");
    fixed->first = "fixedVersion-1.20";
    fixed->second.at("properties").at("minor") = json::parse(R"({"const":20})");
    previous.at("$id") = json::Value(std::string("urn:kinetik:bloom:schema:project-manifest:1.20"));
    defs.at("document-1.0").at("properties").at("schemaVersion") =
        json::parse(R"({"$ref":"#/$defs/fixedVersion-1.20"})");
    validateManifestSchemaV1_20(previous);
}
} // namespace bloom::quality
