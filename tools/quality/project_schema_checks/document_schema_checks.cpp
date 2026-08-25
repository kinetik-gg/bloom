#include "schema_check_helpers.hpp"

#include <array>
#include <string>
#include <string_view>

namespace bloom::quality {
namespace {

using schema_detail::ExpectedBranch;

constexpr std::string_view draft202012 = "https://json-schema.org/draft/2020-12/schema";
constexpr std::string_view documentId = "urn:kinetik:bloom:schema:project-document:1.0";
constexpr std::string_view identifierPattern = "^[a-z0-9][a-z0-9._-]{0,127}$";
constexpr std::string_view objectIdPattern =
    "^(?:[1-9][0-9]{0,18}|1[0-7][0-9]{18}|18[0-3][0-9]{17}|184[0-3][0-9]{16}|"
    "1844[0-5][0-9]{15}|18446[0-6][0-9]{14}|184467[0-3][0-9]{13}|"
    "1844674[0-3][0-9]{12}|184467440[0-6][0-9]{10}|1844674407[0-2][0-9]{9}|"
    "18446744073[0-6][0-9]{8}|1844674407370[0-8][0-9]{6}|"
    "18446744073709[0-4][0-9]{5}|184467440737095[0-4][0-9]{4}|"
    "1844674407370955[0][0-9]{3}|18446744073709551[0-5][0-9]{2}|"
    "184467440737095516[0][0-9]|1844674407370955161[0-5])$";
constexpr std::string_view allocatorPattern =
    "^(?:0|[1-9][0-9]{0,18}|1[0-7][0-9]{18}|18[0-3][0-9]{17}|184[0-3][0-9]{16}|"
    "1844[0-5][0-9]{15}|18446[0-6][0-9]{14}|184467[0-3][0-9]{13}|"
    "1844674[0-3][0-9]{12}|184467440[0-6][0-9]{10}|1844674407[0-2][0-9]{9}|"
    "18446744073[0-6][0-9]{8}|1844674407370[0-8][0-9]{6}|"
    "18446744073709[0-4][0-9]{5}|184467440737095[0-4][0-9]{4}|"
    "1844674407370955[0][0-9]{3}|18446744073709551[0-5][0-9]{2}|"
    "184467440737095516[0][0-9]|1844674407370955161[0-5])$";
constexpr std::string_view positiveSigned64Body =
    "[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[0-1][0-9]{17}|92[0-1][0-9]{16}|"
    "922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|"
    "922337[0-1][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|"
    "9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|"
    "922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|"
    "92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|"
    "9223372036854775[0-7][0-9]{2}|922337203685477580[0-7]";
constexpr std::string_view positiveUnsigned32Pattern =
    "^(?:[1-9][0-9]{0,8}|[1-3][0-9]{9}|4[0-1][0-9]{8}|42[0-8][0-9]{7}|"
    "429[0-3][0-9]{6}|4294[0-8][0-9]{5}|42949[0-5][0-9]{4}|"
    "429496[0-6][0-9]{3}|4294967[0-1][0-9]{2}|42949672[0-8][0-9]|"
    "429496729[0-5])$";

void requirePatternDefinition(const json::Value& definitions, const std::string_view name,
                              const std::string_view pattern) {
    using namespace schema_detail;
    const auto location = "$.$defs." + std::string{name};
    const auto& definition = requireMember(definitions, name, "$.$defs");
    requireExactKeys(definition, {"type", "pattern"}, location + " keys");
    requireExactString(requireMember(definition, "type", location), "string", location + ".type");
    requireExactString(requireMember(definition, "pattern", location), pattern,
                       location + ".pattern");
}

void validateTextDefinitions(const json::Value& definitions) {
    using namespace schema_detail;
    for (const auto [name, maximumLabel] :
         {std::pair{"humanFacingName", "4096-byte"}, std::pair{"structuralText", "256-byte"}}) {
        const auto location = "$.$defs." + std::string{name};
        const auto& value = requireMember(definitions, name, "$.$defs");
        requireExactKeys(value, {"$comment", "type", "minLength"}, location + " keys");
        requireExactString(requireMember(value, "type", location), "string", location + ".type");
        requireExactInteger(requireMember(value, "minLength", location), 1,
                            location + ".minLength");
        const auto& comment =
            requireString(requireMember(value, "$comment", location), location + ".$comment");
        if (comment.find(maximumLabel) == std::string::npos || value.find("maxLength") != nullptr) {
            fail(location + " must leave its UTF-8 byte ceiling to Project I/O");
        }
    }
    requireExact(requireMember(definitions, "namespacedIdentifier", "$.$defs"),
                 "{\"type\":\"string\",\"minLength\":1,\"maxLength\":128,\"pattern\":\"" +
                     std::string{identifierPattern} + "\"}",
                 "$.$defs.namespacedIdentifier");
}

void validateRationals(const json::Value& definitions) {
    using namespace schema_detail;
    struct RationalSpec final {
        std::string_view name;
        std::string_view numerator;
        std::string_view denominator;
    };
    constexpr std::array specs{
        RationalSpec{"rationalTime", "#/$defs/signed64Decimal", "#/$defs/positiveSigned64Decimal"},
        RationalSpec{"positiveRationalTime", "#/$defs/positiveSigned64Decimal",
                     "#/$defs/positiveSigned64Decimal"},
        RationalSpec{"unsignedRatio", "#/$defs/positiveUnsigned32Decimal",
                     "#/$defs/positiveUnsigned32Decimal"},
    };
    for (const auto& spec : specs) {
        const auto& rational = validateObjectShape(
            definitions, spec.name, {"numerator", "denominator"}, {"numerator", "denominator"});
        requireExact(requireMember(rational, "numerator", spec.name),
                     "{\"$ref\":\"" + std::string{spec.numerator} + "\"}",
                     "$.$defs." + std::string{spec.name} + ".properties.numerator");
        requireExact(requireMember(rational, "denominator", spec.name),
                     "{\"$ref\":\"" + std::string{spec.denominator} + "\"}",
                     "$.$defs." + std::string{spec.name} + ".properties.denominator");
    }
}

void validateColorDefinitions(const json::Value& definitions) {
    using namespace schema_detail;
    const auto& color = validateObjectShape(definitions, "colorSettings-1.0",
                                            {"schemaVersion", "processColorSpaceId", "ocioConfig"},
                                            {"schemaVersion", "processColorSpaceId", "ocioConfig"});
    requireExact(requireMember(color, "schemaVersion", "colorSettings"),
                 R"({"$ref":"#/$defs/fixedVersion-1.0"})",
                 "$.$defs.colorSettings-1.0.properties.schemaVersion");
    requireExact(requireMember(color, "processColorSpaceId", "colorSettings"),
                 R"({"const":"lin_rec709_scene"})",
                 "$.$defs.colorSettings-1.0.properties.processColorSpaceId");
    requireExact(requireMember(color, "ocioConfig", "colorSettings"),
                 R"({"$ref":"#/$defs/ocioConfigReference-1.0"})",
                 "$.$defs.colorSettings-1.0.properties.ocioConfig");

    const auto& reference = validateObjectShape(
        definitions, "ocioConfigReference-1.0",
        {"schemaVersion", "locator", "expectedRevision", "portability", "contextVariables"},
        {"schemaVersion", "locator", "expectedRevision", "portability", "contextVariables"});
    requireExact(requireMember(reference, "portability", "ocioConfigReference"),
                 R"({"enum":["builtin","project-relative","external"]})",
                 "$.$defs.ocioConfigReference-1.0.properties.portability");
    validateArray(requireMember(reference, "contextVariables", "ocioConfigReference"),
                  "$.$defs.ocioConfigReference-1.0.properties.contextVariables",
                  "#/$defs/ocioContextVariable-1.0", 256);

    const std::array locatorSpecs{
        ExpectedBranch{"builtin", {"kind", "uri"}, {"kind", "uri"}},
        ExpectedBranch{"project-relative-ocioz", {"kind", "path"}, {"kind", "path"}},
        ExpectedBranch{"external-ocioz", {"kind", "uri"}, {"kind", "uri"}},
        ExpectedBranch{"external-config", {"kind", "uri"}, {"kind", "uri"}},
    };
    const auto locators =
        validateDiscriminatedUnion(definitions, "ocioConfigLocator-1.0", locatorSpecs);
    requireExact(requireMember(*locators[0], "uri", "builtin locator"),
                 R"({"const":"bloom://ocio/neutral-v1/config.ocio"})",
                 "$.$defs.ocioConfigLocator-1.0 builtin URI");
    requireExact(
        requireMember(*locators[1], "path", "project-relative locator"),
        R"schema({"$comment":"The 4096-byte UTF-8 ceiling and complete normalized-component profile are Project I/O checks.","type":"string","minLength":1,"allOf":[{"pattern":"^(?!/)(?![A-Za-z]:)(?!.*\\\\)(?!.*//)[^\\u0000]+$"},{"pattern":"\\.ocioz$"}]})schema",
        "$.$defs.ocioConfigLocator-1.0 project-relative-ocioz path");
    for (const auto index : {2U, 3U}) {
        requireExact(requireMember(*locators[index], "uri", "external locator"),
                     R"({"$ref":"#/$defs/externalFileUri"})",
                     "$.$defs.ocioConfigLocator-1.0 external URI");
    }
    requireExact(
        requireMember(definitions, "externalFileUri", "$.$defs"),
        R"schema({"$comment":"Project I/O enforces the exact RFC 8089 subset, percent-escape rules, and decoded .ocioz/config.ocio leaf name.","type":"string","minLength":1,"maxLength":16384,"allOf":[{"pattern":"^[\\u0001-\\u007f]+$"},{"pattern":"^[Ff][Ii][Ll][Ee]:"},{"pattern":"^[^?#]*$"}]})schema",
        "$.$defs.externalFileUri");

    const auto& revision = validateObjectShape(definitions, "ocioConfigRevision-1.0",
                                               {"algorithm", "digest"}, {"algorithm", "digest"});
    requireExact(requireMember(revision, "algorithm", "revision"), R"({"const":"sha256"})",
                 "$.$defs.ocioConfigRevision-1.0.properties.algorithm");
    requireExact(requireMember(revision, "digest", "revision"),
                 R"({"type":"string","minLength":64,"maxLength":64,"pattern":"^[0-9a-f]{64}$"})",
                 "$.$defs.ocioConfigRevision-1.0.properties.digest");

    const auto& context = validateObjectShape(definitions, "ocioContextVariable-1.0",
                                              {"name", "value"}, {"name", "value"});
    requireExact(
        requireMember(context, "name", "context"),
        R"({"type":"string","minLength":1,"maxLength":128,"pattern":"^[A-Za-z_][A-Za-z0-9_]{0,127}$"})",
        "$.$defs.ocioContextVariable-1.0.properties.name");
    requireExact(
        requireMember(context, "value", "context"),
        R"schema({"$comment":"The normative 4096-byte UTF-8 ceiling is enforced by Project I/O.","type":"string","pattern":"^[^\\u0000]*$"})schema",
        "$.$defs.ocioContextVariable-1.0.properties.value");
}

} // namespace

void validateDocumentSchema(const json::Value& schema) {
    using namespace schema_detail;
    requireExactKeys(schema,
                     {"$schema", "$id", "title", "$comment", "type", "required", "properties",
                      "$defs", "unevaluatedProperties"},
                     "document schema root keys");
    requireExactString(requireMember(schema, "$schema", "$"), draft202012, "$.$schema");
    requireExactString(requireMember(schema, "$id", "$"), documentId, "$.$id");
    requireExactString(requireMember(schema, "title", "$"), "Bloom Project Document 1.0",
                       "$.title");
    static_cast<void>(requireString(requireMember(schema, "$comment", "$"), "$.$comment"));
    requireExactString(requireMember(schema, "type", "$"), "object", "$.type");
    requireExactStringArray(requireMember(schema, "required", "$"),
                            {"schemaVersion", "project", "idAllocation", "extensions"},
                            "$.required");
    requireExactBoolean(requireMember(schema, "unevaluatedProperties", "$"), true,
                        "$.unevaluatedProperties");

    const auto& properties = requireMember(schema, "properties", "$");
    requireExactKeys(properties, {"schemaVersion", "project", "idAllocation", "extensions"},
                     "$.properties keys");
    requireExact(requireMember(properties, "schemaVersion", "$.properties"),
                 R"({"$ref":"#/$defs/fixedVersion-1.0"})", "$.properties.schemaVersion");
    requireExact(requireMember(properties, "project", "$.properties"),
                 R"({"$ref":"#/$defs/project-1.0"})", "$.properties.project");
    requireExact(requireMember(properties, "idAllocation", "$.properties"),
                 R"({"$ref":"#/$defs/idAllocation-1.0"})", "$.properties.idAllocation");
    validateArray(requireMember(properties, "extensions", "$.properties"),
                  "$.properties.extensions", "#/$defs/extensionRecord-1.0");

    const auto& definitions = requireMember(schema, "$defs", "$");
    requireExactKeys(definitions,
                     {"unsigned32",
                      "positiveUnsigned32",
                      "fixedVersion-1.0",
                      "schemaVersion-1.0",
                      "objectId",
                      "allocatorHighWater",
                      "signed64Decimal",
                      "positiveSigned64Decimal",
                      "positiveUnsigned32Decimal",
                      "humanFacingName",
                      "structuralText",
                      "namespacedIdentifier",
                      "rationalTime",
                      "positiveRationalTime",
                      "unsignedRatio",
                      "project-1.0",
                      "composition-1.0",
                      "compositionFormat-1.0",
                      "colorSettings-1.0",
                      "ocioConfigReference-1.0",
                      "ocioConfigLocator-1.0",
                      "externalFileUri",
                      "ocioConfigRevision-1.0",
                      "ocioContextVariable-1.0",
                      "parameter-1.0",
                      "parameterSource-1.0",
                      "parameterValue-1.0",
                      "animationCurve-1.0",
                      "scalarKeyframe-1.0",
                      "vec2Keyframe-1.0",
                      "vec2Value-1.0",
                      "graph-1.0",
                      "node-1.0",
                      "parameterBinding-1.0",
                      "edge-1.0",
                      "outputPortReference-1.0",
                      "inputPortReference-1.0",
                      "layerOutput-1.0",
                      "layerStack-1.0",
                      "layerStackEntry-1.0",
                      "idAllocation-1.0",
                      "highestIssued-1.0",
                      "extensionRecord-1.0",
                      "extensionTarget-1.0",
                      "extensionReferencePolicy-1.0",
                      "extensionHostReference-1.0",
                      "canonicalBase64"},
                     "$.$defs keys");
    requireExact(requireMember(definitions, "unsigned32", "$.$defs"),
                 R"({"type":"integer","minimum":0,"maximum":4294967295})", "$.$defs.unsigned32");
    requireExact(requireMember(definitions, "positiveUnsigned32", "$.$defs"),
                 R"({"type":"integer","minimum":1,"maximum":4294967295})",
                 "$.$defs.positiveUnsigned32");
    validateVersionDefinition(requireMember(definitions, "fixedVersion-1.0", "$.$defs"),
                              "$.$defs.fixedVersion-1.0", true);
    validateVersionDefinition(requireMember(definitions, "schemaVersion-1.0", "$.$defs"),
                              "$.$defs.schemaVersion-1.0", false, "#/$defs/positiveUnsigned32");

    requirePatternDefinition(definitions, "objectId", objectIdPattern);
    requirePatternDefinition(definitions, "allocatorHighWater", allocatorPattern);
    requirePatternDefinition(definitions, "positiveSigned64Decimal",
                             "^(?:" + std::string{positiveSigned64Body} + ")$");
    requirePatternDefinition(
        definitions, "signed64Decimal",
        "^(?:0|" + std::string{positiveSigned64Body} +
            "|-(?:[1-9][0-9]{0,17}|[1-8][0-9]{18}|9[0-1][0-9]{17}|92[0-1][0-9]{16}|"
            "922[0-2][0-9]{15}|9223[0-2][0-9]{14}|92233[0-6][0-9]{13}|"
            "922337[0-1][0-9]{12}|92233720[0-2][0-9]{10}|922337203[0-5][0-9]{9}|"
            "9223372036[0-7][0-9]{8}|92233720368[0-4][0-9]{7}|"
            "922337203685[0-3][0-9]{6}|9223372036854[0-6][0-9]{5}|"
            "92233720368547[0-6][0-9]{4}|922337203685477[0-4][0-9]{3}|"
            "9223372036854775[0-7][0-9]{2}|922337203685477580[0-8]))$");
    requirePatternDefinition(definitions, "positiveUnsigned32Decimal", positiveUnsigned32Pattern);
    validateTextDefinitions(definitions);
    validateRationals(definitions);

    const auto& project = validateObjectShape(definitions, "project-1.0",
                                              {"id", "name", "colorSettings", "compositions"},
                                              {"id", "name", "colorSettings", "compositions"});
    requireExact(requireMember(project, "colorSettings", "project"),
                 R"({"$ref":"#/$defs/colorSettings-1.0"})",
                 "$.$defs.project-1.0.properties.colorSettings");
    validateArray(requireMember(project, "compositions", "project"),
                  "$.$defs.project-1.0.properties.compositions", "#/$defs/composition-1.0");

    const auto& composition = validateObjectShape(
        definitions, "composition-1.0",
        {"id", "name", "duration", "format", "parameters", "animationCurves", "graph"},
        {"id", "name", "duration", "format", "parameters", "animationCurves", "graph"});
    requireExact(requireMember(composition, "duration", "composition"),
                 R"({"$ref":"#/$defs/positiveRationalTime"})",
                 "$.$defs.composition-1.0.properties.duration");
    validateArray(requireMember(composition, "parameters", "composition"),
                  "$.$defs.composition-1.0.properties.parameters", "#/$defs/parameter-1.0");
    validateArray(requireMember(composition, "animationCurves", "composition"),
                  "$.$defs.composition-1.0.properties.animationCurves",
                  "#/$defs/animationCurve-1.0");

    const auto& format = validateObjectShape(definitions, "compositionFormat-1.0",
                                             {"width", "height", "pixelAspect", "frameRate"},
                                             {"width", "height", "pixelAspect", "frameRate"});
    for (const std::string_view dimension : {"width", "height"}) {
        requireExact(requireMember(format, dimension, "composition format"),
                     R"({"type":"integer","minimum":1,"maximum":1048576})",
                     "$.$defs.compositionFormat-1.0.properties." + std::string{dimension});
    }

    validateColorDefinitions(definitions);

    static const std::array sourceSpecs{
        ExpectedBranch{"constant", {"kind", "value"}, {"kind", "value"}},
        ExpectedBranch{"animation-curve", {"kind", "curveId"}, {"kind", "curveId"}},
    };
    static_cast<void>(validateObjectShape(definitions, "parameter-1.0",
                                          {"id", "schemaKey", "source"},
                                          {"id", "schemaKey", "source"}));
    const auto sources =
        validateDiscriminatedUnion(definitions, "parameterSource-1.0", sourceSpecs);
    requireExact(requireMember(*sources[0], "value", "constant source"),
                 R"({"$ref":"#/$defs/parameterValue-1.0"})",
                 "$.$defs.parameterSource-1.0 constant value");
    requireExact(requireMember(*sources[1], "curveId", "animation source"),
                 R"({"$ref":"#/$defs/objectId"})",
                 "$.$defs.parameterSource-1.0 animation-curve curveId");

    static const std::array parameterValueSpecs{
        ExpectedBranch{"bool", {"kind", "value"}, {"kind", "value"}},
        ExpectedBranch{"int64", {"kind", "value"}, {"kind", "value"}},
        ExpectedBranch{"float64", {"kind", "value"}, {"kind", "value"}},
        ExpectedBranch{"vec2", {"kind", "x", "y"}, {"kind", "x", "y"}},
        ExpectedBranch{"color4",
                       {"kind", "red", "green", "blue", "alpha"},
                       {"kind", "red", "green", "blue", "alpha"}},
        ExpectedBranch{"string", {"kind", "value"}, {"kind", "value"}},
        ExpectedBranch{
            "rational", {"kind", "numerator", "denominator"}, {"kind", "numerator", "denominator"}},
    };
    const auto parameterValues =
        validateDiscriminatedUnion(definitions, "parameterValue-1.0", parameterValueSpecs);
    requireExact(requireMember(*parameterValues[0], "value", "bool value"), R"({"type":"boolean"})",
                 "$.$defs.parameterValue-1.0 bool value");
    requireExact(requireMember(*parameterValues[1], "value", "int64 value"),
                 R"({"$ref":"#/$defs/signed64Decimal"})", "$.$defs.parameterValue-1.0 int64 value");
    requireExact(requireMember(*parameterValues[2], "value", "float64 value"),
                 R"({"type":"number"})", "$.$defs.parameterValue-1.0 float64 value");

    static const std::array curveSpecs{
        ExpectedBranch{"scalar", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
        ExpectedBranch{"vec2", {"id", "kind", "keyframes"}, {"id", "kind", "keyframes"}},
    };
    const auto curves = validateDiscriminatedUnion(definitions, "animationCurve-1.0", curveSpecs);
    validateArray(requireMember(*curves[0], "keyframes", "scalar curve"),
                  "$.$defs.animationCurve-1.0 scalar keys", "#/$defs/scalarKeyframe-1.0", 1'000'000,
                  1, true);
    validateArray(requireMember(*curves[1], "keyframes", "vec2 curve"),
                  "$.$defs.animationCurve-1.0 vec2 keys", "#/$defs/vec2Keyframe-1.0", 1'000'000, 1,
                  true);

    validateDocumentGraphAndReferences(definitions, parameterValues, curves);
    validateReferences(schema, schema);
}

void checkProjectSchemas(const std::filesystem::path& repositoryRoot) {
    const auto repository = std::filesystem::weakly_canonical(repositoryRoot);
    validateManifestSchema(
        json::parseFile(repository / "schemas/project/manifest-1.0.schema.json"));
    validateDocumentSchema(
        json::parseFile(repository / "schemas/project/document-1.0.schema.json"));
}

} // namespace bloom::quality
