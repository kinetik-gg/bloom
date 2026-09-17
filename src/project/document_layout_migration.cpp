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
    AssetOrganization
};
enum class Scope { Root, Project, Composition, IdAllocation, HighestIssued };

[[nodiscard]] bool alreadyMigrated(const JsonValue& value, const Scope scope, const Step step) {
    if (step == Step::AssetOrganization)
        return scope == Scope::Project && value.findMember("assetFolders");
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
        step == Step::TextCapability)
        return false;
    if (scope == Scope::Composition) {
        return value.findMember(step == Step::NodeLayout   ? "nodeLayout"
                                : step == Step::NodeGroups ? "nodeGroups"
                                                           : "safeAreas") != nullptr;
    }
    return scope == Scope::HighestIssued && value.findMember("nodeGroup") != nullptr;
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
            append(output, step == Step::NodeLayout         ? "{\"major\":1,\"minor\":1}"
                           : step == Step::NodeGroups       ? "{\"major\":1,\"minor\":2}"
                           : step == Step::AnimationBreadth ? "{\"major\":1,\"minor\":3}"
                           : step == Step::ValueGraph       ? "{\"major\":1,\"minor\":4}"
                           : step == Step::LayerTimeline    ? "{\"major\":1,\"minor\":5}"
                           : step == Step::Merges           ? "{\"major\":1,\"minor\":6}"
                           : step == Step::ContentBounds    ? "{\"major\":1,\"minor\":7}"
                           : step == Step::SafeAreas        ? "{\"major\":1,\"minor\":8}"
                           : step == Step::Images           ? "{\"major\":1,\"minor\":10}"
                           : step == Step::Audio            ? "{\"major\":1,\"minor\":11}"
                           : step == Step::KeyframeHandles  ? "{\"major\":1,\"minor\":12}"
                           : step == Step::LayerParenting   ? "{\"major\":1,\"minor\":13}"
                           : step == Step::Paths            ? "{\"major\":1,\"minor\":14}"
                           : step == Step::TextCapability   ? "{\"major\":1,\"minor\":15}"
                                                            : "{\"major\":1,\"minor\":16}");
        } else if (scope == Scope::Root && member.key() == "project") {
            if (!descend(Scope::Project))
                return false;
        } else if (scope == Scope::Root && member.key() == "idAllocation" &&
                   (step == Step::NodeGroups ||
                    step == Step::Images)) { // NOLINT(bugprone-branch-clone)
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
    if (step == Step::Images) {
        if (scope == Scope::Project)
            append(output, ",\"assets\":[]");
        if (scope == Scope::Composition)
            append(output, ",\"backgroundColor\":[0,0,0,1]");
        if (scope == Scope::HighestIssued)
            append(output, ",\"asset\":\"0\"");
    }
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
} // namespace bloom::project
