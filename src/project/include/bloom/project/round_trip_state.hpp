#pragma once

#include <bloom/project/unknown_json_number.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The move-only owner of unknown-additive-member round-trip data for a newer-minor (schema
// {1, minor>0}) document.json (see docs/architecture/project-format.md, "Versions, Migrations,
// And Preservation"). RoundTripState is Project I/O's exclusively: it never enters
// bloom::document::Document or any live authoring type, and an exact-{1,0} document never
// produces one. This package (RT1) only captures and classifies; the writer-side overlay that
// reconciles RoundTripState back onto a canonical rewrite is a later slice, so RoundTripState
// today is read/inspected but never consumed by a writer.
//
// One retained value is keyed by its attachment point:
//  - a singleton object (root, project, colorSettings, ocioConfig, expectedRevision, locator,
//    format, layerStack, idAllocation/highestIssued, referencePolicy, ...) attaches by schema
//    path -- a sequence of named segments from the document root.
//  - a collection element (composition, parameter, animation curve, keyframe, node, edge,
//    extension record, a node's parameter binding, a Layer Output boundary, a Layer Stack entry,
//    a host reference-table entry) attaches by the contract's fixed stable identity for that
//    collection, never array position (see "Versions, Migrations, And Preservation": "Array
//    reconciliation identities are fixed").
//
// A full attachment path threads both kinds of segment from the root, e.g. the retained members
// of one composition's `format` object are keyed by
// [Composition:"7", Named:"format"] -- so the same retained data survives a document edit that
// reorders or renumbers everything except that composition's own identity.
namespace bloom::project {

// The fixed collection identity kinds this package's decoder retains unknown members for (see
// docs/architecture/project-format.md, "Array reconciliation identities are fixed"). Requirement
// provider/capability pairs are a manifest-level identity outside document.json and are not a
// member of this enum.
enum class RoundTripCollectionKind : std::uint8_t {
    Composition,
    Parameter,
    AnimationCurve,
    Keyframe,
    Node,
    Edge,
    ExtensionRecord,
    ParameterBinding, // identity: role, scoped to its owning node
    LayerOutput,      // identity: LayerId
    LayerStackEntry,  // identity: LayerSlotId
    HostReference,    // identity: UTF-8 key, scoped to its owning extension record
};

// One segment of an attachment path: either a named singleton member (schema path) or one
// collection element identified by its fixed stable identity spelling (the same canonical
// decimal-string/role/key text the wire format uses, so it never depends on decode-time C++
// type representation).
class RoundTripPathSegment final {
  public:
    [[nodiscard]] static RoundTripPathSegment named(std::string name);
    [[nodiscard]] static RoundTripPathSegment collectionElement(RoundTripCollectionKind kind,
                                                                std::string identity);

    [[nodiscard]] bool isCollectionElement() const noexcept { return isCollectionElement_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] RoundTripCollectionKind collectionKind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& identity() const noexcept { return identity_; }

    friend bool operator==(const RoundTripPathSegment&, const RoundTripPathSegment&) = default;

  private:
    RoundTripPathSegment() = default;

    std::string name_;
    std::string identity_;
    RoundTripCollectionKind kind_{};
    bool isCollectionElement_ = false;
};

using RoundTripAttachmentPath = std::vector<RoundTripPathSegment>;

enum class RetainedJsonValueKind : std::uint8_t {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

class RetainedJsonValue;
class RetainedJsonMember;

// A bounded, owned copy of one JSON value reachable from an unknown additive member (see
// bloom::project::JsonValue, whose shape this mirrors). Strings are the decoded Unicode scalar
// sequence, exactly like JsonValue::asString(); numbers are the lossless typed subset
// (UnknownJsonNumber), never a raw source token, so canonical rewriting reproduces the same
// mathematical value and canonical spelling without retaining a general raw-number spelling (see
// docs/architecture/project-format.md, "Unknown JSON Numbers"). Object member order is preserved
// exactly as decoded: unlike the trailing-capture ordering rule applied to a *known* schema
// object's unrecognized members, a value nested inside an already-unknown member is opaque
// content with no schema of its own, so this package does not re-sort it. Mirrors
// JsonValue/JsonMember's own mutual-recursion shape (bloom/project/strict_json_dom.hpp): a
// std::vector member only needs its element type complete at first use, not at class-body
// parse time, so RetainedJsonValue can hold `std::vector<RetainedJsonMember>` directly and
// RetainedJsonMember can hold a `RetainedJsonValue` directly.
class RetainedJsonValue final {
  public:
    // Defined out-of-line in round_trip_state.cpp (never inline in the class body): every one of
    // these constructors implicitly default-initializes `members_`
    // (std::vector<RetainedJsonMember>) as part of establishing the rest of the object, and a
    // function *body* written inside the class is compiled in "complete-class context" as if
    // placed immediately after this class's own closing brace -- at which point
    // RetainedJsonMember (defined later in this same header) is still only forward-declared. Only
    // a `= default` special member gets the standard's later "instantiated at first odr-use"
    // deferral (see this class's copy/move/dtor declarations below, and JsonValue/JsonMember's
    // identical mutual-recursion shape in strict_json_dom.hpp, whose own `~JsonValue() = default;`
    // relies on exactly that deferral); an ordinary hand-written body does not get it.
    RetainedJsonValue() noexcept;
    explicit RetainedJsonValue(bool value) noexcept;
    explicit RetainedJsonValue(UnknownJsonNumber value) noexcept;
    explicit RetainedJsonValue(std::string value);

