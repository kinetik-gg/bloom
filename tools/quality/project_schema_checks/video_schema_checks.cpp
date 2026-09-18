#include "schema_check_helpers.hpp"
#include <algorithm>
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
} // namespace bloom::quality
