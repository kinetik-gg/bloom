#include <bloom/project/round_trip_state.hpp>

#include <algorithm>
#include <utility>

namespace bloom::project {

RoundTripPathSegment RoundTripPathSegment::named(std::string name) {
    RoundTripPathSegment segment;
    segment.name_ = std::move(name);
    segment.isCollectionElement_ = false;
    return segment;
}

RoundTripPathSegment RoundTripPathSegment::collectionElement(const RoundTripCollectionKind kind,
                                                             std::string identity) {
    RoundTripPathSegment segment;
    segment.kind_ = kind;
    segment.identity_ = std::move(identity);
    segment.isCollectionElement_ = true;
    return segment;
}

RetainedJsonValue::RetainedJsonValue() noexcept : kind_(RetainedJsonValueKind::Null) {}

RetainedJsonValue::RetainedJsonValue(const bool value) noexcept
    : kind_(RetainedJsonValueKind::Boolean), boolean_(value) {}

RetainedJsonValue::RetainedJsonValue(const UnknownJsonNumber value) noexcept
    : kind_(RetainedJsonValueKind::Number), number_(value) {}

RetainedJsonValue::RetainedJsonValue(std::string value)
    : kind_(RetainedJsonValueKind::String), text_(std::move(value)) {}

RetainedJsonValue::RetainedJsonValue(const RetainedJsonValue&) = default;
RetainedJsonValue& RetainedJsonValue::operator=(const RetainedJsonValue&) = default;
RetainedJsonValue::RetainedJsonValue(RetainedJsonValue&&) noexcept = default;
RetainedJsonValue& RetainedJsonValue::operator=(RetainedJsonValue&&) noexcept = default;
RetainedJsonValue::~RetainedJsonValue() = default;

RetainedJsonValue RetainedJsonValue::makeArray(std::vector<RetainedJsonValue> elements) {
    RetainedJsonValue value;
    value.kind_ = RetainedJsonValueKind::Array;
    value.elements_ = std::move(elements);
    return value;
}

RetainedJsonValue RetainedJsonValue::makeObject(std::vector<RetainedJsonMember> members) {
    RetainedJsonValue value;
    value.kind_ = RetainedJsonValueKind::Object;
    value.members_ = std::move(members);
    return value;
}

const std::vector<RetainedJsonMember>& RetainedJsonValue::members() const noexcept {
    return members_;
}

const RetainedJsonValue* RetainedJsonValue::findMember(const std::string_view key) const noexcept {
    if (kind_ != RetainedJsonValueKind::Object) {
        return nullptr;
    }
    for (const auto& member : members_) {
        if (member.key() == key) {
            return &member.value();
        }
    }
    return nullptr;
}

bool operator==(const RetainedJsonValue& lhs, const RetainedJsonValue& rhs) {
    if (lhs.kind() != rhs.kind()) {
        return false;
    }
    switch (lhs.kind()) {
    case RetainedJsonValueKind::Null:
        return true;
    case RetainedJsonValueKind::Boolean:
        return lhs.asBoolean() == rhs.asBoolean();
    case RetainedJsonValueKind::Number:
        return lhs.asNumber() == rhs.asNumber();
    case RetainedJsonValueKind::String:
        return lhs.asString() == rhs.asString();
    case RetainedJsonValueKind::Array:
        return lhs.elements() == rhs.elements();
    case RetainedJsonValueKind::Object:
        return lhs.members() == rhs.members();
    }
    return false;
}

void RoundTripState::attach(RoundTripAttachmentPath path, std::vector<RetainedJsonMember> members) {
    entries_.push_back(Entry{std::move(path), std::move(members)});
}

const std::vector<RetainedJsonMember>*
RoundTripState::find(const RoundTripAttachmentPath& path) const noexcept {
    const auto it =
        std::ranges::find_if(entries_, [&path](const Entry& entry) { return entry.path == path; });
    if (it == entries_.end()) {
        return nullptr;
    }
    return &it->members;
}

} // namespace bloom::project
