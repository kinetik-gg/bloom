#pragma once

#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/project_io_memory_resource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The bounded strict JSON DOM: a reader-side counterpart to the canonical JSON writer described
// in docs/architecture/project-format.md ("Canonical JSON Primitives" and "Versions, Migrations,
// And Preservation"). `parseStrictJsonDom()` first runs the existing bounded
// `bloom::project::detail::preflightStrictJson()` scan, then parses the accepted bytes with a
// qualified yyjson 0.12 reader whose allocation is bound to the caller's
// `ProjectIoOperationMemory` budget. No yyjson type, header, or macro appears here or in any
// other public Bloom contract; yyjson stays an implementation detail of strict_json_dom.cpp. The
// resulting tree is a move-only, Bloom-owned value type: object member order is preserved exactly
// as decoded, decoded duplicate object keys are rejected (comparing decoded Unicode strings, so
// `"a"` and `"a"` collide), and every JSON number keeps its exact source token untouched --
// typed numeric conversion (canonical decimal/rational/Float64, or the lossless unknown-number
// subset) remains the caller's responsibility using the existing primitives in
// canonical_decimal.hpp and unknown_json_number.hpp.

namespace bloom::project {

namespace detail {
class StrictJsonDomBuilder;
} // namespace detail

enum class JsonValueKind : std::uint8_t {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

class JsonValue;
class JsonMember;

// One decoded JSON value in the bounded strict DOM. Copying is disabled so a large tree is never
// accidentally duplicated; every value moves. All owned storage (the number/string token, the
// element array, and the member array) allocates through the same PMR resource as its owning
// document, in turn bounded by the caller's `ProjectIoOperationMemory` budget.
class JsonValue final {
  public:
    // Constructs a Null value bound to `resource`; every other kind is populated by the parse
    // pipeline. Public so standard containers (which are not implicitly friends even when called
    // from friended code) can construct DOM nodes in place.
    explicit JsonValue(std::pmr::memory_resource* resource) noexcept;

    JsonValue(const JsonValue&) = delete;
    JsonValue& operator=(const JsonValue&) = delete;
    // Every value reachable from one document shares the same PMR resource pointer, so the
    // underlying containers' allocators always compare equal and moving never allocates, copies
    // element-wise, or throws.
    JsonValue(JsonValue&&) noexcept = default;
    JsonValue& operator=(JsonValue&&) noexcept = default;
    ~JsonValue() = default;

    [[nodiscard]] JsonValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool isNull() const noexcept { return kind_ == JsonValueKind::Null; }

    // Valid only when kind() == Boolean; nullopt otherwise.
    [[nodiscard]] std::optional<bool> asBoolean() const noexcept;
    // Valid only when kind() == Number. Returns the exact source token bytes (e.g. "-0", "1e10",
    // "9007199254740992.0"); the DOM performs no numeric conversion or normalization.
    [[nodiscard]] std::optional<std::string_view> asNumberToken() const noexcept;
    // Valid only when kind() == String. Returns the decoded Unicode scalar sequence (escapes
    // already resolved), never the original escaped spelling.
    [[nodiscard]] std::optional<std::string_view> asString() const noexcept;
    // Valid only when kind() == Array; empty otherwise.
    [[nodiscard]] std::span<const JsonValue> arrayElements() const noexcept;
    // Valid only when kind() == Object; empty otherwise. Members retain exact source order.
    [[nodiscard]] std::span<const JsonMember> objectMembers() const noexcept;
    // Linear lookup by decoded key; returns nullptr when kind() != Object or the key is absent.
    [[nodiscard]] const JsonValue* findMember(std::string_view key) const noexcept;

  private:
    friend class detail::StrictJsonDomBuilder;

    JsonValueKind kind_ = JsonValueKind::Null;
    bool boolean_ = false;
    std::pmr::string text_;
    std::pmr::vector<JsonValue> elements_;
    std::pmr::vector<JsonMember> members_;
};

// One decoded object member in source order: a decoded key plus its owned value.
class JsonMember final {
  public:
    // Constructs a member bound to `resource` with decoded `key` and a Null placeholder value;
    // the parse pipeline fills in the value in place. Public for the same container-construction
    // reason as JsonValue's constructor above. Deliberately not noexcept: copying `key` into
    // PMR-owned storage allocates through ProjectIoMemoryResource's throwing
    // std::pmr::memory_resource surface (do_allocate), which reports budget rejection as
    // std::bad_alloc rather than a null pointer. That exception must reach
    // parseStrictJsonDom()'s catch clause rather than hit a noexcept boundary and terminate.
    JsonMember(std::pmr::memory_resource* resource, std::string_view key);

    JsonMember(const JsonMember&) = delete;
    JsonMember& operator=(const JsonMember&) = delete;
    JsonMember(JsonMember&&) noexcept = default;
    JsonMember& operator=(JsonMember&&) noexcept = default;
    ~JsonMember() = default;

    [[nodiscard]] std::string_view key() const noexcept { return key_; }
    [[nodiscard]] const JsonValue& value() const noexcept { return value_; }

  private:
    friend class detail::StrictJsonDomBuilder;

