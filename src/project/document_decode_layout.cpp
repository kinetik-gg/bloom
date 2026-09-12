#include "document_decode_internal.hpp"

#include <bloom/project/canonical_decimal.hpp>

#include <array>
#include <string>
#include <vector>

namespace bloom::project::detail {
namespace {
bool number(const JsonValue& node, DecodeState& state, const std::string& path, double& out) {
    const auto token = node.asNumberToken();
    if (!token) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    const auto value = parseKnownFloat64(*token);
    if (!value) {
        state.fail(DocumentDecodeError::InvalidFloat64, path);
        return false;
    }
    out = *value.value();
    return true;
}

bool boolean(const JsonValue& node, DecodeState& state, const std::string& path, bool& out) {
    const auto value = node.asBoolean();
    if (!value) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    out = *value;
    return true;
}
} // namespace

bool decodeNodeLayout(const JsonValue& node, DecodeState& state, const std::string& path,
                      document::NodeLayout& out) {
    if (node.kind() != JsonValueKind::Array) {
        state.fail(DocumentDecodeError::WrongValueKind, path);
        return false;
    }
    document::NodeId previous;
    std::size_t index = 0;
    for (const auto& element : node.arrayElements()) {
        const auto recordPath = joinPathIndex(path, index++);
        static constexpr std::array<std::string_view, 5> keys{"nodeId", "position", "width",
                                                              "collapsed", "muted"};
        std::vector<const JsonValue*> members;
        std::vector<RetainedJsonMember> trailing;
        if (!matchOrderedMembers(element, keys, true, state, recordPath, members, trailing))
            return false;
        document::NodeId id;
        if (!decodeObjectId(*members[0], state, joinPath(recordPath, "nodeId"), id))
            return false;
        if (id <= previous) {
            state.fail(DocumentDecodeError::DomainViolation, joinPath(recordPath, "nodeId"));
            return false;
        }
        previous = id;
        const AttachmentScope recordScope(state, RoundTripCollectionKind::NodeLayout,
                                          std::to_string(id.value()));
        if (!trailing.empty() && state.roundTrip)
            state.roundTrip->attach(state.attachmentPath, std::move(trailing));
        document::NodeLayoutRecord record;
        {
            const AttachmentScope positionScope(state, "position");
            static constexpr std::array<std::string_view, 2> positionKeys{"x", "y"};
            std::vector<const JsonValue*> position;
            const auto positionPath = joinPath(recordPath, "position");
            if (!matchOrderedMembers(*members[1], positionKeys, true, state, positionPath,
                                     position) ||
                !number(*position[0], state, joinPath(positionPath, "x"), record.position.x) ||
                !number(*position[1], state, joinPath(positionPath, "y"), record.position.y))
                return false;
        }
        if (!number(*members[2], state, joinPath(recordPath, "width"), record.width) ||
            !boolean(*members[3], state, joinPath(recordPath, "collapsed"), record.collapsed) ||
            !boolean(*members[4], state, joinPath(recordPath, "muted"), record.muted))
            return false;
        if (record.width <= 0.0) {
            state.fail(DocumentDecodeError::DomainViolation, joinPath(recordPath, "width"));
            return false;
        }
        out.emplace(id, record);
    }
    return true;
}
} // namespace bloom::project::detail
