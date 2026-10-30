#include <bloom/project/document_migration.hpp>

#include <bloom/document/asset.hpp>
#include <bloom/document/node_layout.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_json_writer.hpp>

#include <array>
#include <span>
#include <string_view>

namespace bloom::project {
namespace {
using Buffer = std::pmr::vector<char>;
void append(Buffer& output, const std::string_view text) {
    output.insert(output.end(), text.begin(), text.end());
}

bool quoted(Buffer& output, const std::string_view text) {
    auto counter = CanonicalJsonWriter::counting();
    if (!counter.stringValue(text) || !counter.finish())
        return false;
    const auto offset = output.size();
    output.resize(offset + counter.bytesRequired());
    CanonicalJsonWriter writer(std::span(output).subspan(offset), {});
    return writer.stringValue(text) && writer.finish();
}

// Copy strict DOM values without normalizing number tokens: migration must not turn an invalid
// structural integer spelling into an accepted one before typed decoding has examined it.
bool copyValue(const JsonValue& value, Buffer& output) {
    switch (value.kind()) {
    case JsonValueKind::Null:
        append(output, "null");
        return true;
    case JsonValueKind::Boolean:
        append(output, value.asBoolean().value_or(false) ? "true" : "false");
        return true;
    case JsonValueKind::Number:
        append(output, value.asNumberToken().value_or(""));
        return true;
    case JsonValueKind::String:
        return quoted(output, value.asString().value_or(""));
    case JsonValueKind::Array: {
        append(output, "[");
        bool first = true;
        for (const auto& element : value.arrayElements()) {
            if (!first)
                append(output, ",");
            first = false;
            if (!copyValue(element, output))
                return false;
        }
        append(output, "]");
        return true;
    }
    case JsonValueKind::Object: {
        append(output, "{");
        bool first = true;
        for (const auto& member : value.objectMembers()) {
            if (!first)
                append(output, ",");
            first = false;
            if (!quoted(output, member.key()))
                return false;
            append(output, ":");
            if (!copyValue(member.value(), output))
                return false;
        }
        append(output, "}");
        return true;
    }
    }
    return false;
}

bool layout(const JsonValue& composition, Buffer& output) {
    const auto* graph = composition.findMember("graph");
    const auto* nodes = graph ? graph->findMember("nodes") : nullptr;
    if (!nodes || nodes->kind() != JsonValueKind::Array)
        return false;
    std::array<std::size_t, 4> rows{};
    append(output, "[");
    bool first = true;
    for (const auto& node : nodes->arrayElements()) {
        const auto* id = node.findMember("id");
        const auto* type = node.findMember("typeId");
        const auto idText = id ? id->asString() : std::nullopt;
        const auto typeText = type ? type->asString() : std::nullopt;
        if (!idText || !typeText)
            return false;
        const auto parsedId = parseCanonicalObjectId(*idText);
        if (!parsedId || *parsedId.value() == 0)
            return false;
        const auto column = document::defaultNodeLayoutColumn(*typeText);
        const auto record = document::defaultNodeLayoutRecord(column, rows.at(column)++);
        if (!first)
            append(output, ",");
        first = false;
        append(output, "{\"nodeId\":");
        if (!quoted(output, *idText))
            return false;
        append(output, ",\"position\":{\"x\":");
        const auto x = formatCanonicalFloat64(record.position.x);
        const auto y = formatCanonicalFloat64(record.position.y);
        if (!x || !y)
            return false;
        append(output, x.value()->view());
        append(output, ",\"y\":");
        append(output, y.value()->view());
        append(output, "},\"width\":128,\"collapsed\":false,\"muted\":false}");
    }
    append(output, "]");
    return true;
}

// Both steps are the same walk: copy every member verbatim, rewrite the root schema version, and
// append exactly one new member to each object the step adds one to. A new member is appended LAST
// in its object, which is canonical precisely because a strictly-decodable source document of the
// step's own version carries no trailing unknown members to be pushed behind -- an unknown member
// at a supported minor is a decode error, not retained data, so there is nothing to order against.
// AnimationBreadth is the version-only step (1.2 -> 1.3): it appends no member anywhere, because
// what 1.3 adds is a new animation-curve KIND and a new interpolation TOKEN, neither of which an
// older file can contain. It still walks the whole document through the same copy/rewrite path so
// every numeric spelling is preserved byte for byte and the version rewrite is the only difference.
// ValueGraph (1.3 -> 1.4) is version-only for the same reason: what 1.4 adds is a new
// constant-value kind ("vec3") and a new parameter-source kind ("driver"), and a 1.3 file can
// contain neither -- the step exists so the chain has no hole, not because a 1.3 document is
// missing anything.
enum class Step {
    NodeLayout,
    NodeGroups,
    AnimationBreadth,
    ValueGraph,
    LayerTimeline,
    Merges,
    ContentBounds,
    SafeAreas,
    Images,
    Audio,
    KeyframeHandles,
    LayerParenting,
    Paths,
    TextCapability,
    AssetOrganization,
    Video,
    DataBlocks,
    WorkingColorSpace,
    AssetInputColorSpace,
    LutAssets,
    ExternalFileLocators,
    LayerMasks,
    CompositionFolders,
    VideoColourDefault,
    ExportQueue,
    EasingAndLayerFlags
};
enum class Scope { Root, Project, Composition, IdAllocation, HighestIssued };

[[nodiscard]] bool alreadyMigrated(const JsonValue& value, const Scope scope, const Step step) {
    if (step == Step::AssetOrganization)
        return scope == Scope::Project && value.findMember("assetFolders");
    if (step == Step::DataBlocks)
        return (scope == Scope::Project && value.findMember("dataBlocks")) ||
               (scope == Scope::HighestIssued && value.findMember("dataBlock"));
    // Layer masks (1.23 -> 1.24, ADR 0028) is version-only: every member it adds is optional.
    if (step == Step::LayerMasks)
        return false;
    // Composition folders (1.26 -> 1.27, #378) is version-only too: the composition `folder`
    // member is optional and absent means the Assets root.
    if (step == Step::CompositionFolders)
        return false;
    // The SDR video colour default (1.27 -> 1.28, ADR 0032) is version-only: no member changes.
    // The source version tells the open path that the project was saved under the old default.
    if (step == Step::VideoColourDefault)
        return false;
    // Easing equations and layer flags (1.29 -> 1.30) are version-only: the new interpolation
    // tokens are new values and every new member (keyframe `easing`, layer `shy` and
    // `nameFollowsSource`) is optional. The open path infers nameFollowsSource for a decoded
    // document older than 1.30 during reconstruction, where the source names are known.
    if (step == Step::EasingAndLayerFlags)
        return false;
    // The export queue (1.28 -> 1.29, ADR 0033) adds required members; their presence proves the
    // step already ran.
    if (step == Step::ExportQueue)
        return (scope == Scope::Project && value.findMember("exportQueue")) ||
               (scope == Scope::HighestIssued &&
                (value.findMember("exportJob") || value.findMember("exportOutput")));
    if (step == Step::Images) {
        return (scope == Scope::Project && value.findMember("assets")) ||
               (scope == Scope::Composition && value.findMember("backgroundColor")) ||
               (scope == Scope::HighestIssued && value.findMember("asset"));
    }
    // A version-only step adds nothing, so there is no member whose presence could prove it already
    // ran; its own source-version refusal (sourceVersionIs() below) is the whole guard.
    if (step == Step::AnimationBreadth || step == Step::ValueGraph || step == Step::LayerTimeline ||
        step == Step::Merges || step == Step::ContentBounds || step == Step::Audio ||
        step == Step::KeyframeHandles || step == Step::LayerParenting || step == Step::Paths ||
        step == Step::TextCapability || step == Step::Video)
        return false;
    if (step == Step::WorkingColorSpace)
        return false;
    if (step == Step::AssetInputColorSpace || step == Step::LutAssets ||
        step == Step::ExternalFileLocators)
        return false;
    if (scope == Scope::Composition) {
        return value.findMember(step == Step::NodeLayout   ? "nodeLayout"
                                : step == Step::NodeGroups ? "nodeGroups"
                                                           : "safeAreas") != nullptr;
    }
    return scope == Scope::HighestIssued && value.findMember("nodeGroup") != nullptr;
}

[[nodiscard]] std::optional<std::string_view> legacySrgbTextureSpace(const JsonValue& project) {
    const auto* colorSettings = project.findMember("colorSettings");
    const auto* ocioConfig = colorSettings ? colorSettings->findMember("ocioConfig") : nullptr;
    const auto* locator = ocioConfig ? ocioConfig->findMember("locator") : nullptr;
    const auto* uri = locator ? locator->findMember("uri") : nullptr;
    const auto uriText = uri ? uri->asString() : std::nullopt;
    if (!uriText)
        return std::nullopt;
    if (*uriText == "bloom://ocio/neutral-v1/config.ocio")
        return "srgb_rec709_display";
    if (*uriText == "ocio://cg-config-v1.0.0_aces-v1.3_ocio-v2.1")
        return "sRGB - Texture";
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> legacyWorkingSpace(const JsonValue& project) {
    const auto* colorSettings = project.findMember("colorSettings");
    const auto* value = colorSettings ? colorSettings->findMember("processColorSpaceId") : nullptr;
    return value ? value->asString() : std::nullopt;
}

[[nodiscard]] bool emitMigratedInterpretation(const JsonValue& interpretation,
                                              const JsonValue& project, Buffer& output) {
    const auto* colorSpace = interpretation.findMember("colorSpace");
    const auto* alphaAssociation = interpretation.findMember("alphaAssociation");
    const auto colorSpaceToken = colorSpace ? colorSpace->asNumberToken() : std::nullopt;
    const auto alphaAssociationToken =
        alphaAssociation ? alphaAssociation->asNumberToken() : std::nullopt;
    if (interpretation.kind() != JsonValueKind::Object || colorSpace == nullptr ||
        alphaAssociation == nullptr || interpretation.objectMembers().size() != 2 ||
        !colorSpaceToken || !alphaAssociationToken)
        return false;
    const auto parsed = parseCanonicalAllocatorHighWater(*colorSpaceToken);
    if (!parsed || *parsed.value() > 3)
        return false;
    std::string_view inputId;
    switch (*parsed.value()) {
    case 0: // Auto remains Auto and resolves from the media descriptor.
    case 3: // Raw remains the explicit data/no-conversion legacy mode.
        break;
    case 1: {
        const auto id = legacySrgbTextureSpace(project);
        if (!id)
            return false;
        inputId = *id;
        break;
    }
    case 2: {
        const auto id = legacyWorkingSpace(project);
        if (!id || id->empty())
            return false;
        inputId = *id;
        break;
    }
    default:
        return false;
    }
    append(output, "{\"colorSpace\":");
    if (!copyValue(*colorSpace, output))
        return false;
    append(output, ",\"inputColorSpaceId\":");
    if (!quoted(output, inputId))
        return false;
    append(output, ",\"alphaAssociation\":");
    if (!copyValue(*alphaAssociation, output))
        return false;
    append(output, "}");
    return true;
}

[[nodiscard]] bool emitMigratedAssets(const JsonValue& assets, const JsonValue& project,
                                      Buffer& output) {
    if (assets.kind() != JsonValueKind::Array)
        return false;
    append(output, "[");
    bool first = true;
    for (const auto& asset : assets.arrayElements()) {
        if (asset.kind() != JsonValueKind::Object || asset.findMember("interpretation") == nullptr)
            return false;
        if (!first)
            append(output, ",");
        first = false;
        append(output, "{");
        bool firstMember = true;
        for (const auto& member : asset.objectMembers()) {
            if (!firstMember)
                append(output, ",");
            firstMember = false;
            if (!quoted(output, member.key()))
                return false;
            append(output, ":");
            if (member.key() == "interpretation") {
                if (!emitMigratedInterpretation(member.value(), project, output))
                    return false;
            } else if (!copyValue(member.value(), output)) {
                return false;
            }
        }
        append(output, "}");
    }
    append(output, "]");
    return true;
}

bool transform(const JsonValue& value, const Scope scope, const Step step, Buffer& output) {
    if (value.kind() != JsonValueKind::Object || alreadyMigrated(value, scope, step))
        return false;
    append(output, "{");
    bool first = true;
    for (const auto& member : value.objectMembers()) {
        if (scope == Scope::Composition && step == Step::SafeAreas && member.key() == "workArea") {
            if (!first)
                append(output, ",");
            first = false;
            append(output, "\"safeAreas\":{\"action\":0.9,\"title\":0.8}");
        }
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        const auto descend = [&](const Scope child) {
            return transform(member.value(), child, step, output);
        };
        if (scope == Scope::Root && member.key() == "schemaVersion") {
            append(output, step == Step::NodeLayout             ? "{\"major\":1,\"minor\":1}"
                           : step == Step::NodeGroups           ? "{\"major\":1,\"minor\":2}"
                           : step == Step::AnimationBreadth     ? "{\"major\":1,\"minor\":3}"
                           : step == Step::ValueGraph           ? "{\"major\":1,\"minor\":4}"
                           : step == Step::LayerTimeline        ? "{\"major\":1,\"minor\":5}"
                           : step == Step::Merges               ? "{\"major\":1,\"minor\":6}"
                           : step == Step::ContentBounds        ? "{\"major\":1,\"minor\":7}"
                           : step == Step::SafeAreas            ? "{\"major\":1,\"minor\":8}"
                           : step == Step::Images               ? "{\"major\":1,\"minor\":10}"
                           : step == Step::Audio                ? "{\"major\":1,\"minor\":11}"
                           : step == Step::KeyframeHandles      ? "{\"major\":1,\"minor\":12}"
                           : step == Step::LayerParenting       ? "{\"major\":1,\"minor\":13}"
                           : step == Step::Paths                ? "{\"major\":1,\"minor\":14}"
                           : step == Step::TextCapability       ? "{\"major\":1,\"minor\":15}"
                           : step == Step::AssetOrganization    ? "{\"major\":1,\"minor\":16}"
                           : step == Step::Video                ? "{\"major\":1,\"minor\":17}"
                           : step == Step::DataBlocks           ? "{\"major\":1,\"minor\":18}"
                           : step == Step::WorkingColorSpace    ? "{\"major\":1,\"minor\":19}"
                           : step == Step::AssetInputColorSpace ? "{\"major\":1,\"minor\":20}"
                           : step == Step::LutAssets            ? "{\"major\":1,\"minor\":21}"
                           : step == Step::LayerMasks           ? "{\"major\":1,\"minor\":24}"
                           : step == Step::CompositionFolders   ? "{\"major\":1,\"minor\":27}"
                           : step == Step::VideoColourDefault   ? "{\"major\":1,\"minor\":28}"
                           : step == Step::ExportQueue          ? "{\"major\":1,\"minor\":29}"
                           : step == Step::EasingAndLayerFlags  ? "{\"major\":1,\"minor\":30}"
                                                                : "{\"major\":1,\"minor\":22}");
        } else if (scope == Scope::Root && member.key() == "project") {
            if (!descend(Scope::Project))
                return false;
        } else if (scope == Scope::Root && member.key() == "idAllocation" &&
                   (step == Step::NodeGroups || step == Step::Images || step == Step::DataBlocks ||
                    step == Step::ExportQueue)) { // NOLINT(bugprone-branch-clone)
            if (!descend(Scope::IdAllocation))
                return false;
        } else if (scope == Scope::IdAllocation && member.key() == "highestIssued") {
            if (!descend(Scope::HighestIssued))
                return false;
        } else if (scope == Scope::Project && member.key() == "assets" &&
                   step == Step::AssetOrganization) {
            if (member.value().kind() != JsonValueKind::Array)
                return false;
            append(output, "[");
            std::uint64_t index = 0;
            for (const auto& asset : member.value().arrayElements()) {
                if (asset.kind() != JsonValueKind::Object || asset.findMember("name") ||
                    asset.findMember("folder") || asset.findMember("tags") ||
                    asset.findMember("order"))
                    return false;
                if (index != 0)
                    append(output, ",");
                if (!copyValue(asset, output))
                    return false;
                output.pop_back();
                const auto* locator = asset.findMember("locator");
                const auto* path = locator ? locator->findMember("path") : nullptr;
                if (!path || !path->asString())
                    return false;
                document::AssetRecord record;
                record.locator.path = path->asString().value_or("");
                if (const auto* portability = locator->findMember("portability"))
                    record.locator.portability = portability->asString().value_or("");
                if (const auto* font = asset.findMember("font")) {
                    record.kind = document::AssetKind::Font;
                    if (const auto* family = font->findMember("family"))
                        record.fontFamily = family->asString().value_or("");
                    if (const auto* style = font->findMember("style"))
                        record.fontStyle = style->asString().value_or("");
                }
                append(output, ",\"name\":");
                if (!quoted(output, document::defaultAssetName(record)))
                    return false;
                append(output, ",\"tags\":[],\"order\":");
                const auto orderText = formatCanonicalUInt64(index++);
                if (!quoted(output, orderText.view()))
                    return false;
                append(output, "}");
            }
            append(output, "]");
        } else if (scope == Scope::Project && member.key() == "assets" &&
                   step == Step::AssetInputColorSpace) {
            if (!emitMigratedAssets(member.value(), value, output))
                return false;
        } else if (scope == Scope::Project && member.key() == "compositions") {
            if (member.value().kind() != JsonValueKind::Array)
                return false;
            append(output, "[");
            bool firstComposition = true;
            for (const auto& composition : member.value().arrayElements()) {
                if (!firstComposition)
                    append(output, ",");
                firstComposition = false;
                if (!transform(composition, Scope::Composition, step, output))
                    return false;
            }
            append(output, "]");
        } else if (!copyValue(member.value(), output))
            return false;
    }
    if (scope == Scope::Composition) {
        if (step == Step::NodeLayout) {
            append(output, ",\"nodeLayout\":");
            if (!layout(value, output))
                return false;
        } else if (step == Step::NodeGroups) {
            // A 1.1 file has no groups: the feature did not exist, so there is nothing to infer.
            append(output, ",\"nodeGroups\":[]");
        } else if (step == Step::SafeAreas && value.findMember("workArea") == nullptr) {
            append(output, ",\"safeAreas\":{\"action\":0.9,\"title\":0.8}");
        }
    }
    if (scope == Scope::HighestIssued && step == Step::NodeGroups)
        append(output, ",\"nodeGroup\":\"0\"");
    if (scope == Scope::HighestIssued && step == Step::DataBlocks)
        append(output, ",\"dataBlock\":\"0\"");
    if (step == Step::Images) {
        if (scope == Scope::Project)
            append(output, ",\"assets\":[]");
        if (scope == Scope::Composition)
            append(output, ",\"backgroundColor\":[0,0,0,1]");
        if (scope == Scope::HighestIssued)
            append(output, ",\"asset\":\"0\"");
    }
    if (scope == Scope::Project && step == Step::DataBlocks)
        append(output, ",\"dataBlocks\":[]");
    if (scope == Scope::Project && step == Step::ExportQueue)
        append(output, ",\"exportQueue\":{\"jobs\":[]}");
    if (scope == Scope::HighestIssued && step == Step::ExportQueue)
        append(output, ",\"exportJob\":\"0\",\"exportOutput\":\"0\"");
    append(output, "}");
    return true;
}

[[nodiscard]] bool sourceVersionIs(const JsonValue& root, const std::string_view minorToken) {
    const auto* version = root.findMember("schemaVersion");
    const auto* major = version ? version->findMember("major") : nullptr;
    const auto* minor = version ? version->findMember("minor") : nullptr;
    return major && minor && major->asNumberToken() == "1" &&
           minor->asNumberToken() == minorToken && version->objectMembers().size() == 2;
}
// A node-schema step rewrites only the node-record schemaVersion of one node type at the listed
// source versions; every binding and every other byte is copied unchanged, so no record is inserted
// and no value is rewritten.
//
// Layer time (1.22 -> 1.23): the boundary gains optional members that an older file cannot contain,
// and the Layer Output node schema advances (3 or 4 -> 5) to declare the optional Time Remap role.
//
// Channel sets (1.24 -> 1.26, ADR 0026): Image source v1 becomes v2. Its new `part` and `layer`
// parameters are optional (absent means the first part and the root layer) and its new `channels`
// output starts unconnected, so existing projects evaluate byte-identically. The step is chained
// from 1.24 because the video-io 1.25 step is not merged yet; once it is, this step's source
// version becomes 1.25.
struct NodeSchemaRewrite final {
    std::string_view typeId;
    std::array<std::string_view, 2> sourceVersions;
    std::string_view targetVersion;
    std::string_view documentVersion;
};

inline constexpr NodeSchemaRewrite kLayerTimeRewrite{
    "bloom.layer-output", {"3", "4"}, "5", "{\"major\":1,\"minor\":23}"};
inline constexpr NodeSchemaRewrite kChannelSetRewrite{
    "bloom.image-source", {"1", "1"}, "2", "{\"major\":1,\"minor\":26}"};

[[nodiscard]] bool emitRewrittenNode(const JsonValue& node, const NodeSchemaRewrite& rewrite,
                                     Buffer& output) {
    if (node.kind() != JsonValueKind::Object)
        return false;
    const auto* typeId = node.findMember("typeId");
    const auto* schemaVersion = node.findMember("schemaVersion");
    const auto version = schemaVersion ? schemaVersion->asNumberToken() : std::nullopt;
    const auto matches =
        typeId && typeId->asString() == rewrite.typeId && version &&
        (*version == rewrite.sourceVersions[0] || *version == rewrite.sourceVersions[1]);
    append(output, "{");
    bool first = true;
    for (const auto& member : node.objectMembers()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (matches && member.key() == "schemaVersion")
            append(output, rewrite.targetVersion);
        else if (!copyValue(member.value(), output))
            return false;
    }
    append(output, "}");
    return true;
}

[[nodiscard]] bool emitRewrittenObject(const JsonValue& object, const std::string_view rewriteKey,
                                       const auto& rewrite, Buffer& output) {
    if (object.kind() != JsonValueKind::Object)
        return false;
    append(output, "{");
    bool first = true;
    for (const auto& member : object.objectMembers()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (member.key() == rewriteKey ? !rewrite(member.value(), output)
                                       : !copyValue(member.value(), output))
            return false;
    }
    append(output, "}");
    return true;
}

[[nodiscard]] bool emitNodeSchemaDocument(const JsonValue& root, const NodeSchemaRewrite& rewrite,
                                          Buffer& output) {
    const auto nodes = [&](const JsonValue& array, Buffer& out) {
        if (array.kind() != JsonValueKind::Array)
            return false;
        append(out, "[");
        bool firstNode = true;
        for (const auto& node : array.arrayElements()) {
            if (!firstNode)
                append(out, ",");
            firstNode = false;
            if (!emitRewrittenNode(node, rewrite, out))
                return false;
        }
        append(out, "]");
        return true;
    };
    const auto graph = [&](const JsonValue& value, Buffer& out) {
        return emitRewrittenObject(value, "nodes", nodes, out);
    };
    const auto composition = [&](const JsonValue& value, Buffer& out) {
        return emitRewrittenObject(value, "graph", graph, out);
    };
    const auto compositions = [&](const JsonValue& value, Buffer& out) {
        if (value.kind() != JsonValueKind::Array)
            return false;
        append(out, "[");
        bool firstComposition = true;
        for (const auto& element : value.arrayElements()) {
            if (!firstComposition)
                append(out, ",");
            firstComposition = false;
            if (!composition(element, out))
                return false;
        }
        append(out, "]");
        return true;
    };
    const auto project = [&](const JsonValue& value, Buffer& out) {
        return emitRewrittenObject(value, "compositions", compositions, out);
    };
    if (root.kind() != JsonValueKind::Object)
        return false;
    append(output, "{");
    bool first = true;
    for (const auto& member : root.objectMembers()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (member.key() == "schemaVersion")
            append(output, rewrite.documentVersion);
        else if (member.key() == "project" ? !project(member.value(), output)
                                           : !copyValue(member.value(), output))
            return false;
    }
    append(output, "}");
    return true;
}
// Video source metadata (-> 1.25): every video-asset stream gains "fieldOrder" (0, unknown;
// older files never recorded it) and "timecodeType" derived from its timecode text, both after
// "range". Every other byte is copied unchanged.
[[nodiscard]] bool emitVideoMetadataStream(const JsonValue& stream, Buffer& output) {
    if (stream.kind() != JsonValueKind::Object)
        return false;
    const auto* timecode = stream.findMember("timecode");
    const auto text = timecode ? timecode->asString() : std::nullopt;
    if (!text || stream.findMember("range") == nullptr ||
        stream.findMember("fieldOrder") != nullptr)
        return false;
    append(output, "{");
    bool first = true;
    for (const auto& member : stream.objectMembers()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (!copyValue(member.value(), output))
            return false;
        if (member.key() == "range") {
            append(output, ",\"fieldOrder\":0,\"timecodeType\":");
            append(output, std::array<std::string_view, 3>{"0", "1",
                                                           "2"}[document::timecodeTypeFor(*text)]);
        }
    }
    append(output, "}");
    return true;
}

[[nodiscard]] bool emitVideoMetadataObject(const JsonValue& object, const std::string_view key,
                                           const auto& rewrite, Buffer& output) {
    if (object.kind() != JsonValueKind::Object)
        return false;
    append(output, "{");
    bool first = true;
    for (const auto& member : object.objectMembers()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (member.key() == key ? !rewrite(member.value(), output)
                                : !copyValue(member.value(), output))
            return false;
    }
    append(output, "}");
    return true;
}

[[nodiscard]] bool emitVideoMetadataArray(const JsonValue& array, const auto& element,
                                          Buffer& output) {
    if (array.kind() != JsonValueKind::Array)
        return false;
    append(output, "[");
    bool first = true;
    for (const auto& value : array.arrayElements()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!element(value, output))
            return false;
    }
    append(output, "]");
    return true;
}

[[nodiscard]] bool emitVideoMetadataDocument(const JsonValue& root, Buffer& output) {
    const auto streams = [](const JsonValue& value, Buffer& out) {
        return emitVideoMetadataArray(value, emitVideoMetadataStream, out);
    };
    const auto video = [&](const JsonValue& value, Buffer& out) {
        return emitVideoMetadataObject(value, "streams", streams, out);
    };
    const auto asset = [&](const JsonValue& value, Buffer& out) {
        return emitVideoMetadataObject(value, "video", video, out);
    };
    const auto assets = [&](const JsonValue& value, Buffer& out) {
        return emitVideoMetadataArray(value, asset, out);
    };
    const auto project = [&](const JsonValue& value, Buffer& out) {
        return emitVideoMetadataObject(value, "assets", assets, out);
    };
    if (root.kind() != JsonValueKind::Object)
        return false;
    append(output, "{");
    bool first = true;
    for (const auto& member : root.objectMembers()) {
        if (!first)
            append(output, ",");
        first = false;
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (member.key() == "schemaVersion")
            append(output, "{\"major\":1,\"minor\":25}");
        else if (member.key() == "project" ? !project(member.value(), output)
                                           : !copyValue(member.value(), output))
            return false;
    }
    append(output, "}");
    return true;
}
} // namespace

MigrationStepOutcome migrateNodeLayoutV1_0(const JsonValue& root,
                                           std::pmr::memory_resource* /*resource*/,
                                           Buffer& output) {
    if (!sourceVersionIs(root, "0"))
        return MigrationStepOutcome::failure("/schemaVersion");
    if (!transform(root, Scope::Root, Step::NodeLayout, output))
        return MigrationStepOutcome::failure("/project/compositions");
    return MigrationStepOutcome::success();
}

MigrationStepOutcome migrateNodeGroupsV1_1(const JsonValue& root,
                                           std::pmr::memory_resource* /*resource*/,
                                           Buffer& output) {
    if (!sourceVersionIs(root, "1"))
        return MigrationStepOutcome::failure("/schemaVersion");
    if (!transform(root, Scope::Root, Step::NodeGroups, output))
        return MigrationStepOutcome::failure("/project/compositions");
    return MigrationStepOutcome::success();
}

MigrationStepOutcome migrateAnimationBreadthV1_2(const JsonValue& root,
                                                 std::pmr::memory_resource* /*resource*/,
                                                 Buffer& output) {
    if (!sourceVersionIs(root, "2"))
        return MigrationStepOutcome::failure("/schemaVersion");
    if (!transform(root, Scope::Root, Step::AnimationBreadth, output))
        return MigrationStepOutcome::failure("/project/compositions");
    return MigrationStepOutcome::success();
}

MigrationStepOutcome migrateValueGraphV1_3(const JsonValue& root,
                                           std::pmr::memory_resource* /*resource*/,
                                           Buffer& output) {
    if (!sourceVersionIs(root, "3"))
        return MigrationStepOutcome::failure("/schemaVersion");
    if (!transform(root, Scope::Root, Step::ValueGraph, output))
        return MigrationStepOutcome::failure("/project/compositions");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateLayerTimelineV1_4(const JsonValue& root, std::pmr::memory_resource*,
                                              Buffer& output) {
    if (!sourceVersionIs(root, "4") || !transform(root, Scope::Root, Step::LayerTimeline, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateMergesV1_5(const JsonValue& root, std::pmr::memory_resource*,
                                       Buffer& output) {
    if (!sourceVersionIs(root, "5") || !transform(root, Scope::Root, Step::Merges, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}

MigrationStepOutcome migrateViewerSafeAreasV1_7(const JsonValue& root, std::pmr::memory_resource*,
                                                Buffer& output) {
    if (!sourceVersionIs(root, "7") || !transform(root, Scope::Root, Step::SafeAreas, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateImagesV1_9(const JsonValue& root, std::pmr::memory_resource*,
                                       Buffer& output) {
    if (!sourceVersionIs(root, "9") || !transform(root, Scope::Root, Step::Images, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateAudioV1_10(const JsonValue& root, std::pmr::memory_resource*,
                                       Buffer& output) {
    if (!sourceVersionIs(root, "10") || !transform(root, Scope::Root, Step::Audio, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
// A version-only step: 1.12 adds the two ease-handle members, both omitted whenever they hold
// their default, and a 1.11 document has no non-default handle to write. The step exists so the
// ladder has no hole, exactly as migrateAudioV1_10 does.
MigrationStepOutcome migrateKeyframeHandlesV1_11(const JsonValue& root, std::pmr::memory_resource*,
                                                 Buffer& output) {
    if (!sourceVersionIs(root, "11") ||
        !transform(root, Scope::Root, Step::KeyframeHandles, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateLayerParentingV1_12(const JsonValue& root, std::pmr::memory_resource*,
                                                Buffer& output) {
    if (!sourceVersionIs(root, "12") || !transform(root, Scope::Root, Step::LayerParenting, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateContentBoundsV1_6(const JsonValue& root, std::pmr::memory_resource*,
                                              Buffer& output) {
    if (!sourceVersionIs(root, "6") || !transform(root, Scope::Root, Step::ContentBounds, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migratePathsV1_13(const JsonValue& root, std::pmr::memory_resource*,
                                       Buffer& output) {
    if (!sourceVersionIs(root, "13") || !transform(root, Scope::Root, Step::Paths, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateTextCapabilityV1_14(const JsonValue& root, std::pmr::memory_resource*,
                                                Buffer& output) {
    if (!sourceVersionIs(root, "14") || !transform(root, Scope::Root, Step::TextCapability, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateAssetOrganizationV1_15(const JsonValue& root,
                                                   std::pmr::memory_resource*, Buffer& output) {
    if (!sourceVersionIs(root, "15") ||
        !transform(root, Scope::Root, Step::AssetOrganization, output))
        return MigrationStepOutcome::failure("/project/assets");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateVideoV1_16(const JsonValue& root, std::pmr::memory_resource*,
                                       Buffer& output) {
    if (!sourceVersionIs(root, "16") || !transform(root, Scope::Root, Step::Video, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}

MigrationStepOutcome migrateDataBlocksV1_17(const JsonValue& root, std::pmr::memory_resource*,
                                            Buffer& output) {
    if (!sourceVersionIs(root, "17"))
        return MigrationStepOutcome::failure("/schemaVersion");
    if (!transform(root, Scope::Root, Step::DataBlocks, output))
        return MigrationStepOutcome::failure("/project/dataBlocks");
    return MigrationStepOutcome::success();
}

MigrationStepOutcome migrateWorkingColorSpaceV1_18(const JsonValue& root,
                                                   std::pmr::memory_resource*, Buffer& output) {
    // Existing projects already inherit their project working space. The new optional composition
    // member therefore needs no inferred JSON value; only the schema version advances.
    if (!sourceVersionIs(root, "18") ||
        !transform(root, Scope::Root, Step::WorkingColorSpace, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}

MigrationStepOutcome migrateAssetInputColorSpaceV1_19(const JsonValue& root,
                                                      std::pmr::memory_resource*, Buffer& output) {
    if (!sourceVersionIs(root, "19") ||
        !transform(root, Scope::Root, Step::AssetInputColorSpace, output))
        return MigrationStepOutcome::failure("/project/assets");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateLutAssetsV1_20(const JsonValue& root, std::pmr::memory_resource*,
                                           Buffer& output) {
    if (!sourceVersionIs(root, "20") || !transform(root, Scope::Root, Step::LutAssets, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateExternalFileLocatorsV1_21(const JsonValue& root,
                                                      std::pmr::memory_resource*, Buffer& output) {
    // Older locators remain project-relative; the new arm only expands what a writer can emit.
    if (!sourceVersionIs(root, "21") ||
        !transform(root, Scope::Root, Step::ExternalFileLocators, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateLayerTimeV1_22(const JsonValue& root, std::pmr::memory_resource*,
                                           Buffer& output) {
    if (!sourceVersionIs(root, "22") || !emitNodeSchemaDocument(root, kLayerTimeRewrite, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateLayerMasksV1_23(const JsonValue& root, std::pmr::memory_resource*,
                                            Buffer& output) {
    if (!sourceVersionIs(root, "23") || !transform(root, Scope::Root, Step::LayerMasks, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateCompositionFoldersV1_26(const JsonValue& root,
                                                    std::pmr::memory_resource*, Buffer& output) {
    if (!sourceVersionIs(root, "26") ||
        !transform(root, Scope::Root, Step::CompositionFolders, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateVideoColourDefaultV1_27(const JsonValue& root,
                                                    std::pmr::memory_resource*, Buffer& output) {
    if (!sourceVersionIs(root, "27") ||
        !transform(root, Scope::Root, Step::VideoColourDefault, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateEasingAndLayerFlagsV1_29(const JsonValue& root,
                                                     std::pmr::memory_resource*, Buffer& output) {
    if (!sourceVersionIs(root, "29") ||
        !transform(root, Scope::Root, Step::EasingAndLayerFlags, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateExportQueueV1_28(const JsonValue& root, std::pmr::memory_resource*,
                                             Buffer& output) {
    if (!sourceVersionIs(root, "28") || !transform(root, Scope::Root, Step::ExportQueue, output))
        return MigrationStepOutcome::failure("/project/exportQueue");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateChannelSetsV1_25(const JsonValue& root, std::pmr::memory_resource*,
                                             Buffer& output) {
    if (!sourceVersionIs(root, "25") || !emitNodeSchemaDocument(root, kChannelSetRewrite, output))
        return MigrationStepOutcome::failure("/schemaVersion");
    return MigrationStepOutcome::success();
}
MigrationStepOutcome migrateVideoSourceMetadataV1_24(const JsonValue& root,
                                                     std::pmr::memory_resource*, Buffer& output) {
    if (!sourceVersionIs(root, "24") || !emitVideoMetadataDocument(root, output))
        return MigrationStepOutcome::failure("/project/assets");
    return MigrationStepOutcome::success();
}

} // namespace bloom::project
