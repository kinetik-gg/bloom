#include <bloom/project/manifest_decode.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_manifest.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::project {

namespace {

// ------------------------------------------------------------------------------------------
// Decode plumbing. Deliberately mirrors document_decode_internal.hpp's shape (matchOrderedMembers/
// decodeStringMember/decodeUInt32Member/joinPath) rather than including that header: it is a
// private seam typed to DocumentDecodeError/RoundTripState shared only between document_decode.cpp
// and document_decode_composition.cpp (see its own file comment), and this module has no RT1-style
// capture state to thread through it. Unlike that header's matchOrderedMembers, the overload here
// never captures a trailing member: manifest round-trip capture is out of scope (see
// manifest_decode.hpp's file comment), so a rejected trailing member is always either a hard error
// (containerVersion {1,0}) or moot (containerVersion {1, minor > 0} short-circuits to
// PreservationRequired before any object's trailing members are ever inspected).
// ------------------------------------------------------------------------------------------

struct DecodeState final {
    ManifestDecodeError error = ManifestDecodeError::None;
    std::string path;

    void fail(const ManifestDecodeError newError, std::string newPath) noexcept {
        if (error == ManifestDecodeError::None) {
            error = newError;
            path = std::move(newPath);
        }
    }
};

[[nodiscard]] std::string joinPath(const std::string& base, const std::string_view segment) {
    std::string result;
    result.reserve(base.size() + 1 + segment.size());
    result.append(base);
    result.push_back('/');
    result.append(segment);
    return result;
}

[[nodiscard]] std::string joinPathIndex(const std::string& base, const std::size_t index) {
    return joinPath(base, std::to_string(index));
}

// Checks that `object` is a JSON object whose leading members exactly match `expectedKeys` in
// order, filling `outValues` with pointers to each matched member's value. When
// `rejectExtraMembers` is false, trailing members beyond the matched prefix are accepted without
// inspection -- used only to peek at containerVersion's major/minor before this module knows
// whether trailing members (here or anywhere else in the manifest) should be tolerated at all (see
// decodeManifestEnvelope's own two-pass bootstrap, mirroring decodeDocumentEnvelope's).
[[nodiscard]] bool matchOrderedMembers(const JsonValue& object,
                                       const std::span<const std::string_view> expectedKeys,
                                       const bool rejectExtraMembers, DecodeState& state,
                                       const std::string& basePath,
                                       std::vector<const JsonValue*>& outValues) {
    if (object.kind() != JsonValueKind::Object) {
        state.fail(ManifestDecodeError::WrongValueKind, basePath);
        return false;
    }

    const auto members = object.objectMembers();
    outValues.clear();
    outValues.reserve(expectedKeys.size());
    for (std::size_t index = 0; index < expectedKeys.size(); ++index) {
        if (members.size() <= index) {
            state.fail(ManifestDecodeError::MissingMember, joinPath(basePath, expectedKeys[index]));
            return false;
        }
        if (members[index].key() != expectedKeys[index]) {
            const bool knownElsewhere = std::find(expectedKeys.begin(), expectedKeys.end(),
                                                  members[index].key()) != expectedKeys.end();
            state.fail(knownElsewhere ? ManifestDecodeError::MemberOutOfOrder
                                      : ManifestDecodeError::UnknownMember,
                       joinPath(basePath, members[index].key()));
            return false;
        }
        outValues.push_back(&members[index].value());
    }

    if (rejectExtraMembers && members.size() > expectedKeys.size()) {
        state.fail(ManifestDecodeError::UnknownMember,
                   joinPath(basePath, members[expectedKeys.size()].key()));
        return false;
    }
    return true;
}

// Not noexcept: DecodeState::fail() takes its path argument by value, so a failing call here copies
// `path` into that by-value parameter -- an allocation that can throw std::bad_alloc. Marking this
// noexcept while it allocates on the failure path would be an untruthful noexcept claim.
[[nodiscard]] bool decodeStringMember(const JsonValue& value, DecodeState& state,
                                      const std::string& path, std::string_view& out) {
    if (value.kind() != JsonValueKind::String) {
        state.fail(ManifestDecodeError::WrongValueKind, path);
        return false;
    }
    const auto text = value.asString();
    if (!text.has_value()) {
        state.fail(ManifestDecodeError::WrongValueKind, path);
        return false;
    }
    out = *text;
    return true;
}

// asNumberToken() is documented to return a value whenever kind() already matches, but that
// invariant is not visible to static analysis; re-check has_value() immediately before
// dereferencing rather than trusting the prior kind() check alone (mirrors document_decode.cpp's
// own decodeUInt32Member).
[[nodiscard]] bool decodeUInt32Member(const JsonValue& value, DecodeState& state,
                                      const std::string& path, std::uint32_t& out) {
    if (value.kind() != JsonValueKind::Number) {
        state.fail(ManifestDecodeError::WrongValueKind, path);
        return false;
    }
    const auto token = value.asNumberToken();
    if (!token.has_value()) {
        state.fail(ManifestDecodeError::WrongValueKind, path);
        return false;
    }
    // Every version-object component is an unbounded uint32 (see "Decimal Strings And JSON
    // Integers": "must fit their declared uint32 or smaller range" -- major/minor declare no
    // smaller range). Both CanonicalDecimalError variants parseCanonicalJsonUInt32 can report here
    // (non-canonical spelling, or a value that fits uint64 but exceeds uint32) collapse to the same
    // InvalidJsonUInt32: unlike document_decode.cpp's DomainViolation catch-all (which also covers
    // unrelated semantic domains such as OCIO/pixel-format rules), this module has no broader
    // "value parsed fine but violates a domain rule" concept for a bare version component to share
    // with, so a second error value would only exist to distinguish two flavors of "not a valid
    // uint32" that the adversarial test corpus does not ask decode() callers to tell apart.
    const auto parsed = parseCanonicalJsonUInt32(*token, std::numeric_limits<std::uint32_t>::max());
    if (!parsed) {
        state.fail(ManifestDecodeError::InvalidJsonUInt32, path);
        return false;
    }
    out = *parsed.value();
    return true;
}

[[nodiscard]] bool decodeVersionField(const JsonValue& node, DecodeState& state,
                                      const std::string& path, const bool rejectExtraMembers,
                                      document::SchemaVersion& out) {
    static constexpr std::array<std::string_view, 2> kKeys{"major", "minor"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, rejectExtraMembers, state, path, members)) {
        return false;
    }
    std::uint32_t major = 0;
    if (!decodeUInt32Member(*members[0], state, joinPath(path, "major"), major)) {
        return false;
    }
    std::uint32_t minor = 0;
    if (!decodeUInt32Member(*members[1], state, joinPath(path, "minor"), minor)) {
        return false;
    }
    out.major = major;
    out.minor = minor;
    return true;
}

