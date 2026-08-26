#include <bloom/project/document_decode.hpp>

#include "document_decode_internal.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_document.hpp>

#include <algorithm>
#include <array>
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
// TU-local only: mapUInt32Error is never called outside detail::decodeUInt32Member's own
// definition below, so unlike the primitives in document_decode_internal.hpp it keeps internal
// linkage rather than becoming part of the cross-file decode seam.
[[nodiscard]] DocumentDecodeError mapUInt32Error(const CanonicalDecimalError error) noexcept {
    return error == CanonicalDecimalError::InvalidLexicalForm
               ? DocumentDecodeError::InvalidJsonUInt32
               : DocumentDecodeError::DomainViolation;
}
} // namespace

// ------------------------------------------------------------------------------------------
// Shared decode plumbing (declared in document_decode_internal.hpp; also used by
// document_decode_composition.cpp).
// ------------------------------------------------------------------------------------------

namespace detail {

std::string joinPath(const std::string& base, const std::string_view segment) {
    std::string result;
    result.reserve(base.size() + 1 + segment.size());
    result.append(base);
    result.push_back('/');
    result.append(segment);
    return result;
}

std::string joinPathIndex(const std::string& base, const std::size_t index) {
    return joinPath(base, std::to_string(index));
}

// Checks that `object` is a JSON object whose leading members exactly match `expectedKeys` in
// order, filling `outValues` with pointers to each matched member's value. When
// `rejectExtraMembers` is true the object must contain exactly `expectedKeys.size()` members;
// otherwise trailing members beyond the matched prefix are accepted without inspection.
bool matchOrderedMembers(const JsonValue& object,
                         const std::span<const std::string_view> expectedKeys,
                         const bool rejectExtraMembers, DecodeState& state,
                         const std::string& basePath, std::vector<const JsonValue*>& outValues) {
    if (object.kind() != JsonValueKind::Object) {
        state.fail(DocumentDecodeError::WrongValueKind, basePath);
        return false;
    }

    const auto members = object.objectMembers();
    outValues.clear();
    outValues.reserve(expectedKeys.size());
    for (std::size_t index = 0; index < expectedKeys.size(); ++index) {
        if (members.size() <= index) {
            state.fail(DocumentDecodeError::MissingMember, joinPath(basePath, expectedKeys[index]));
            return false;
        }
        if (members[index].key() != expectedKeys[index]) {
            const bool knownElsewhere = std::find(expectedKeys.begin(), expectedKeys.end(),
                                                  members[index].key()) != expectedKeys.end();
            state.fail(knownElsewhere ? DocumentDecodeError::MemberOutOfOrder
                                      : DocumentDecodeError::UnknownMember,
                       joinPath(basePath, members[index].key()));
            return false;
        }
        outValues.push_back(&members[index].value());
    }

    if (rejectExtraMembers && members.size() > expectedKeys.size()) {
        state.fail(DocumentDecodeError::UnknownMember,
                   joinPath(basePath, members[expectedKeys.size()].key()));
        return false;
    }
    return true;
}

DocumentDecodeError mapRationalError(const CanonicalDecimalError error) noexcept {
    return error == CanonicalDecimalError::NotReduced
               ? DocumentDecodeError::UnreducedRational
               : DocumentDecodeError::InvalidRationalComponent;
}

std::string fieldPath(const std::string& base, const CanonicalDecimalField field) {
    switch (field) {
    case CanonicalDecimalField::Numerator:
        return joinPath(base, "numerator");
    case CanonicalDecimalField::Denominator:
        return joinPath(base, "denominator");
    case CanonicalDecimalField::Value:
    case CanonicalDecimalField::None:
        break;
    }
    return base;
}

bool decodeStringMember(const JsonValue& value, DecodeState& state, const std::string& path,
                        std::string_view& out) noexcept {
    if (value.kind() != JsonValueKind::String) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto text = value.asString();
    if (!text.has_value()) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    out = *text;
    return true;
}

// asNumberToken()/asString() are documented to return a value whenever kind() already matches, but
// that invariant is not visible to static analysis; every caller re-checks has_value() immediately
// before dereferencing rather than trusting the prior kind() check alone.
bool decodeUInt32Member(const JsonValue& value, DecodeState& state, const std::string& path,
                        const std::uint32_t maximum, std::uint32_t& out) {
    if (value.kind() != JsonValueKind::Number) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto token = value.asNumberToken();
    if (!token.has_value()) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto parsed = parseCanonicalJsonUInt32(*token, maximum);
    if (!parsed) {
        state.fail(mapUInt32Error(parsed.error()), path);
        return false;
    }
    out = *parsed.value();
    return true;
}

// Reads an object's first member, requires it to be literally named "kind", and decodes its string
// value as the discriminator for a branch the caller resolves afterward (constant value, parameter
// source, edge destination, OCIO locator). The branch-specific full key set (and therefore whether
// a mis-first key is "known elsewhere") is not knowable until `kindText` is read, so this cannot go
// through matchOrderedMembers itself.
bool decodeKindDiscriminator(const JsonValue& node, DecodeState& state, const std::string& path,
                             std::string_view& kindText) {
    if (node.kind() != JsonValueKind::Object) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto members = node.objectMembers();
    const auto kindPath = joinPath(path, "kind");
    if (members.empty()) {
        state.fail(DocumentDecodeError::MissingMember, kindPath);
        return false;
    }
    if (members[0].key() != "kind") {
        state.fail(DocumentDecodeError::UnknownMember, joinPath(path, members[0].key()));
        return false;
    }
    return decodeStringMember(members[0].value(), state, kindPath, kindText);
}

namespace {
[[nodiscard]] bool decodeRationalStrings(const JsonValue& node, DecodeState& state,
                                         const std::string& path, std::string_view& numeratorText,
                                         std::string_view& denominatorText) {
    static constexpr std::array<std::string_view, 2> kKeys{"numerator", "denominator"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }
    if (!decodeStringMember(*members[0], state, joinPath(path, "numerator"), numeratorText)) {
        return false;
    }
    return decodeStringMember(*members[1], state, joinPath(path, "denominator"), denominatorText);
}
} // namespace

bool decodeRationalTimeValue(const JsonValue& node, DecodeState& state, const std::string& path,
                             core::RationalTime& out) {
    std::string_view numeratorText;
    std::string_view denominatorText;
    if (!decodeRationalStrings(node, state, path, numeratorText, denominatorText)) {
        return false;
    }
    const auto parsed = parseCanonicalRationalTime(numeratorText, denominatorText);
    if (!parsed) {
        state.fail(mapRationalError(parsed.error()), fieldPath(path, parsed.field()));
        return false;
    }
    out = *parsed.value();
    return true;
}

} // namespace detail

