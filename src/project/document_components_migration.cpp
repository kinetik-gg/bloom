#include <bloom/project/document_migration.hpp>

#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_json_writer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
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

bool copyTrailing(const JsonValue& object, Buffer& output,
                  const std::span<const std::string_view> known) {
    for (const auto& member : object.objectMembers()) {
        if (std::find(known.begin(), known.end(), member.key()) != known.end())
            continue;
        append(output, ",");
        if (!quoted(output, member.key()))
            return false;
        append(output, ":");
        if (!copyValue(member.value(), output))
            return false;
    }
    return true;
}

bool emitSplitKey(const JsonValue& oldKey, Buffer& output, const std::string_view component,
                  const JsonValue& value, const std::string_view id) {
    const auto* time = oldKey.findMember("time");
    const auto* interpolation = oldKey.findMember("outgoingInterpolation");
    if (time == nullptr || interpolation == nullptr)
        return false;
    append(output, "{\"id\":");
    if (!quoted(output, id))
        return false;
    append(output, ",\"component\":");
    if (!quoted(output, component))
        return false;
    append(output, ",\"time\":");
    if (!copyValue(*time, output))
        return false;
    append(output, ",\"value\":");
    if (!copyValue(value, output))
        return false;
    append(output, ",\"outgoingInterpolation\":");
    if (!copyValue(*interpolation, output))
        return false;
    if (!copyTrailing(
            oldKey, output,
            std::array<std::string_view, 4>{"id", "time", "value", "outgoingInterpolation"}))
        return false;
    append(output, "}");
    return true;
}

bool emitSplitCurve(const JsonValue& curve, Buffer& output, std::uint64_t& nextKeyframeId) {
    const auto* id = curve.findMember("id");
    const auto* kind = curve.findMember("kind");
    const auto* keys = curve.findMember("keyframes");
    if (id == nullptr || kind == nullptr || keys == nullptr || keys->kind() != JsonValueKind::Array)
        return false;
    const auto kindText = kind->asString();
    if (!kindText.has_value())
        return false;
    if (*kindText != "vec2" && *kindText != "color4")
        return copyValue(curve, output);

    constexpr std::array<std::string_view, 4> vec2Names{"x", "y", {}, {}};
    constexpr std::array<std::string_view, 4> colorNames{"red", "green", "blue", "alpha"};
    const auto names = *kindText == "vec2" ? vec2Names : colorNames;
    const std::size_t count = *kindText == "vec2" ? 2 : 4;

    append(output, "{\"id\":");
    if (!copyValue(*id, output))
        return false;
    append(output, ",\"kind\":");
    if (!copyValue(*kind, output))
        return false;
    append(output, ",\"keyframes\":[");

    bool first = true;
    for (const auto& oldKey : keys->arrayElements()) {
        const auto* oldId = oldKey.findMember("id");
        const auto* oldValue = oldKey.findMember("value");
        if (oldId == nullptr || oldValue == nullptr || oldValue->kind() != JsonValueKind::Object)
            return false;
        const auto oldIdText = oldId->asString();
        if (!oldIdText.has_value())
            return false;
        for (std::size_t index = 0; index < count; ++index) {
            const auto* scalar = oldValue->findMember(names[index]);
            if (scalar == nullptr)
                return false;
            if (!first)
                append(output, ",");
            first = false;
            if (index == 0) {
                if (!emitSplitKey(oldKey, output, names[index], *scalar, *oldIdText))
                    return false;
            } else {
                const auto generated = formatCanonicalUInt64(nextKeyframeId--);
                if (!emitSplitKey(oldKey, output, names[index], *scalar, generated.view()))
                    return false;
            }
        }
    }
    append(output, "]");
    if (!copyTrailing(curve, output, std::array<std::string_view, 3>{"id", "kind", "keyframes"}))
        return false;
    append(output, "}");
    return true;
}

