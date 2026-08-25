#include "schema_check_helpers.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::quality::schema_detail {
namespace {

void requireReference(const json::Value& properties, const std::string_view property,
                      const std::string_view target, const std::string_view location) {
    requireExact(requireMember(properties, property, location),
                 "{\"$ref\":\"#/$defs/" + std::string{target} + "\"}",
                 std::string{location} + '.' + std::string{property});
}

void validateGraphDefinitions(const json::Value& definitions) {
    const auto& graph =
        validateObjectShape(definitions, "graph-1.0",
                            {"nodes", "edges", "layerOutputs", "layerStack", "compositionOutput"},
                            {"nodes", "edges", "layerOutputs", "layerStack", "compositionOutput"});
    validateArray(requireMember(graph, "nodes", "graph"), "$.$defs.graph-1.0.properties.nodes",
                  "#/$defs/node-1.0");
    validateArray(requireMember(graph, "edges", "graph"), "$.$defs.graph-1.0.properties.edges",
                  "#/$defs/edge-1.0");
    validateArray(requireMember(graph, "layerOutputs", "graph"),
                  "$.$defs.graph-1.0.properties.layerOutputs", "#/$defs/layerOutput-1.0");

    const auto& node = validateObjectShape(definitions, "node-1.0",
                                           {"id", "typeId", "schemaVersion", "parameters"},
                                           {"id", "typeId", "schemaVersion", "parameters"});
    validateArray(requireMember(node, "parameters", "node"),
                  "$.$defs.node-1.0.properties.parameters", "#/$defs/parameterBinding-1.0");
    static_cast<void>(validateObjectShape(definitions, "parameterBinding-1.0",
                                          {"role", "parameterId"}, {"role", "parameterId"}));
    static_cast<void>(validateObjectShape(definitions, "edge-1.0", {"id", "source", "destination"},
                                          {"id", "source", "destination"}));
    static_cast<void>(validateObjectShape(definitions, "outputPortReference-1.0",
                                          {"nodeId", "port"}, {"nodeId", "port"}));

    const std::array inputSpecs{
        ExpectedBranch{"node-input", {"kind", "nodeId", "port"}, {"kind", "nodeId", "port"}},
        ExpectedBranch{"layer-stack-input",
                       {"kind", "stackNodeId", "slotId", "role"},
                       {"kind", "stackNodeId", "slotId", "role"}},
    };
    static_cast<void>(
        validateDiscriminatedUnion(definitions, "inputPortReference-1.0", inputSpecs));
    static_cast<void>(validateObjectShape(definitions, "layerOutput-1.0",
                                          {"nodeId", "layerId", "name", "outputPort"},
                                          {"nodeId", "layerId", "name", "outputPort"}));
    const auto& layerStack = validateObjectShape(definitions, "layerStack-1.0",
                                                 {"nodeId", "entries"}, {"nodeId", "entries"});
    validateArray(requireMember(layerStack, "entries", "layer stack"),
                  "$.$defs.layerStack-1.0.properties.entries", "#/$defs/layerStackEntry-1.0");
    static_cast<void>(validateObjectShape(definitions, "layerStackEntry-1.0", {"slotId", "layerId"},
                                          {"slotId", "layerId"}));
}

void validateAllocatorAndExtensions(const json::Value& definitions) {
    static_cast<void>(
        validateObjectShape(definitions, "idAllocation-1.0", {"highestIssued"}, {"highestIssued"}));
    constexpr std::array watermarkNames{
        std::string_view{"composition"},    std::string_view{"node"},
        std::string_view{"edge"},           std::string_view{"layer"},
        std::string_view{"layerSlot"},      std::string_view{"parameter"},
        std::string_view{"animationCurve"}, std::string_view{"keyframe"},
        std::string_view{"driverBinding"},  std::string_view{"extensionRecord"},
    };
    const auto& watermarks =
        validateObjectShape(definitions, "highestIssued-1.0",
                            {"composition", "node", "edge", "layer", "layerSlot", "parameter",
                             "animationCurve", "keyframe", "driverBinding", "extensionRecord"},
                            {"composition", "node", "edge", "layer", "layerSlot", "parameter",
                             "animationCurve", "keyframe", "driverBinding", "extensionRecord"});
    for (const auto name : watermarkNames) {
        requireReference(watermarks, name, "allocatorHighWater",
                         "$.$defs.highestIssued-1.0.properties");
    }

    const auto& extension =
        validateObjectShape(definitions, "extensionRecord-1.0",
                            {"id", "ownerId", "typeId", "schemaVersion", "subject", "mediaType",
                             "referencePolicy", "payload"},
                            {"id", "ownerId", "typeId", "schemaVersion", "subject", "mediaType",
                             "referencePolicy", "payload"});
    requireExact(requireMember(extension, "subject", "extension"),
                 R"({"oneOf":[{"type":"null"},{"$ref":"#/$defs/extensionTarget-1.0"}]})",
                 "$.$defs.extensionRecord-1.0.properties.subject");
    const auto& target =
        validateObjectShape(definitions, "extensionTarget-1.0", {"kind", "id"}, {"kind", "id"});
    requireExact(
        requireMember(target, "kind", "extension target"),
        R"({"enum":["project","composition","node","edge","layer","layer-slot","parameter","animation-curve","keyframe"]})",
        "$.$defs.extensionTarget-1.0.properties.kind");

    const std::array policySpecs{
        ExpectedBranch{"none", {"kind"}, {"kind"}},
        ExpectedBranch{"host-table", {"kind", "references"}, {"kind", "references"}},
        ExpectedBranch{
            "owner-remapper", {"kind", "remapperId", "version"}, {"kind", "remapperId", "version"}},
    };
    static_cast<void>(
        validateDiscriminatedUnion(definitions, "extensionReferencePolicy-1.0", policySpecs));
    static_cast<void>(validateObjectShape(definitions, "extensionHostReference-1.0",
                                          {"key", "target"}, {"key", "target"}));

    requireExact(
        requireMember(definitions, "canonicalBase64", "$.$defs"),
        R"schema({"$comment":"Project I/O rejects nonzero unused tail bits and checks the decoded per-record and aggregate payload ceilings before allocation.","type":"string","maxLength":89478488,"pattern":"^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$"})schema",
        "$.$defs.canonicalBase64");
}

void validateDirectReferences(const json::Value& definitions) {
    struct ReferenceSpec final {
        std::string_view definition;
        std::string_view property;
        std::string_view target;
    };
    constexpr std::array references{
        ReferenceSpec{"project-1.0", "id", "objectId"},
        ReferenceSpec{"project-1.0", "name", "humanFacingName"},
        ReferenceSpec{"project-1.0", "colorSettings", "colorSettings-1.0"},
        ReferenceSpec{"composition-1.0", "id", "objectId"},
        ReferenceSpec{"composition-1.0", "name", "humanFacingName"},
        ReferenceSpec{"composition-1.0", "duration", "positiveRationalTime"},
        ReferenceSpec{"composition-1.0", "format", "compositionFormat-1.0"},
        ReferenceSpec{"composition-1.0", "graph", "graph-1.0"},
        ReferenceSpec{"compositionFormat-1.0", "pixelAspect", "unsignedRatio"},
        ReferenceSpec{"compositionFormat-1.0", "frameRate", "unsignedRatio"},
        ReferenceSpec{"colorSettings-1.0", "schemaVersion", "fixedVersion-1.0"},
        ReferenceSpec{"colorSettings-1.0", "ocioConfig", "ocioConfigReference-1.0"},
        ReferenceSpec{"ocioConfigReference-1.0", "schemaVersion", "fixedVersion-1.0"},
        ReferenceSpec{"ocioConfigReference-1.0", "locator", "ocioConfigLocator-1.0"},
        ReferenceSpec{"ocioConfigReference-1.0", "expectedRevision", "ocioConfigRevision-1.0"},
        ReferenceSpec{"parameter-1.0", "id", "objectId"},
        ReferenceSpec{"parameter-1.0", "schemaKey", "structuralText"},
        ReferenceSpec{"parameter-1.0", "source", "parameterSource-1.0"},
        ReferenceSpec{"scalarKeyframe-1.0", "id", "objectId"},
        ReferenceSpec{"scalarKeyframe-1.0", "time", "rationalTime"},
        ReferenceSpec{"vec2Keyframe-1.0", "id", "objectId"},
        ReferenceSpec{"vec2Keyframe-1.0", "time", "rationalTime"},
        ReferenceSpec{"vec2Keyframe-1.0", "value", "vec2Value-1.0"},
        ReferenceSpec{"graph-1.0", "layerStack", "layerStack-1.0"},
        ReferenceSpec{"graph-1.0", "compositionOutput", "outputPortReference-1.0"},
        ReferenceSpec{"node-1.0", "id", "objectId"},
        ReferenceSpec{"node-1.0", "typeId", "namespacedIdentifier"},
        ReferenceSpec{"node-1.0", "schemaVersion", "positiveUnsigned32"},
        ReferenceSpec{"parameterBinding-1.0", "role", "structuralText"},
        ReferenceSpec{"parameterBinding-1.0", "parameterId", "objectId"},
        ReferenceSpec{"edge-1.0", "id", "objectId"},
        ReferenceSpec{"edge-1.0", "source", "outputPortReference-1.0"},
        ReferenceSpec{"edge-1.0", "destination", "inputPortReference-1.0"},
        ReferenceSpec{"outputPortReference-1.0", "nodeId", "objectId"},
        ReferenceSpec{"outputPortReference-1.0", "port", "structuralText"},
        ReferenceSpec{"layerOutput-1.0", "nodeId", "objectId"},
        ReferenceSpec{"layerOutput-1.0", "layerId", "objectId"},
        ReferenceSpec{"layerOutput-1.0", "name", "humanFacingName"},
        ReferenceSpec{"layerOutput-1.0", "outputPort", "structuralText"},
        ReferenceSpec{"layerStack-1.0", "nodeId", "objectId"},
        ReferenceSpec{"layerStackEntry-1.0", "slotId", "objectId"},
        ReferenceSpec{"layerStackEntry-1.0", "layerId", "objectId"},
        ReferenceSpec{"idAllocation-1.0", "highestIssued", "highestIssued-1.0"},
        ReferenceSpec{"extensionRecord-1.0", "id", "objectId"},
        ReferenceSpec{"extensionRecord-1.0", "ownerId", "namespacedIdentifier"},
        ReferenceSpec{"extensionRecord-1.0", "typeId", "namespacedIdentifier"},
        ReferenceSpec{"extensionRecord-1.0", "schemaVersion", "schemaVersion-1.0"},
        ReferenceSpec{"extensionRecord-1.0", "mediaType", "structuralText"},
        ReferenceSpec{"extensionRecord-1.0", "referencePolicy", "extensionReferencePolicy-1.0"},
        ReferenceSpec{"extensionRecord-1.0", "payload", "canonicalBase64"},
        ReferenceSpec{"extensionTarget-1.0", "id", "objectId"},
        ReferenceSpec{"extensionHostReference-1.0", "key", "structuralText"},
        ReferenceSpec{"extensionHostReference-1.0", "target", "extensionTarget-1.0"},
    };
    for (const auto& spec : references) {
        const auto& definition = requireMember(definitions, spec.definition, "$.$defs");
        const auto& properties = requireMember(definition, "properties", spec.definition);
        requireReference(properties, spec.property, spec.target,
                         "$.$defs." + std::string{spec.definition} + ".properties");
    }
}

} // namespace