namespace {

using detail::decodeKindDiscriminator;
using detail::decodeObjectId;
using detail::decodeRationalTimeValue;
using detail::DecodeState;
using detail::decodeStringMember;
using detail::decodeUInt32Member;
using detail::fieldPath;
using detail::joinPath;
using detail::joinPathIndex;
using detail::mapRationalError;
using detail::matchOrderedMembers;

using document::ColorSettings;
using document::CompositionFormat;
using document::CompositionId;
using document::FrameRate;
using document::OcioConfigLocator;
using document::OcioConfigPortability;
using document::OcioConfigReference;
using document::OcioConfigRevision;
using document::OcioContextVariable;
using document::OcioRevisionAlgorithm;
using document::ProjectId;
using document::SchemaVersion;

// Translates a bloom::document::ValidationIssue path (dot/bracket notation, e.g.
// "ocioConfig.contextVariables[3].name") into this module's slash-separated path convention
// (e.g. "ocioConfig/contextVariables/3/name").
[[nodiscard]] std::string translateValidationPath(const std::string_view dotted) {
    std::string result;
    result.reserve(dotted.size() + 4);
    for (std::size_t index = 0; index < dotted.size();) {
        const char character = dotted[index];
        if (character == '.') {
            result.push_back('/');
            ++index;
        } else if (character == '[') {
            result.push_back('/');
            ++index;
            while (index < dotted.size() && dotted[index] != ']') {
                result.push_back(dotted[index]);
                ++index;
            }
            if (index < dotted.size()) {
                ++index;
            }
        } else {
            result.push_back(character);
            ++index;
        }
    }
    return result;
}

[[nodiscard]] bool decodeSchemaVersionField(const JsonValue& node, DecodeState& state,
                                            const std::string& path, SchemaVersion& out) {
    static constexpr std::array<std::string_view, 2> kKeys{"major", "minor"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    std::uint32_t major = 0;
    if (!decodeUInt32Member(*members[0], state, joinPath(path, "major"),
                            std::numeric_limits<std::uint32_t>::max(), major)) {
        return false;
    }
    std::uint32_t minor = 0;
    if (!decodeUInt32Member(*members[1], state, joinPath(path, "minor"),
                            std::numeric_limits<std::uint32_t>::max(), minor)) {
        return false;
    }

    out.major = major;
    out.minor = minor;
    return true;
}

[[nodiscard]] bool decodeDuration(const JsonValue& node, DecodeState& state,
                                  const std::string& path, core::RationalTime& out) {
    if (!decodeRationalTimeValue(node, state, path, out)) {
        return false;
    }
    if (out.numerator() <= 0) {
        state.fail(DocumentDecodeError::NonPositiveDuration, path);
        return false;
    }
    return true;
}

[[nodiscard]] bool decodePixelAspect(const JsonValue& node, DecodeState& state,
                                     const std::string& path, core::PixelAspectRatio& out) {
    // decodePixelAspect/decodeFrameRate below intentionally re-decode their {numerator,denominator}
    // pair through the domain-specific parseCanonicalPixelAspectRatio/parseCanonicalPositiveRatio
    // surfaces rather than the general detail::decodeRationalTimeValue used by duration/keyframe
    // time/rational constants: pixel aspect and frame rate additionally require an unsigned
    // (never-negative) domain, which those two dedicated parsers enforce.
    static constexpr std::array<std::string_view, 2> kKeys{"numerator", "denominator"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }
    std::string_view numeratorText;
    if (!decodeStringMember(*members[0], state, joinPath(path, "numerator"), numeratorText)) {
        return false;
    }
    std::string_view denominatorText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "denominator"), denominatorText)) {
        return false;
    }
    const auto parsed = parseCanonicalPixelAspectRatio(numeratorText, denominatorText);
    if (!parsed) {
        state.fail(mapRationalError(parsed.error()), fieldPath(path, parsed.field()));
        return false;
    }
    out = *parsed.value();
    return true;
}