// ------------------------------------------------------------------------------------------
// `document` member (docs/architecture/project-format.md, "Manifest Shape"): exactly `path` then
// `schemaVersion`, closed.
// ------------------------------------------------------------------------------------------

[[nodiscard]] bool decodeDocumentSection(const JsonValue& node, DecodeState& state,
                                         const std::string& path, DecodedManifest& out) {
    static constexpr std::array<std::string_view, 2> kKeys{"path", "schemaVersion"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    const auto pathMemberPath = joinPath(path, "path");
    std::string_view pathText;
    if (!decodeStringMember(*members[0], state, pathMemberPath, pathText)) {
        return false;
    }
    if (pathText != kCanonicalManifestDocumentPath) {
        state.fail(ManifestDecodeError::InvalidDocumentPath, pathMemberPath);
        return false;
    }
    out.documentPath = std::string(pathText);

    const auto schemaVersionPath = joinPath(path, "schemaVersion");
    document::SchemaVersion schemaVersion;
    if (!decodeVersionField(*members[1], state, schemaVersionPath, true, schemaVersion)) {
        return false;
    }
    // Document `schemaVersion`'s major gates the same way containerVersion's does (decode refuses
    // an unsupported major; migration is out of scope). Its minor does NOT: a newer document minor
    // embedded in an otherwise-{1,0} manifest is exposed as a plain decoded value rather than
    // refused here -- manifest/document agreement and the document side's own newer-minor handling
    // belong to a later chain-layer slice (see manifest_decode.hpp's file comment).
    if (schemaVersion.major != kCanonicalManifestDocumentSchemaVersionV1.major) {
        state.fail(ManifestDecodeError::UnsupportedMajorVersion,
                   joinPath(schemaVersionPath, "major"));
        return false;
    }
    out.documentSchemaVersion = schemaVersion;
    return true;
}

// ------------------------------------------------------------------------------------------
// `requirements` array (docs/architecture/project-format.md, "Manifest Shape"): each record is
// exactly providerId/capabilityId/schemaVersion/providedNodeTypeIds, closed; sorted by providerId,
// then capabilityId, then schema major/minor; duplicate provider/capability pairs invalid.
// ------------------------------------------------------------------------------------------

// Mirrors canonical_manifest.cpp's and manifest_requirements.cpp's own file-local requirementLess:
// this exact three-key comparator (UTF-8 providerId, then UTF-8 capabilityId, then schema
// major/minor) is already duplicated identically between those two translation units (the writer's
// pre-encode validator and the semantic coverage validator), so mirroring it a third time here for
// the read side matches the codebase's existing precedent rather than introducing a new shared
// header only decode would use.
[[nodiscard]] bool requirementLess(const ManifestRequirement& left,
                                   const ManifestRequirement& right) noexcept {
    const auto providerOrder = core::compareUtf8Bytes(left.providerId, right.providerId);
    if (providerOrder != std::strong_ordering::equal) {
        return providerOrder == std::strong_ordering::less;
    }
    const auto capabilityOrder = core::compareUtf8Bytes(left.capabilityId, right.capabilityId);
    if (capabilityOrder != std::strong_ordering::equal) {
        return capabilityOrder == std::strong_ordering::less;
    }
    return left.schemaVersion < right.schemaVersion;
}

[[nodiscard]] bool decodeProvidedNodeTypeIds(const JsonValue& node, DecodeState& state,
                                             const std::string& path,
                                             std::vector<std::string>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(ManifestDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());

    std::string_view previous;
    bool hasPrevious = false;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const auto elementPath = joinPathIndex(path, index);
        std::string_view text;
        if (!decodeStringMember(elements[index], state, elementPath, text)) {
            return false;
        }
        if (!document::isValidNamespacedIdentifier(text)) {
            state.fail(ManifestDecodeError::InvalidProvidedNodeTypeId, elementPath);
            return false;
        }
        if (hasPrevious) {
            const auto order = core::compareUtf8Bytes(previous, text);
            if (order == std::strong_ordering::equal) {
                state.fail(ManifestDecodeError::DuplicateProvidedNodeTypeId, elementPath);
                return false;
            }
            if (order != std::strong_ordering::less) {
                state.fail(ManifestDecodeError::InvalidProvidedNodeTypeOrder, elementPath);
                return false;
            }
        }
        // `text` is a view into the DOM node's own decoded string storage (JsonValue::asString()),
        // which the caller contract (see manifest_decode.hpp) guarantees outlives this whole call;
        // safe to keep comparing against across iterations without re-reading `out`.
        previous = text;
        hasPrevious = true;
        out.emplace_back(text);
    }
    return true;
}