bool copyMigrated(const JsonValue& value, Buffer& output, std::uint64_t& nextKeyframeId,
                  const bool root, const bool highestIssued) {
    if (value.kind() == JsonValueKind::Object) {
        append(output, "{");
        bool first = true;
        for (const auto& member : value.objectMembers()) {
            if (!first)
                append(output, ",");
            first = false;
            if (!quoted(output, member.key()))
                return false;
            append(output, ":");
            if (root && member.key() == "schemaVersion") {
                append(output, "{\"major\":1,\"minor\":9}");
            } else if (highestIssued && member.key() == "keyframe") {
                const auto highWater = formatCanonicalUInt64(nextKeyframeId);
                if (!quoted(output, highWater.view()))
                    return false;
            } else if (member.key() == "animationCurves" &&
                       member.value().kind() == JsonValueKind::Array) {
                append(output, "[");
                bool firstCurve = true;
                for (const auto& curve : member.value().arrayElements()) {
                    if (!firstCurve)
                        append(output, ",");
                    firstCurve = false;
                    if (!emitSplitCurve(curve, output, nextKeyframeId))
                        return false;
                }
                append(output, "]");
            } else if (member.key() == "highestIssued") {
                if (!copyMigrated(member.value(), output, nextKeyframeId, false, true))
                    return false;
            } else if (!copyMigrated(member.value(), output, nextKeyframeId, false, false)) {
                return false;
            }
        }
        append(output, "}");
        return true;
    }
    if (value.kind() == JsonValueKind::Array) {
        append(output, "[");
        bool first = true;
        for (const auto& element : value.arrayElements()) {
            if (!first)
                append(output, ",");
            first = false;
            if (!copyMigrated(element, output, nextKeyframeId, false, false))
                return false;
        }
        append(output, "]");
        return true;
    }
    return copyValue(value, output);
}

[[nodiscard]] bool sourceVersionIs(const JsonValue& root, const std::string_view minorToken) {
    const auto* version = root.findMember("schemaVersion");
    const auto* major = version == nullptr ? nullptr : version->findMember("major");
    const auto* minor = version == nullptr ? nullptr : version->findMember("minor");
    return major != nullptr && minor != nullptr && major->asNumberToken() == "1" &&
           minor->asNumberToken() == minorToken && version->objectMembers().size() == 2;
}

std::uint64_t additionalKeyframeCount(const JsonValue& value) {
    std::uint64_t result = 0;
    if (value.kind() == JsonValueKind::Object) {
        for (const auto& member : value.objectMembers()) {
            if (member.key() == "animationCurves" &&
                member.value().kind() == JsonValueKind::Array) {
                for (const auto& curve : member.value().arrayElements()) {
                    const auto* kind = curve.findMember("kind");
                    const auto* keys = curve.findMember("keyframes");
                    if (kind == nullptr || keys == nullptr || keys->kind() != JsonValueKind::Array)
                        continue;
                    const auto kindText = kind->asString();
                    if (!kindText.has_value())
                        continue;
                    const std::uint64_t multiplier = *kindText == "vec2"     ? 1
                                                     : *kindText == "color4" ? 3
                                                                             : 0;
                    result += multiplier * static_cast<std::uint64_t>(keys->arrayElements().size());
                }
            }
            result += additionalKeyframeCount(member.value());
        }
    } else if (value.kind() == JsonValueKind::Array) {
        for (const auto& element : value.arrayElements())
            result += additionalKeyframeCount(element);
    }
    return result;
}

} // namespace

MigrationStepOutcome migrateAnimationComponentsV1_8(const JsonValue& root,
                                                    std::pmr::memory_resource* /*resource*/,
                                                    Buffer& output) {
    if (!sourceVersionIs(root, "8"))
        return MigrationStepOutcome::failure("/schemaVersion");
    const auto* allocation = root.findMember("idAllocation");
    const auto* highest = allocation == nullptr ? nullptr : allocation->findMember("highestIssued");
    const auto* keyframe = highest == nullptr ? nullptr : highest->findMember("keyframe");
    if (keyframe == nullptr)
        return MigrationStepOutcome::failure("/idAllocation/highestIssued/keyframe");
    const auto keyframeText = keyframe->asString();
    if (!keyframeText.has_value())
        return MigrationStepOutcome::failure("/idAllocation/highestIssued/keyframe");
    // Allocator high-water values are allowed to be the sentinel "0" even though durable object
    // IDs themselves start at one. The old schema writes that sentinel for an as-yet-unused
    // keyframe namespace, so parse the allocator form rather than the object-ID form here.
    const auto parsed = parseCanonicalAllocatorHighWater(*keyframeText);
    if (!parsed || parsed.value() == nullptr)
        return MigrationStepOutcome::failure("/idAllocation/highestIssued/keyframe");
    const auto originalKeyframeId = *parsed.value();
    const auto additional = additionalKeyframeCount(root);
    if (additional > std::numeric_limits<std::uint64_t>::max() - originalKeyframeId)
        return MigrationStepOutcome::failure("/idAllocation/highestIssued/keyframe");
    // The root writes idAllocation before project, so reserve the complete range up front and
    // consume it backwards while emitting the split keys. Existing IDs remain untouched and the
    // high-water value is already correct when that earlier root member is copied.
    std::uint64_t nextKeyframeId = originalKeyframeId + additional;
    if (!copyMigrated(root, output, nextKeyframeId, true, false))
        return MigrationStepOutcome::failure("/project/compositions/animationCurves");
    return MigrationStepOutcome::success();
}

} // namespace bloom::project