[[nodiscard]] bool decodeFrameRate(const JsonValue& node, DecodeState& state,
                                   const std::string& path, FrameRate& out) {
    static constexpr std::array<std::string_view, 2> kKeys{"numerator", "denominator"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }
    std::string_view numeratorText;
    if (!decodeStringMember(*members[0], state, joinPath(path, "numerator"), numeratorText)) {
        return false;
    }
    std::string_view denominatorText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "denominator"), denominatorText)) {
        return false;
    }
    const auto parsed = parseCanonicalPositiveRatio(numeratorText, denominatorText);
    if (!parsed) {
        state.fail(mapRationalError(parsed.error()), fieldPath(path, parsed.field()));
        return false;
    }
    const auto created = FrameRate::create(parsed.value()->numerator, parsed.value()->denominator);
    if (!created.has_value()) {
        // Unreachable given parseCanonicalPositiveRatio already proved a reduced, positive,
        // uint32-bounded pair; kept as a defensive typed failure rather than trusting silently.
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = *created;
    return true;
}

[[nodiscard]] bool decodeFormat(const JsonValue& node, DecodeState& state, const std::string& path,
                                CompositionFormat& out) {
    static constexpr std::array<std::string_view, 4> kKeys{"width", "height", "pixelAspect",
                                                           "frameRate"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    const auto widthPath = joinPath(path, "width");
    std::uint32_t width = 0;
    if (!decodeUInt32Member(*members[0], state, widthPath, CompositionFormat::kMaximumDimension,
                            width)) {
        return false;
    }
    if (width == 0) {
        state.fail(DocumentDecodeError::DomainViolation, widthPath);
        return false;
    }

    const auto heightPath = joinPath(path, "height");
    std::uint32_t height = 0;
    if (!decodeUInt32Member(*members[1], state, heightPath, CompositionFormat::kMaximumDimension,
                            height)) {
        return false;
    }
    if (height == 0) {
        state.fail(DocumentDecodeError::DomainViolation, heightPath);
        return false;
    }

    const auto product = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (product > CompositionFormat::kMaximumPixelCount) {
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }

    // Neither core::PixelAspectRatio nor FrameRate has a default constructor (both are built only
    // through their checked static factories), so these locals are seeded with a valid factory
    // value and then overwritten by the decode below.
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    if (!decodePixelAspect(*members[2], state, joinPath(path, "pixelAspect"), pixelAspect)) {
        return false;
    }
    FrameRate frameRate = FrameRate::framesPerSecond24();
    if (!decodeFrameRate(*members[3], state, joinPath(path, "frameRate"), frameRate)) {
        return false;
    }

    const auto created = CompositionFormat::create(width, height, pixelAspect, frameRate);
    if (!created.has_value()) {
        // Unreachable given the width/height/product pre-checks above mirror
        // CompositionFormat::isValidExtent exactly; kept as a defensive typed failure.
        state.fail(DocumentDecodeError::DomainViolation, path);
        return false;
    }
    out = *created;
    return true;
}

// A composition object is closed: exactly id/name/duration/format/parameters/animationCurves/graph
// in exact order (see docs/architecture/project-format.md, "Project And Composition"). The
// composition interior -- parameters, animationCurves, and the graph, plus the cross-reference
// checks that span them -- is decoded by detail::decodeCompositionInterior in
// document_decode_composition.cpp.
[[nodiscard]] bool decodeComposition(const JsonValue& node, DecodeState& state,
                                     const std::string& path, DecodedComposition& out) {
    static constexpr std::array<std::string_view, 7> kKeys{
        "id", "name", "duration", "format", "parameters", "animationCurves", "graph"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    if (!decodeObjectId(*members[0], state, joinPath(path, "id"), out.id)) {
        return false;
    }

    std::string_view nameText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "name"), nameText)) {
        return false;
    }
    out.name = std::string(nameText);

    if (!decodeDuration(*members[2], state, joinPath(path, "duration"), out.duration)) {
        return false;
    }
    if (!decodeFormat(*members[3], state, joinPath(path, "format"), out.format)) {
        return false;
    }

    return detail::decodeCompositionInterior(
        *members[4], *members[5], *members[6], state, joinPath(path, "parameters"),
        joinPath(path, "animationCurves"), joinPath(path, "graph"), out.parameters,
        out.animationCurves, out.graph);
}