    std::pmr::string key_;
    JsonValue value_;
};

inline constexpr std::size_t kStrictJsonDomMaximumInputBytes = 256U << 20U;
inline constexpr std::uint32_t kStrictJsonDomMaximumDepth = 128;
inline constexpr std::uint64_t kStrictJsonDomMaximumValues = 4'000'000;
inline constexpr std::uint64_t kStrictJsonDomMaximumContainerEntries = 1'000'000;
inline constexpr std::uint64_t kStrictJsonDomMaximumDecodedStringBytes = 89'478'488;

// Bounds applied by the preceding preflight scan and carried through to the DOM build. A field
// above the fixed v1 ceiling (mirroring strict_json_preflight.hpp) is rejected as InvalidLimits;
// callers only ever lower these budgets.
struct StrictJsonDomLimits final {
    std::size_t maximumInputBytes = kStrictJsonDomMaximumInputBytes;
    std::uint64_t maximumValues = kStrictJsonDomMaximumValues;
    std::uint64_t maximumContainerEntries = kStrictJsonDomMaximumContainerEntries;
    std::uint32_t maximumDepth = kStrictJsonDomMaximumDepth;
    std::uint64_t maximumDecodedStringBytes = kStrictJsonDomMaximumDecodedStringBytes;
};

enum class StrictJsonDomError : std::uint8_t {
    None,
    InvalidLimits,
    InputTooLarge,
    BomForbidden,
    EmptyInput,
    InvalidUtf8,
    InvalidSyntax,
    InvalidEscape,
    InvalidUnicodeScalar,
    InvalidNumber,
    TrailingData,
    DepthLimitExceeded,
    ValueLimitExceeded,
    ContainerEntryLimitExceeded,
    DecodedStringLimitExceeded,
    SizeOverflow,
    // A decoded object key collided with an earlier decoded key in the same object; memberPath()
    // names the exact offending member.
    DuplicateObjectKey,
    // The bounded PMR budget rejected an allocation needed by the qualified reader or the DOM
    // build; the destination document is never constructed.
    ResourceExhausted,
    // The qualified reader rejected input that the preflight scan accepted; this should not occur
    // given a conforming preflight/reader pair and is reported rather than trusted blindly.
    ParseFailed,
};

// A bounded diagnostic path to one JSON member, formatted as `/key/0/key`. Truncated() is true
// when the exact path did not fit; the retained prefix is still a genuine path prefix.
class StrictJsonDomPathText final {
  public:
    StrictJsonDomPathText() noexcept = default;

    [[nodiscard]] std::string_view view() const& noexcept { return {chars_.data(), size_}; }
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  private:
    friend class detail::StrictJsonDomBuilder;

    std::array<char, 512> chars_{};
    std::uint16_t size_ = 0;
    bool truncated_ = false;
};

// A parsed document owning its PMR resource and root value for as long as the document lives.
// The resource lives behind a stable heap address so moving the document never invalidates the
// pointers held by its PMR-allocated tree.
class StrictJsonDomDocument final {
  public:
    StrictJsonDomDocument(const StrictJsonDomDocument&) = delete;
    StrictJsonDomDocument& operator=(const StrictJsonDomDocument&) = delete;
    StrictJsonDomDocument(StrictJsonDomDocument&&) noexcept = default;
    StrictJsonDomDocument& operator=(StrictJsonDomDocument&&) noexcept = default;
    ~StrictJsonDomDocument() = default;

    [[nodiscard]] const JsonValue& root() const noexcept { return root_; }

  private:
    friend class detail::StrictJsonDomBuilder;

    StrictJsonDomDocument(std::unique_ptr<ProjectIoMemoryResource> resource,
                          JsonValue root) noexcept;

    std::unique_ptr<ProjectIoMemoryResource> resource_;
    JsonValue root_;
};

// [[nodiscard]] failure-aware result. On success, document() names the parsed tree; on failure,
// error() names the typed cause with either byteOffset() (preflight/reader-stage failures, an
// RFC 8259 byte position) or memberPath() (DuplicateObjectKey) populated.
class [[nodiscard]] StrictJsonDomResult final {
  public:
    StrictJsonDomResult(const StrictJsonDomResult&) = delete;
    StrictJsonDomResult& operator=(const StrictJsonDomResult&) = delete;
    StrictJsonDomResult(StrictJsonDomResult&&) noexcept = default;
    StrictJsonDomResult& operator=(StrictJsonDomResult&&) noexcept = default;
    ~StrictJsonDomResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == StrictJsonDomError::None;
    }
    [[nodiscard]] StrictJsonDomError error() const noexcept { return error_; }
    [[nodiscard]] std::size_t byteOffset() const noexcept { return byteOffset_; }
    [[nodiscard]] std::string_view memberPath() const noexcept { return path_.view(); }
    [[nodiscard]] const StrictJsonDomDocument* document() const& noexcept {
        return document_.has_value() ? &*document_ : nullptr;
    }
    [[nodiscard]] const StrictJsonDomDocument* document() const&& = delete;

  private:
    friend class detail::StrictJsonDomBuilder;

    StrictJsonDomResult() noexcept = default;

    std::optional<StrictJsonDomDocument> document_;
    StrictJsonDomError error_ = StrictJsonDomError::InvalidLimits;
    std::size_t byteOffset_ = 0;
    StrictJsonDomPathText path_;
};

// Runs the bounded preflight scan with `limits`, then parses the accepted bytes into a bounded
// Bloom-owned DOM. `operation` supplies and bounds every allocation made on this call's behalf,
// including the qualified reader's internal working memory; a budget rejection is reported as
// ResourceExhausted rather than throwing or falling back to unmetered heap allocation. Never
// throws.
[[nodiscard]] StrictJsonDomResult parseStrictJsonDom(std::span<const std::byte> input,
                                                     const StrictJsonDomLimits& limits,
                                                     ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
