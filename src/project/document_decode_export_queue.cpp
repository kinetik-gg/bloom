// Document 1.29 export queue decode (ADR 0033). Closed object shapes in writer order; record
// validation beyond the wire shape (profiles per format, bounds, composition existence) is
// document::Project::validate()'s during reconstruction.
#include "document_decode_internal.hpp"

#include <bloom/document/export_queue.hpp>

#include <limits>

namespace bloom::project::detail {
namespace {

using document::ExportJob;
using document::ExportOutput;

bool fail(DecodeState& state, const std::string& path,
          const DocumentDecodeError error = DocumentDecodeError::DomainViolation) {
    state.fail(error, path);
    return false;
}

bool text(const JsonValue& node, DecodeState& state, const std::string& path, std::string& out) {
    std::string_view value;
    if (!decodeStringMember(node, state, path, value))
        return false;
    if (value.size() > document::kExportTextMaximumBytes)
        return fail(state, path);
    out = value;
    return true;
}

bool boolean(const JsonValue& node, DecodeState& state, const std::string& path, bool& out) {
    const auto parsed = node.asBoolean();
    if (node.kind() != JsonValueKind::Boolean || !parsed)
        return fail(state, path, DocumentDecodeError::WrongValueKind);
    out = *parsed;
    return true;
}

bool decimal(const JsonValue& node, DecodeState& state, const std::string& path,
             std::uint64_t& out) {
    std::string_view value;
    if (!decodeStringMember(node, state, path, value))
        return false;
    const auto parsed = parseCanonicalAllocatorHighWater(value);
    if (!parsed)
        return fail(state, path);
    out = *parsed.value();
    return true;
}

template <typename Parse, typename Enum>
bool token(const JsonValue& node, DecodeState& state, const std::string& path, Parse&& parse,
           Enum& out) {
    std::string_view value;
    if (!decodeStringMember(node, state, path, value))
        return false;
    const auto parsed = parse(value);
    if (!parsed)
        return fail(state, path);
    out = *parsed;
    return true;
}

// Matches exactly `keys` (no extra members): the 1.29 shapes are closed.
bool closed(const JsonValue& node, std::span<const std::string_view> keys, DecodeState& state,
            const std::string& path, std::vector<const JsonValue*>& fields) {
    if (!matchOrderedMembers(node, keys, false, state, path, fields))
        return false;
    if (node.objectMembers().size() != keys.size())
        return fail(state, path, DocumentDecodeError::UnknownMember);
    return true;
}

bool decodeSettings(const JsonValue& node, DecodeState& state, const std::string& path,
                    document::ExportOutputSettings& out) {
    std::vector<std::string_view> keys{"format", "codecProfile"};
    const bool hasBitrate = node.findMember("videoBitrateKbps") != nullptr;
    if (hasBitrate)
        keys.push_back("videoBitrateKbps");
    for (const auto* key :
         {"compression", "outputColorSpace", "display", "view", "bakeLook", "includeAudio"})
        keys.push_back(key);
    // Optional in 1.29: absent means true.
    const bool hasCachedFrames = node.findMember("useCachedFrames") != nullptr;
    if (hasCachedFrames)
        keys.push_back("useCachedFrames");
    for (const auto* key : {"audioSampleRate", "encoder", "naming"})
        keys.push_back(key);
    std::vector<const JsonValue*> fields;
    if (!closed(node, keys, state, path, fields))
        return false;
    std::size_t at = 0;
    if (!token(*fields[at], state, joinPath(path, "format"), document::parseExportFormat,
               out.format))
        return false;
    // The WebM and AV1 tokens exist from document 1.33; an older document cannot name them.
    if (document::isDocument133ExportFormat(out.format) &&
        state.documentMinor < document::kExportWebmFormatsMinor)
        return fail(state, joinPath(path, "format"));
    ++at;
    if (!text(*fields[at++], state, joinPath(path, "codecProfile"), out.codecProfile))
        return false;
    if (hasBitrate) {
        std::uint32_t bitrate = 0;
        if (!decodeUInt32Member(*fields[at++], state, joinPath(path, "videoBitrateKbps"),
                                std::numeric_limits<std::uint32_t>::max(), bitrate))
            return false;
        out.videoBitrateKbps = bitrate;
    }
    if (!text(*fields[at++], state, joinPath(path, "compression"), out.compression) ||
        !text(*fields[at++], state, joinPath(path, "outputColorSpace"), out.outputColorSpaceId) ||
        !text(*fields[at++], state, joinPath(path, "display"), out.displayName) ||
        !text(*fields[at++], state, joinPath(path, "view"), out.viewName) ||
        !boolean(*fields[at++], state, joinPath(path, "bakeLook"), out.bakeLook) ||
        !boolean(*fields[at++], state, joinPath(path, "includeAudio"), out.includeAudio))
        return false;
    out.useCachedFrames = true;
    if (hasCachedFrames &&
        !boolean(*fields[at++], state, joinPath(path, "useCachedFrames"), out.useCachedFrames))
        return false;
    if (!decodeUInt32Member(*fields[at++], state, joinPath(path, "audioSampleRate"),
                            std::numeric_limits<std::uint32_t>::max(), out.audioSampleRate) ||
        !token(*fields[at++], state, joinPath(path, "encoder"), document::parseExportEncoder,
               out.encoder))
        return false;
    const auto& naming = *fields[at];
    const auto namingPath = joinPath(path, "naming");
    std::vector<std::string_view> namingKeys;
    const bool hasStart = naming.findMember("startFrame") != nullptr;
    if (hasStart)
        namingKeys.push_back("startFrame");
    namingKeys.push_back("framePadding");
    namingKeys.push_back("pattern");
    std::vector<const JsonValue*> namingFields;
    if (!closed(naming, namingKeys, state, namingPath, namingFields))
        return false;
    std::size_t namingAt = 0;
    if (hasStart) {
        std::uint64_t start = 0;
        if (!decimal(*namingFields[namingAt++], state, joinPath(namingPath, "startFrame"), start))
            return false;
        out.naming.startFrame = start;
    }
    return decodeUInt32Member(*namingFields[namingAt++], state,
                              joinPath(namingPath, "framePadding"), 64, out.naming.framePadding) &&
           text(*namingFields[namingAt], state, joinPath(namingPath, "pattern"),
                out.naming.pattern);
}

bool decodeOutput(const JsonValue& node, DecodeState& state, const std::string& path,
                  ExportOutput& out) {
    static constexpr std::array<std::string_view, 6> kKeys{
        "id", "preset", "settings", "destination", "enabled", "importIntoAssets"};
    std::vector<const JsonValue*> fields;
    if (!closed(node, kKeys, state, path, fields) ||
        !decodeObjectId(*fields[0], state, joinPath(path, "id"), out.id) ||
        !text(*fields[1], state, joinPath(path, "preset"), out.presetId) ||
        !decodeSettings(*fields[2], state, joinPath(path, "settings"), out.settings))
        return false;
    static constexpr std::array<std::string_view, 3> kDestinationKeys{"portability", "path",
                                                                      "overwrite"};
    const auto destinationPath = joinPath(path, "destination");
    std::vector<const JsonValue*> destination;
    return closed(*fields[3], kDestinationKeys, state, destinationPath, destination) &&
           text(*destination[0], state, joinPath(destinationPath, "portability"),
                out.destination.portability) &&
           text(*destination[1], state, joinPath(destinationPath, "path"), out.destination.path) &&
           token(*destination[2], state, joinPath(destinationPath, "overwrite"),
                 document::parseExportOverwrite, out.destination.overwrite) &&
           boolean(*fields[4], state, joinPath(path, "enabled"), out.enabled) &&
           boolean(*fields[5], state, joinPath(path, "importIntoAssets"), out.importIntoAssets);
}

bool decodeJob(const JsonValue& node, DecodeState& state, const std::string& path, ExportJob& out) {
    std::vector<std::string_view> keys{"id", "composition", "name", "range", "outputs", "enabled"};
    const bool hasLabel = node.findMember("labelColor") != nullptr;
    if (hasLabel)
        keys.push_back("labelColor");
    std::vector<const JsonValue*> fields;
    if (!closed(node, keys, state, path, fields) ||
        !decodeObjectId(*fields[0], state, joinPath(path, "id"), out.id) ||
        !decodeObjectId(*fields[1], state, joinPath(path, "composition"), out.composition) ||
        !text(*fields[2], state, joinPath(path, "name"), out.name))
        return false;
    const auto rangePath = joinPath(path, "range");
    const auto* kind = fields[3]->findMember("kind");
    const bool custom = kind && kind->asString() == std::optional<std::string_view>("custom");
    std::vector<std::string_view> rangeKeys{"kind", "first"};
    if (custom)
        rangeKeys.push_back("last");
    std::vector<const JsonValue*> range;
    if (!closed(*fields[3], rangeKeys, state, rangePath, range) ||
        !token(*range[0], state, joinPath(rangePath, "kind"), document::parseExportRangeKind,
               out.render.range.kind) ||
        !decimal(*range[1], state, joinPath(rangePath, "first"), out.render.range.first) ||
        (custom && !decimal(*range[2], state, joinPath(rangePath, "last"), out.render.range.last)))
        return false;
    const auto outputsPath = joinPath(path, "outputs");
    if (fields[4]->kind() != JsonValueKind::Array ||
        fields[4]->arrayElements().size() > document::kExportJobMaximumOutputs)
        return fail(state, outputsPath);
    const auto outputs = fields[4]->arrayElements();
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        ExportOutput output;
        if (!decodeOutput(outputs[index], state, joinPathIndex(outputsPath, index), output))
            return false;
        out.outputs.push_back(std::move(output));
    }
    if (!boolean(*fields[5], state, joinPath(path, "enabled"), out.enabled))
        return false;
    if (hasLabel) {
        const auto labelPath = joinPath(path, "labelColor");
        const auto channels = fields[6]->arrayElements();
        if (fields[6]->kind() != JsonValueKind::Array || channels.size() != 3)
            return fail(state, labelPath, DocumentDecodeError::WrongValueKind);
        std::array<std::uint8_t, 3> rgb{};
        for (std::size_t index = 0; index < rgb.size(); ++index) {
            std::uint32_t channel = 0;
            if (!decodeUInt32Member(channels[index], state, joinPathIndex(labelPath, index), 255,
                                    channel))
                return false;
            rgb[index] = static_cast<std::uint8_t>(channel);
        }
        out.labelColor = rgb;
    }
    return true;
}

} // namespace

bool decodeExportQueue(const JsonValue& node, DecodeState& state, const std::string& path,
                       document::ExportQueue& out) {
    static constexpr std::array<std::string_view, 1> kKeys{"jobs"};
    std::vector<const JsonValue*> fields;
    if (!closed(node, kKeys, state, path, fields))
        return false;
    const auto jobsPath = joinPath(path, "jobs");
    if (fields[0]->kind() != JsonValueKind::Array ||
        fields[0]->arrayElements().size() > document::kExportQueueMaximumJobs)
        return fail(state, jobsPath);
    const auto jobs = fields[0]->arrayElements();
    out.jobs.clear();
    out.jobs.reserve(jobs.size());
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        ExportJob job;
        if (!decodeJob(jobs[index], state, joinPathIndex(jobsPath, index), job))
            return false;
        out.jobs.push_back(std::move(job));
    }
    return true;
}

} // namespace bloom::project::detail