[[nodiscard]] bool decodeLocator(const JsonValue& node, DecodeState& state, const std::string& path,
                                 OcioConfigLocator& out) {
    std::string_view kindText;
    if (!decodeKindDiscriminator(node, state, path, kindText)) {
        return false;
    }

    std::string_view secondKey;
    if (kindText == "builtin" || kindText == "external-ocioz" || kindText == "external-config") {
        secondKey = "uri";
    } else if (kindText == "project-relative-ocioz") {
        secondKey = "path";
    } else {
        state.fail(DocumentDecodeError::InvalidOcioLocatorKind, joinPath(path, "kind"));
        return false;
    }

    const std::array<std::string_view, 2> keys{"kind", secondKey};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, keys, true, state, path, members)) {
        return false;
    }

    std::string_view valueText;
    if (!decodeStringMember(*members[1], state, joinPath(path, secondKey), valueText)) {
        return false;
    }

    if (kindText == "builtin") {
        out = document::BuiltInOcioConfigLocator{std::string(valueText)};
    } else if (kindText == "project-relative-ocioz") {
        out = document::ProjectRelativeOciozLocator{std::string(valueText)};
    } else if (kindText == "external-ocioz") {
        out = document::ExternalOciozLocator{std::string(valueText)};
    } else {
        out = document::ExternalOcioConfigLocator{std::string(valueText)};
    }
    return true;
}