[[nodiscard]] bool decodeRequirement(const JsonValue& node, DecodeState& state,
                                     const std::string& path, ManifestRequirement& out) {
    static constexpr std::array<std::string_view, 4> kKeys{"providerId", "capabilityId",
                                                           "schemaVersion", "providedNodeTypeIds"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    const auto providerIdPath = joinPath(path, "providerId");
    std::string_view providerIdText;
    if (!decodeStringMember(*members[0], state, providerIdPath, providerIdText)) {
        return false;
    }
    if (!document::isValidNamespacedIdentifier(providerIdText)) {
        state.fail(ManifestDecodeError::InvalidProviderId, providerIdPath);
        return false;
    }
    out.providerId = std::string(providerIdText);

    const auto capabilityIdPath = joinPath(path, "capabilityId");
    std::string_view capabilityIdText;
    if (!decodeStringMember(*members[1], state, capabilityIdPath, capabilityIdText)) {
        return false;
    }
    if (!document::isValidNamespacedIdentifier(capabilityIdText)) {
        state.fail(ManifestDecodeError::InvalidCapabilityId, capabilityIdPath);
        return false;
    }
    out.capabilityId = std::string(capabilityIdText);

    if (!decodeVersionField(*members[2], state, joinPath(path, "schemaVersion"), true,
                            out.schemaVersion)) {
        return false;
    }

    return decodeProvidedNodeTypeIds(*members[3], state, joinPath(path, "providedNodeTypeIds"),
                                     out.providedNodeTypeIds);
}

[[nodiscard]] bool decodeRequirements(const JsonValue& node, DecodeState& state,
                                      const std::string& path,
                                      std::vector<ManifestRequirement>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(ManifestDecodeError::WrongValueKind, path);
        return false;
    }
    const auto elements = node.arrayElements();
    out.clear();
    out.reserve(elements.size());

    for (std::size_t index = 0; index < elements.size(); ++index) {
        const auto elementPath = joinPathIndex(path, index);
        ManifestRequirement requirement;
        if (!decodeRequirement(elements[index], state, elementPath, requirement)) {
            return false;
        }
        if (!out.empty()) {
            const auto& previous = out.back();
            if (previous.providerId == requirement.providerId &&
                previous.capabilityId == requirement.capabilityId) {
                state.fail(ManifestDecodeError::DuplicateRequirementIdentity, elementPath);
                return false;
            }
            if (!requirementLess(previous, requirement)) {
                state.fail(ManifestDecodeError::InvalidRequirementOrder, elementPath);
                return false;
            }
        }
        // `out` is reserved to elements.size() up front and grows by exactly one push per
        // iteration, so `out.back()` above never dangles across the next iteration's reserve.
        out.push_back(std::move(requirement));
    }
    return true;
}

} // namespace