void validateDocumentGraphAndReferences(const json::Value& definitions,
                                        const std::vector<const json::Value*>& parameterValues,
                                        const std::vector<const json::Value*>& curves) {
    static_cast<void>(validateObjectShape(definitions, "scalarKeyframe-1.0",
                                          {"id", "time", "value", "outgoingInterpolation"},
                                          {"id", "time", "value", "outgoingInterpolation"}));
    static_cast<void>(validateObjectShape(definitions, "vec2Keyframe-1.0",
                                          {"id", "time", "value", "outgoingInterpolation"},
                                          {"id", "time", "value", "outgoingInterpolation"}));
    static_cast<void>(validateObjectShape(definitions, "vec2Value-1.0", {"x", "y"}, {"x", "y"}));

    validateGraphDefinitions(definitions);
    validateAllocatorAndExtensions(definitions);
    validateDirectReferences(definitions);

    for (const std::string_view keyframe : {"scalarKeyframe-1.0", "vec2Keyframe-1.0"}) {
        const auto& properties =
            requireMember(requireMember(definitions, keyframe, "$.$defs"), "properties", keyframe);
        requireExact(requireMember(properties, "outgoingInterpolation", keyframe),
                     R"({"enum":["hold","linear"]})",
                     "$.$defs." + std::string{keyframe} + ".properties.outgoingInterpolation");
    }
    const auto& scalarProperties =
        requireMember(requireMember(definitions, "scalarKeyframe-1.0", "$.$defs"), "properties",
                      "scalarKeyframe-1.0");
    requireExact(requireMember(scalarProperties, "value", "scalar keyframe"),
                 R"({"type":"number"})", "$.$defs.scalarKeyframe-1.0.properties.value");
    const auto& vec2Properties = requireMember(
        requireMember(definitions, "vec2Value-1.0", "$.$defs"), "properties", "vec2Value-1.0");
    requireExact(vec2Properties, R"({"x":{"type":"number"},"y":{"type":"number"}})",
                 "$.$defs.vec2Value-1.0.properties");
    for (const auto index : {3U}) {
        requireExact(requireMember(*parameterValues[index], "x", "vec2 parameter"),
                     R"({"type":"number"})", "$.$defs.parameterValue-1.0 vec2 x");
        requireExact(requireMember(*parameterValues[index], "y", "vec2 parameter"),
                     R"({"type":"number"})", "$.$defs.parameterValue-1.0 vec2 y");
    }
    for (const std::string_view channel : {"red", "green", "blue", "alpha"}) {
        requireExact(requireMember(*parameterValues[4], channel, "color4 parameter"),
                     R"({"type":"number"})",
                     "$.$defs.parameterValue-1.0 color4 " + std::string{channel});
    }
    requireExact(requireMember(*parameterValues[5], "value", "string parameter"),
                 R"({"type":"string"})", "$.$defs.parameterValue-1.0 string value");
    requireReference(*parameterValues[6], "numerator", "signed64Decimal",
                     "$.$defs.parameterValue-1.0 rational");
    requireReference(*parameterValues[6], "denominator", "positiveSigned64Decimal",
                     "$.$defs.parameterValue-1.0 rational");
    for (auto index = std::size_t{0}; index < curves.size(); ++index) {
        requireReference(*curves[index], "id", "objectId", "$.$defs.animationCurve-1.0 curve");
    }

    const auto& inputs =
        requireArray(requireMember(requireMember(definitions, "inputPortReference-1.0", "$.$defs"),
                                   "oneOf", "inputPortReference-1.0"),
                     "inputPortReference-1.0.oneOf");
    const auto& nodeInput = requireMember(inputs[0], "properties", "node input");
    requireReference(nodeInput, "nodeId", "objectId", "$.$defs.inputPortReference-1.0.oneOf[0]");
    requireReference(nodeInput, "port", "structuralText",
                     "$.$defs.inputPortReference-1.0.oneOf[0]");
    const auto& layerInput = requireMember(inputs[1], "properties", "layer input");
    requireReference(layerInput, "stackNodeId", "objectId",
                     "$.$defs.inputPortReference-1.0.oneOf[1]");
    requireReference(layerInput, "slotId", "objectId", "$.$defs.inputPortReference-1.0.oneOf[1]");
    requireReference(layerInput, "role", "structuralText",
                     "$.$defs.inputPortReference-1.0.oneOf[1]");

    const auto& policies = requireArray(
        requireMember(requireMember(definitions, "extensionReferencePolicy-1.0", "$.$defs"),
                      "oneOf", "extensionReferencePolicy-1.0"),
        "extensionReferencePolicy-1.0.oneOf");
    const auto& hostProperties = requireMember(policies[1], "properties", "host policy");
    validateArray(requireMember(hostProperties, "references", "host policy"),
                  "$.$defs.extensionReferencePolicy-1.0 host-table references",
                  "#/$defs/extensionHostReference-1.0");
    const auto& remapperProperties = requireMember(policies[2], "properties", "remapper policy");
    requireReference(remapperProperties, "remapperId", "namespacedIdentifier",
                     "$.$defs.extensionReferencePolicy-1.0 owner-remapper");
    requireReference(remapperProperties, "version", "schemaVersion-1.0",
                     "$.$defs.extensionReferencePolicy-1.0 owner-remapper");
}

} // namespace bloom::quality::schema_detail