[[nodiscard]] bool decodeExpectedRevision(const JsonValue& node, DecodeState& state,
                                          const std::string& path, OcioConfigRevision& out) {
    static constexpr std::array<std::string_view, 2> kKeys{"algorithm", "digest"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    std::string_view algorithmText;
    if (!decodeStringMember(*members[0], state, joinPath(path, "algorithm"), algorithmText)) {
        return false;
    }
    out.algorithm =
        algorithmText == "sha256" ? OcioRevisionAlgorithm::Sha256 : OcioRevisionAlgorithm::Unknown;

    const auto digestPath = joinPath(path, "digest");
    std::string_view digestText;
    if (!decodeStringMember(*members[1], state, digestPath, digestText)) {
        return false;
    }
    const auto digest = core::Sha256Digest::fromLowercaseHex(digestText);
    if (!digest.has_value()) {
        state.fail(DocumentDecodeError::InvalidDigestSpelling, digestPath);
        return false;
    }
    out.digest = *digest;
    return true;
}

[[nodiscard]] bool decodeContextVariables(const JsonValue& node, DecodeState& state,
                                          const std::string& path,
                                          std::vector<OcioContextVariable>& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    static constexpr std::array<std::string_view, 2> kKeys{"name", "value"};
    out.clear();
    const auto elements = node.arrayElements();
    out.reserve(elements.size());
    for (std::size_t index = 0; index < elements.size(); ++index) {
        const auto elementPath = joinPathIndex(path, index);
        std::vector<const JsonValue*> members;
        if (!matchOrderedMembers(elements[index], kKeys, true, state, elementPath, members)) {
            return false;
        }
        std::string_view nameText;
        if (!decodeStringMember(*members[0], state, joinPath(elementPath, "name"), nameText)) {
            return false;
        }
        std::string_view valueText;
        if (!decodeStringMember(*members[1], state, joinPath(elementPath, "value"), valueText)) {
            return false;
        }
        out.push_back({std::string(nameText), std::string(valueText)});
    }
    return true;
}

[[nodiscard]] bool decodeOcioConfig(const JsonValue& node, DecodeState& state,
                                    const std::string& path, OcioConfigReference& out) {
    static constexpr std::array<std::string_view, 5> kKeys{
        "schemaVersion", "locator", "expectedRevision", "portability", "contextVariables"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    if (!decodeSchemaVersionField(*members[0], state, joinPath(path, "schemaVersion"),
                                  out.schemaVersion)) {
        return false;
    }
    if (!decodeLocator(*members[1], state, joinPath(path, "locator"), out.locator)) {
        return false;
    }
    if (!decodeExpectedRevision(*members[2], state, joinPath(path, "expectedRevision"),
                                out.expectedRevision)) {
        return false;
    }

    const auto portabilityPath = joinPath(path, "portability");
    std::string_view portabilityText;
    if (!decodeStringMember(*members[3], state, portabilityPath, portabilityText)) {
        return false;
    }
    if (portabilityText == "builtin") {
        out.portability = OcioConfigPortability::BuiltIn;
    } else if (portabilityText == "project-relative") {
        out.portability = OcioConfigPortability::ProjectRelative;
    } else if (portabilityText == "external") {
        out.portability = OcioConfigPortability::External;
    } else {
        out.portability = OcioConfigPortability::Unknown;
    }

    return decodeContextVariables(*members[4], state, joinPath(path, "contextVariables"),
                                  out.contextVariables);
}

[[nodiscard]] bool decodeColorSettings(const JsonValue& node, DecodeState& state,
                                       const std::string& path, ColorSettings& out) {
    static constexpr std::array<std::string_view, 3> kKeys{"schemaVersion", "processColorSpaceId",
                                                           "ocioConfig"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    if (!decodeSchemaVersionField(*members[0], state, joinPath(path, "schemaVersion"),
                                  out.schemaVersion)) {
        return false;
    }

    std::string_view processColorSpaceIdText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "processColorSpaceId"),
                            processColorSpaceIdText)) {
        return false;
    }
    out.processColorSpaceId = std::string(processColorSpaceIdText);

    if (!decodeOcioConfig(*members[2], state, joinPath(path, "ocioConfig"), out.ocioConfig)) {
        return false;
    }

    const auto validation = out.validate();
    if (!validation.ok()) {
        const auto& issue = validation.issues().front();
        state.fail(DocumentDecodeError::DomainViolation,
                   joinPath(path, translateValidationPath(issue.path)));
        return false;
    }
    return true;
}

