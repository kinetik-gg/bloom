#include "document_decode_internal.hpp"
#include <array>
#include <bloom/document/asset.hpp>
#include <limits>

namespace bloom::project::detail {
namespace {
bool text(const JsonValue& node, DecodeState& state, const std::string& path, std::string& out) {
    std::string_view value;
    if (!decodeStringMember(node, state, path, value))
        return false;
    out = value;
    return true;
}
bool number(const JsonValue& node, DecodeState& state, const std::string& path,
            std::uint32_t& out) {
    const auto token = node.asNumberToken();
    if (!token) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = parseCanonicalAllocatorHighWater(*token);
    if (!parsed || *parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = static_cast<std::uint32_t>(*parsed.value());
    return true;
}
bool integer(const JsonValue& node, DecodeState& state, const std::string& path,
             std::int64_t& out) {
    const auto token = node.asString();
    if (!token) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = parseCanonicalInt64(*token);
    if (!parsed) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = *parsed.value();
    return true;
}
bool digest(const JsonValue& node, DecodeState& state, const std::string& path,
            core::Sha256Digest& out) {
    const auto token = node.asString();
    const auto value = token ? core::Sha256Digest::fromLowercaseHex(*token) : std::nullopt;
    if (!value) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = *value;
    return true;
}
bool locator(const JsonValue& node, DecodeState& state, const std::string& path,
             document::AssetLocator& out) {
    constexpr std::array<std::string_view, 4> keys{"kind", "portability", "path", "relinkHint"};
    std::vector<const JsonValue*> members;
    return matchOrderedMembers(node, keys, false, state, path, members) &&
           text(*members[0], state, path, out.kind) &&
           text(*members[1], state, path, out.portability) &&
           text(*members[2], state, path, out.path) &&
           text(*members[3], state, path, out.relinkHint);
}
bool manifest(const JsonValue& node, DecodeState& state, const std::string& path,
              document::AssetSequenceManifest& out) {
    constexpr std::array<std::string_view, 6> keys{"pattern", "padding", "first",
                                                   "last",    "members", "gaps"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, keys, false, state, path, members) ||
        !text(*members[0], state, path, out.pattern) ||
        !number(*members[1], state, path, out.padding) ||
        !integer(*members[2], state, path, out.first) ||
        !integer(*members[3], state, path, out.last))
        return false;
    if (members[4]->kind() != JsonValueKind::Array || members[5]->kind() != JsonValueKind::Array ||
        members[4]->arrayElements().size() > 100000 ||
        members[5]->arrayElements().size() > 100000) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    constexpr std::array<std::string_view, 3> memberKeys{"frame", "locator", "contentDigest"};
    for (const auto& value : members[4]->arrayElements()) {
        std::vector<const JsonValue*> fields;
        document::AssetSequenceMember member;
        if (!matchOrderedMembers(value, memberKeys, false, state, path, fields) ||
            !integer(*fields[0], state, path, member.frame) ||
            !locator(*fields[1], state, path, member.locator) ||
            !digest(*fields[2], state, path, member.contentDigest))
            return false;
        out.members.push_back(std::move(member));
    }
    for (const auto& value : members[5]->arrayElements()) {
        std::int64_t frame = 0;
        if (!integer(value, state, path, frame))
            return false;
        out.gaps.push_back(frame);
    }
    return true;
}
bool audio(const JsonValue& node, DecodeState& state, const std::string& path,
           document::AssetRecord& out) {
    constexpr std::array<std::string_view, 4> keys{"rate", "channels", "frames", "duration"};
    std::vector<const JsonValue*> fields;
    if (!matchOrderedMembers(node, keys, false, state, path, fields) ||
        !number(*fields[0], state, path, out.rate) ||
        !number(*fields[1], state, path, out.channels))
        return false;
    const auto frameText = fields[2]->asString();
    if (!frameText) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsedFrames = parseCanonicalObjectId(*frameText);
    if (!parsedFrames || *parsedFrames.value() == 0) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out.frames = *parsedFrames.value();
    return decodeRationalTimeValue(*fields[3], state, path, out.duration);
}
} // namespace
bool decodeAssets(const JsonValue& node, DecodeState& state, const std::string& path,
                  std::vector<document::AssetRecord>& out) {
    if (node.kind() != JsonValueKind::Array || node.arrayElements().size() > 100000) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    constexpr std::array<std::string_view, 8> keys{
        "id", "kind", "locator", "contentDigest", "interpretation", "width", "height", "manifest"};
    std::uint64_t previous = 0;
    for (const auto& value : node.arrayElements()) {
        std::vector<const JsonValue*> fields;
        document::AssetRecord asset;
        std::string kind;
        if (!matchOrderedMembers(value, keys, false, state, path, fields) ||
            !decodeObjectId(*fields[0], state, path, asset.id) ||
            !text(*fields[1], state, path, kind) ||
            !locator(*fields[2], state, path, asset.locator) ||
            !digest(*fields[3], state, path, asset.contentDigest))
            return false;
        if (asset.id.value() <= previous ||
            (kind != "image" && kind != "sequence" && kind != "audio")) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        previous = asset.id.value();
        asset.kind = kind == "image" ? document::AssetKind::Image
                                       : kind == "sequence" ? document::AssetKind::Sequence
                                                             : document::AssetKind::Audio;
        constexpr std::array<std::string_view, 2> interpretationKeys{"colorSpace",
                                                                     "alphaAssociation"};
        std::vector<const JsonValue*> interpretation;
        std::uint32_t space = 0, alpha = 0;
        if (!matchOrderedMembers(*fields[4], interpretationKeys, false, state, path,
                                 interpretation) ||
            !number(*interpretation[0], state, path, space) ||
            !number(*interpretation[1], state, path, alpha) || space > 3 || alpha > 1 ||
            !number(*fields[5], state, path, asset.width) ||
            !number(*fields[6], state, path, asset.height) ||
            !manifest(*fields[7], state, path, asset.manifest)) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        asset.interpretation = {static_cast<document::AssetColorSpace>(space),
                                static_cast<document::AssetAlphaAssociation>(alpha)};
        if (asset.kind == document::AssetKind::Audio) {
            const auto* audioNode = value.findMember("audio");
            if (audioNode == nullptr || !audio(*audioNode, state, path, asset)) {
                state.fail(DocumentDecodeError::DomainViolation, path);
                return false;
            }
        }
        if (!asset.validate().ok()) {
            state.fail(DocumentDecodeError::DomainViolation, path);
            return false;
        }
        out.push_back(std::move(asset));
    }
    return true;
}
} // namespace bloom::project::detail