    [[nodiscard]] static RetainedJsonValue makeArray(std::vector<RetainedJsonValue> elements);
    [[nodiscard]] static RetainedJsonValue makeObject(std::vector<RetainedJsonMember> members);

    RetainedJsonValue(const RetainedJsonValue&);
    RetainedJsonValue& operator=(const RetainedJsonValue&);
    RetainedJsonValue(RetainedJsonValue&&) noexcept;
    RetainedJsonValue& operator=(RetainedJsonValue&&) noexcept;
    ~RetainedJsonValue();

    [[nodiscard]] RetainedJsonValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool asBoolean() const noexcept { return boolean_; }
    // Valid only when kind() == Number; terminates otherwise rather than dereferencing an empty
    // optional (so misuse is a defined, diagnosable abort, not UB). The explicit has_value()
    // guard, not just a documented precondition, is what static analysis needs to treat the
    // dereference below as checked.
    [[nodiscard]] const UnknownJsonNumber& asNumber() const noexcept {
        if (number_.has_value()) {
            return *number_;
        }
        std::terminate();
    }
    [[nodiscard]] const std::string& asString() const noexcept { return text_; }
    [[nodiscard]] const std::vector<RetainedJsonValue>& elements() const noexcept {
        return elements_;
    }
    [[nodiscard]] const std::vector<RetainedJsonMember>& members() const noexcept;
    // Linear lookup by decoded key among this object's retained members; nullptr when kind() !=
    // Object or the key is absent.
    [[nodiscard]] const RetainedJsonValue* findMember(std::string_view key) const noexcept;

    friend bool operator==(const RetainedJsonValue&, const RetainedJsonValue&);

  private:
    RetainedJsonValueKind kind_;
    bool boolean_ = false;
    // UnknownJsonNumber deliberately has no default constructor of its own (every instance must
    // name a real parsed number; see unknown_json_number.hpp), so this holder uses optional<T>
    // rather than asking that class to grow a placeholder state just for this one caller.
    std::optional<UnknownJsonNumber> number_;
    std::string text_;
    std::vector<RetainedJsonValue> elements_;
    std::vector<RetainedJsonMember> members_;
};

// One retained member of a retained JSON object: its exact decoded key plus its retained value.
class RetainedJsonMember final {
  public:
    RetainedJsonMember(std::string key, RetainedJsonValue value)
        : key_(std::move(key)), value_(std::move(value)) {}

    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] const RetainedJsonValue& value() const noexcept { return value_; }

    friend bool operator==(const RetainedJsonMember&, const RetainedJsonMember&) = default;

  private:
    std::string key_;
    RetainedJsonValue value_;
};

// The move-only container of every retained attachment point decoded from one newer-minor
// document. Never copyable: a RoundTripState is owned by exactly one decode/open result at a
// time and is never implicitly duplicated.
class RoundTripState final {
  public:
    struct Entry final {
        RoundTripAttachmentPath path;
        std::vector<RetainedJsonMember> members; // already in ascending UTF-8 key order

        friend bool operator==(const Entry&, const Entry&) = default;
    };

    RoundTripState() = default;
    RoundTripState(const RoundTripState&) = delete;
    RoundTripState& operator=(const RoundTripState&) = delete;
    RoundTripState(RoundTripState&&) noexcept = default;
    RoundTripState& operator=(RoundTripState&&) noexcept = default;
    ~RoundTripState() = default;

    // Records one attachment point's retained trailing members. `members` must already be in
    // ascending UTF-8 key order (the decoder enforces this before calling); `path` must not
    // already have an entry (each attachment point is visited at most once per decode).
    void attach(RoundTripAttachmentPath path, std::vector<RetainedJsonMember> members);

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

    // Linear lookup by exact attachment path; nullptr when no entry was attached there.
    [[nodiscard]] const std::vector<RetainedJsonMember>*
    find(const RoundTripAttachmentPath& path) const noexcept;

  private:
    std::vector<Entry> entries_;
};

} // namespace bloom::project