ManifestDecodePathText ManifestDecodePathText::from(const std::string_view text) noexcept {
    ManifestDecodePathText result;
    const auto copySize = std::min(text.size(), result.chars_.size());
    for (std::size_t index = 0; index < copySize; ++index) {
        result.chars_[index] = text[index];
    }
    result.size_ = static_cast<std::uint16_t>(copySize);
    result.truncated_ = text.size() > copySize;
    return result;
}

ManifestDecodeResult ManifestDecodeResult::success(DecodedManifest manifest) {
    ManifestDecodeResult result;
    result.manifest_ = std::move(manifest);
    result.outcome_ = ManifestDecodeOutcome::Decoded;
    return result;
}

ManifestDecodeResult ManifestDecodeResult::failure(const ManifestDecodeError error,
                                                   const std::string_view path) {
    ManifestDecodeResult result;
    result.outcome_ = ManifestDecodeOutcome::Failed;
    result.error_ = error;
    result.path_ = ManifestDecodePathText::from(path);
    return result;
}

ManifestDecodeResult ManifestDecodeResult::preservationRequired(const std::string_view path) {
    ManifestDecodeResult result;
    result.outcome_ = ManifestDecodeOutcome::PreservationRequired;
    result.path_ = ManifestDecodePathText::from(path);
    return result;
}