[[nodiscard]] bool decodeProject(const JsonValue& node, DecodeState& state, const std::string& path,
                                 DecodedDocumentEnvelope& out) {
    static constexpr std::array<std::string_view, 4> kKeys{"id", "name", "colorSettings",
                                                           "compositions"};
    std::vector<const JsonValue*> members;
    if (!matchOrderedMembers(node, kKeys, true, state, path, members)) {
        return false;
    }

    if (!decodeObjectId(*members[0], state, joinPath(path, "id"), out.projectId)) {
        return false;
    }

    std::string_view nameText;
    if (!decodeStringMember(*members[1], state, joinPath(path, "name"), nameText)) {
        return false;
    }
    out.projectName = std::string(nameText);

    if (!decodeColorSettings(*members[2], state, joinPath(path, "colorSettings"),
                             out.colorSettings)) {
        return false;
    }

    const auto compositionsPath = joinPath(path, "compositions");
    if (members[3]->kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, compositionsPath);
        return false;
    }
    const auto elements = members[3]->arrayElements();
    out.compositions.clear();
    out.compositions.reserve(elements.size());
    std::uint64_t previousId = 0;
    bool hasPrevious = false;
    for (std::size_t index = 0; index < elements.size(); ++index) {
        DecodedComposition composition;
        if (!decodeComposition(elements[index], state, joinPathIndex(compositionsPath, index),
                               composition)) {
            return false;
        }
        const auto currentId = composition.id.value();
        if (hasPrevious) {
            if (currentId == previousId) {
                state.fail(DocumentDecodeError::DuplicateComposition,
                           joinPath(joinPathIndex(compositionsPath, index), "id"));
                return false;
            }
            if (currentId < previousId) {
                state.fail(DocumentDecodeError::UnsortedCompositions,
                           joinPath(joinPathIndex(compositionsPath, index), "id"));
                return false;
            }
        }
        previousId = currentId;
        hasPrevious = true;
        out.compositions.push_back(std::move(composition));
    }
    return true;
}

} // namespace

DocumentDecodePathText DocumentDecodePathText::from(const std::string_view text) noexcept {
    DocumentDecodePathText result;
    const auto copySize = std::min(text.size(), result.chars_.size());
    for (std::size_t index = 0; index < copySize; ++index) {
        result.chars_[index] = text[index];
    }
    result.size_ = static_cast<std::uint16_t>(copySize);
    result.truncated_ = text.size() > copySize;
    return result;
}

DocumentDecodeResult DocumentDecodeResult::success(DecodedDocumentEnvelope envelope) {
    DocumentDecodeResult result;
    result.envelope_ = std::move(envelope);
    result.error_ = DocumentDecodeError::None;
    return result;
}

DocumentDecodeResult DocumentDecodeResult::failure(const DocumentDecodeError error,
                                                   const std::string_view path) {
    DocumentDecodeResult result;
    result.error_ = error;
    result.path_ = DocumentDecodePathText::from(path);
    return result;
}

DocumentDecodeResult decodeDocumentEnvelope(const JsonValue& root) {
    static constexpr std::array<std::string_view, 4> kKeys{"schemaVersion", "project",
                                                           "idAllocation", "extensions"};
    detail::DecodeState state;
    std::vector<const JsonValue*> members;
    if (!detail::matchOrderedMembers(root, kKeys, true, state, std::string{}, members)) {
        return DocumentDecodeResult::failure(state.error, state.path);
    }

    document::SchemaVersion schemaVersion;
    if (!decodeSchemaVersionField(*members[0], state, "/schemaVersion", schemaVersion)) {
        return DocumentDecodeResult::failure(state.error, state.path);
    }
    if (schemaVersion != kCanonicalDocumentSchemaVersionV1) {
        return DocumentDecodeResult::failure(DocumentDecodeError::DomainViolation,
                                             "/schemaVersion");
    }

    DecodedDocumentEnvelope envelope;
    if (!decodeProject(*members[1], state, "/project", envelope)) {
        return DocumentDecodeResult::failure(state.error, state.path);
    }

    if (members[2]->kind() != JsonValueKind::Object) {
        return DocumentDecodeResult::failure(DocumentDecodeError::WrongValueKind, "/idAllocation");
    }
    if (members[3]->kind() != JsonValueKind::Array) {
        return DocumentDecodeResult::failure(DocumentDecodeError::WrongValueKind, "/extensions");
    }

    return DocumentDecodeResult::success(std::move(envelope));
}

} // namespace bloom::project