ManifestDecodeResult decodeManifestEnvelope(const JsonValue& root) {
    static constexpr std::array<std::string_view, 4> kRootKeys{"format", "containerVersion",
                                                               "document", "requirements"};
    DecodeState state;
    std::vector<const JsonValue*> members;
    // First pass: match only the exact-order prefix, not yet rejecting a trailing member. Whether
    // the root object's (or containerVersion's own) trailing members should ever be tolerated
    // depends on containerVersion.minor, which this same pass's own result (members[1]) is what
    // determines -- an unavoidable bootstrap order, mirroring decodeDocumentEnvelope's identical
    // two-pass root read in document_decode.cpp.
    if (!matchOrderedMembers(root, kRootKeys, false, state, std::string{}, members)) {
        return ManifestDecodeResult::failure(state.error, state.path);
    }

    // Identity before version: `format` is this file's identity declaration -- pass 1 above has
    // already proven members[0] occupies format's canonical position, so checking its kind and
    // exact value costs nothing extra here, before containerVersion is even peeked at. A manifest
    // that does not declare itself as this format must fail outright (WrongValueKind/InvalidFormat)
    // rather than falling into the newer-minor containerVersion short-circuit below and being
    // misclassified as a newer *Bloom* manifest -- containerVersion classification only makes sense
    // once the file has already claimed to be this format at all.
    const auto formatPath = std::string("/format");
    std::string_view formatText;
    if (!decodeStringMember(*members[0], state, formatPath, formatText)) {
        return ManifestDecodeResult::failure(state.error, state.path);
    }
    if (formatText != kCanonicalManifestFormat) {
        return ManifestDecodeResult::failure(ManifestDecodeError::InvalidFormat, formatPath);
    }

    document::SchemaVersion containerVersion;
    if (!decodeVersionField(*members[1], state, "/containerVersion", false, containerVersion)) {
        return ManifestDecodeResult::failure(state.error, state.path);
    }
    if (containerVersion.major != kCanonicalManifestContainerVersionV1.major) {
        return ManifestDecodeResult::failure(ManifestDecodeError::UnsupportedMajorVersion,
                                             "/containerVersion/major");
    }
    if (containerVersion.minor != kCanonicalManifestContainerVersionV1.minor) {
        // Same-major newer-minor containerVersion: classify and stop without inspecting anything
        // else in the manifest beyond the `format` identity check already done above (root's own
        // trailing members, containerVersion's own trailing members, `document`, or `requirements`)
        // -- see manifest_decode.hpp's file comment for why this is unconditional rather than
        // contingent on whether an unrecognized member is actually present anywhere.
        return ManifestDecodeResult::preservationRequired("/containerVersion/minor");
    }

    // Exact {1,0}: strict decode from here on. Second pass over the root re-checks its own shape
    // now that containerVersion.minor is known to be 0, so a trailing unknown root member is a hard
    // error (mirrors decodeDocumentEnvelope's second pass, minus RT1 capture -- this module never
    // captures).
    if (!matchOrderedMembers(root, kRootKeys, true, state, std::string{}, members)) {
        return ManifestDecodeResult::failure(state.error, state.path);
    }
    // containerVersion's own shape gets the same strict re-check: a trailing member there is only
    // tolerated (by short-circuiting above, before this point is ever reached) when minor > 0.
    document::SchemaVersion strictContainerVersion;
    if (!decodeVersionField(*members[1], state, "/containerVersion", true,
                            strictContainerVersion)) {
        return ManifestDecodeResult::failure(state.error, state.path);
    }

    DecodedManifest manifest;
    manifest.containerVersion = strictContainerVersion;

    if (!decodeDocumentSection(*members[2], state, "/document", manifest)) {
        return ManifestDecodeResult::failure(state.error, state.path);
    }

    if (!decodeRequirements(*members[3], state, "/requirements", manifest.requirements)) {
        return ManifestDecodeResult::failure(state.error, state.path);
    }

    return ManifestDecodeResult::success(std::move(manifest));
}

} // namespace bloom::project
